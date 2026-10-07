// Exercises the production transfer and Client against the real helper process.
// Torrent reads provide deterministic verified bytes; no external swarm is used.
#include "download_transfer.h"
#include "download_writer/client.hpp"
#include "http.h"
#include "torrent/engine.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <spawn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace fs = std::filesystem;
namespace wire = download_writer::wire;
using namespace std::chrono_literals;
namespace {
int checks = 0, job_number = 0;
std::string media_bytes, helper_path, helper_root;
std::atomic<int64_t> first_read{-1};
std::atomic<int> readers_opened{0}, readers_closed{0};
std::atomic<bool>* cancel_during_read = nullptr;
int64_t cancel_at = -1, error_at = -1;

enum class Fault { None, HoldWrite, DropWrite, DropInitialCheckpoint, DropFinalCheckpoint, DropClose, NoHelper };
struct Control {
    Fault fault = Fault::None;
    std::mutex mutex;
    std::condition_variable wake;
    bool held = false, release = false;
    std::atomic<bool> injected{false}, gate_timeout{false};
    std::atomic<int> launched{0}, destroyed{0};
    std::atomic<int64_t> acknowledged{0};
};
std::shared_ptr<Control> control;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
std::string bytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
int64_t checkpoint_bytes(const DownloadTransferRequest& request) {
    const auto state = json::parse(bytes(request.checkpoint_path), nullptr, false);
    return state.is_object() ? state.value("bytes", int64_t(-1)) : -1;
}

class ProcessChannel final : public download_writer::Channel {
public:
    explicit ProcessChannel(std::shared_ptr<Control> state) : state_(std::move(state)) {
        int sockets[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) throw std::runtime_error("socketpair failed");
        posix_spawn_file_actions_t actions;
        ::posix_spawn_file_actions_init(&actions);
        ::posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
        ::posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_addclose(&actions, sockets[0]);
        ::posix_spawn_file_actions_addclose(&actions, sockets[1]);
        char* arguments[]{helper_path.data(), helper_root.data(), nullptr};
        const int result = ::posix_spawn(&pid_, helper_path.c_str(), &actions, nullptr, arguments, environ);
        ::posix_spawn_file_actions_destroy(&actions);
        ::close(sockets[1]);
        if (result) { ::close(sockets[0]); throw std::runtime_error("helper spawn failed"); }
        socket_ = sockets[0];
        const timeval timeout{5, 0};
        ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        ++state_->launched;
    }
    ~ProcessChannel() override {
        ::shutdown(socket_, SHUT_RDWR);
        ::close(socket_);
        int status = 0;
        bool reaped = false;
        for (int attempt = 0; attempt < 200; ++attempt) {
            const auto result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_ || (result < 0 && errno == ECHILD)) { reaped = true; break; }
            std::this_thread::sleep_for(5ms);
        }
        if (!reaped) { ::kill(pid_, SIGKILL); while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {} }
        ++state_->destroyed;
    }
    int64_t send(const void* data, size_t count) override {
        // Headers, large data and acknowledgements all cross fragmented calls.
        const size_t wanted = std::min(count, payload_left_ ? size_t(65521) : size_t(13));
        const auto written = ::send(socket_, data, wanted, MSG_NOSIGNAL);
        if (written > 0) {
            if (payload_left_) payload_left_ -= size_t(written);
            else {
                std::memcpy(header_.data() + header_used_, data, size_t(written));
                header_used_ += size_t(written);
                if (header_used_ == sizeof(wire::Message)) {
                    std::memcpy(&request_, header_.data(), sizeof(request_));
                    header_used_ = 0;
                    payload_left_ = request_.payload_bytes;
                }
            }
        }
        return written;
    }
    int64_t receive(void* data, size_t count) override {
        if (!reply_used_) {
            size_t received = 0;
            while (received < sizeof(reply_)) {
                const auto result = ::recv(socket_, reinterpret_cast<char*>(&reply_) + received, sizeof(reply_) - received, 0);
                if (result < 0 && errno == EINTR) continue;
                if (result <= 0) return result;
                received += size_t(result);
            }
            const bool write = request_.operation == wire::Operation::write;
            const bool checkpoint = request_.operation == wire::Operation::checkpoint;
            const bool selected =
                ((state_->fault == Fault::HoldWrite || state_->fault == Fault::DropWrite) && write) ||
                (state_->fault == Fault::DropInitialCheckpoint && checkpoint && request_.offset == 0) ||
                (state_->fault == Fault::DropFinalCheckpoint && checkpoint && request_.offset == request_.total) ||
                (state_->fault == Fault::DropClose && request_.operation == wire::Operation::close);
            if (selected && !state_->injected.exchange(true)) {
                if (state_->fault != Fault::HoldWrite) return 0;
                std::unique_lock<std::mutex> lock(state_->mutex);
                state_->held = true;
                state_->wake.notify_all();
                if (!state_->wake.wait_for(lock, 5s, [&] { return state_->release; })) {
                    state_->gate_timeout = true;
                    return 0;
                }
            }
        }
        const size_t copied = std::min({count, size_t(7), sizeof(reply_) - reply_used_});
        std::memcpy(data, reinterpret_cast<const char*>(&reply_) + reply_used_, copied);
        reply_used_ += copied;
        if (reply_used_ == sizeof(reply_)) {
            if (!reply_.error && (request_.operation == wire::Operation::write || request_.operation == wire::Operation::begin))
                state_->acknowledged = reply_.offset;
            reply_used_ = 0;
        }
        return int64_t(copied);
    }
private:
    std::shared_ptr<Control> state_;
    int socket_ = -1;
    pid_t pid_ = -1;
    std::array<char, sizeof(wire::Message)> header_{};
    size_t header_used_ = 0, payload_left_ = 0, reply_used_ = 0;
    wire::Message request_{}, reply_{};
};

std::unique_ptr<download_writer::Channel> channel_factory() {
    if (control->fault == Fault::NoHelper) { errno = ECONNREFUSED; return {}; }
    return std::make_unique<ProcessChannel>(control);
}
void reset(Fault fault = Fault::None) {
    control = std::make_shared<Control>();
    control->fault = fault;
    first_read = -1;
    cancel_during_read = nullptr;
    cancel_at = error_at = -1;
}
DownloadTransferRequest request() {
    std::string name = "d" + std::string(32, '0');
    const std::string ending = std::to_string(++job_number);
    name.replace(name.size() - ending.size(), ending.size(), ending);
    DownloadTransferRequest result;
    result.work_dir = (fs::path(helper_root) / name).string();
    fs::create_directories(result.work_dir);
    result.partial_path = result.work_dir + "/media.part";
    result.checkpoint_path = result.work_dir + "/transfer.json";
    result.stream.kind = StreamKind::Torrent;
    result.stream.info_hash = "0123456789abcdef0123456789abcdef01234567";
    result.stream.file_idx = 0;
    result.stream.filename = "synthetic.mp4";
    return result;
}
void complete(const DownloadTransferRequest& item, const DownloadTransferResult& result) {
    check(result.status == DownloadTransferStatus::Complete, "complete transfer: " + result.error);
    check(result.done == int64_t(media_bytes.size()) && result.total == result.done, "completion reports the exact size");
    check(bytes(item.partial_path) == media_bytes, "helper output matches every byte, including its unaligned tail");
    check(checkpoint_bytes(item) == result.done, "completion has a durable full-file checkpoint");
    check(control->launched == 1 && control->destroyed == 1, "the transfer owns and destroys exactly one helper channel");
}
void resume(const DownloadTransferRequest& item, int64_t expected) {
    reset();
    std::atomic<bool> cancel{false};
    const auto result = download_transfer(item, {}, cancel);
    complete(item, result);
    check(first_read == expected, "resume starts exactly at the acknowledged durable checkpoint");
}
} // namespace

namespace bt {
struct Torrent {};
Engine& Engine::get() { static Engine engine; return engine; }
void Engine::start(const std::string&, const std::vector<std::string>&) {}
Stats Engine::stats(const std::string&) { Stats result; result.found = result.has_metadata = true; result.peers = 4; result.seeders = 3; return result; }
bool Engine::wait_metadata(const std::string&, std::vector<FileInfo>& files, const std::atomic<bool>*, double, std::string*) {
    files = {{"synthetic.mp4", int64_t(media_bytes.size()), 0}}; return true;
}
int Engine::guess_file(const std::vector<FileInfo>&, int, int) { return 0; }
std::shared_ptr<Torrent> Engine::open_reader(const std::string&, int, int* id, int64_t* size, std::string*, ReaderRole role) {
    if (role != ReaderRole::Download) throw std::runtime_error("transfer did not request the download reader role");
    ++readers_opened;
    *id = 1; *size = int64_t(media_bytes.size()); return std::make_shared<Torrent>();
}
void Engine::close_reader(const std::shared_ptr<Torrent>&, int) { ++readers_closed; }
int Engine::read(const std::shared_ptr<Torrent>&, int, int, int64_t position, uint8_t* data, int wanted, const std::atomic<bool>* abort) {
    int64_t expected = -1;
    first_read.compare_exchange_strong(expected, position);
    if (cancel_during_read && position >= cancel_at) {
        *cancel_during_read = true;
        while (!abort->load()) std::this_thread::sleep_for(1ms);
        return -1;
    }
    if ((error_at >= 0 && position >= error_at) || abort->load()) return -1;
    const auto count = std::min<int64_t>(wanted, int64_t(media_bytes.size()) - position);
    if (count > 0) std::memcpy(data, media_bytes.data() + position, size_t(count));
    return int(count);
}
} // namespace bt

int main(int argc, char** argv) {
    try {
        if (argc != 4) return 2;
        helper_path = fs::absolute(argv[1]).string();
        helper_root = fs::absolute(argv[2]).string();
        fs::create_directories(helper_root);
        media_bytes = bytes(argv[3]);
        check(media_bytes.size() > (20u << 20) && media_bytes.size() % 16384 != 0, "fixture covers multiple blocks and an unaligned tail");
        ::signal(SIGPIPE, SIG_IGN);
        http_init("");
        download_writer::set_test_channel_factory(channel_factory);
        {
            reset(Fault::HoldWrite);
            const auto item = request();
            const auto held_control = control;
            std::atomic<bool> cancel{false}, prefix_valid{true};
            std::atomic<int> callbacks{0};
            std::atomic<int64_t> published{0};
            auto running = std::async(std::launch::async, [&] {
                return download_transfer(item, [&](const DownloadTransferProgress& progress) {
                    if (progress.done > held_control->acknowledged.load()) prefix_valid = false;
                    published = progress.done;
                    ++callbacks;
                }, cancel);
            });
            bool held;
            { std::unique_lock<std::mutex> lock(control->mutex); held = control->wake.wait_for(lock, 3s, [&] { return control->held; }); }
            const auto pending = running.wait_for(0ms);
            const auto physical_size = fs::exists(item.partial_path) ? fs::file_size(item.partial_path) : 0;
            const auto durable = checkpoint_bytes(item);
            std::this_thread::sleep_for(1100ms);
            const auto before_ack = published.load();
            const auto heartbeat_callbacks = callbacks.load();
            { std::lock_guard<std::mutex> lock(control->mutex); control->release = true; } control->wake.notify_all();
            const auto result = running.get();
            check(held && !control->gate_timeout, "the first real helper write acknowledgement was withheld");
            check(pending == std::future_status::timeout && physical_size == (4u << 20), "a physical helper write cannot finish the transfer before acknowledgement");
            check(durable == 0 && before_ack == 0 && heartbeat_callbacks >= 2, "heartbeat progress cannot publish an unacknowledged prefix");
            check(prefix_valid, "every published prefix is bounded by the helper acknowledgement");
            complete(item, result);
        }
        {
            reset(); const auto item = request(); std::atomic<bool> cancel{false};
            cancel_during_read = &cancel; cancel_at = 4ll << 20;
            const auto result = download_transfer(item, {}, cancel);
            check(result.status == DownloadTransferStatus::Cancelled && result.done == (4ll << 20), "cancellation preserves only the completed block");
            check(checkpoint_bytes(item) == result.done && bytes(item.partial_path) == media_bytes.substr(0, size_t(result.done)), "cancellation durably saves the exact verified prefix");
            check(control->destroyed == 1, "cancel closes the helper before returning");
            { std::ofstream tail(item.partial_path, std::ios::app | std::ios::binary); tail << "uncommitted-crash-tail"; }
            resume(item, result.done);
        }
        {
            reset(Fault::DropWrite); const auto item = request(); std::atomic<bool> cancel{false};
            int64_t published = 0;
            const auto result = download_transfer(item, [&](const DownloadTransferProgress& progress) { published = progress.done; }, cancel);
            check(control->injected && result.status == DownloadTransferStatus::Error && result.done == 0 && published == 0, "a lost write acknowledgement never publishes or completes the data");
            check(fs::file_size(item.partial_path) == (4u << 20) && checkpoint_bytes(item) == 0, "unacknowledged physical bytes stay outside the durable checkpoint");
            check(control->destroyed == 1, "a failed exchange closes its channel");
            resume(item, 0);
        }
        for (const auto fault : {Fault::DropFinalCheckpoint, Fault::DropClose}) {
            reset(fault); const auto item = request(); std::atomic<bool> cancel{false};
            const auto result = download_transfer(item, {}, cancel);
            check(control->injected && result.status == DownloadTransferStatus::Error, "lost commit or close acknowledgement prevents promotion");
            check(checkpoint_bytes(item) == int64_t(media_bytes.size()), "the helper's completed durable checkpoint survives a lost acknowledgement");
            check(control->destroyed == 1, "commit/close failure releases the helper channel");
            resume(item, -1);
        }
        {
            reset(Fault::DropInitialCheckpoint); const auto item = request(); std::atomic<bool> cancel{false};
            const auto result = download_transfer(item, {}, cancel);
            check(control->injected && result.status == DownloadTransferStatus::Error && first_read == -1, "missing initial checkpoint acknowledgement prevents all torrent reads");
            check(checkpoint_bytes(item) == 0 && fs::file_size(item.partial_path) == 0, "failed startup leaves an empty durable checkpoint");
            resume(item, 0);
        }
        {
            reset(Fault::NoHelper); const auto item = request(); std::atomic<bool> cancel{false};
            const auto result = download_transfer(item, {}, cancel);
            check(result.status == DownloadTransferStatus::Error && first_read == -1, "helper launch failure does not consume torrent data");
            check(!fs::exists(item.partial_path) && !fs::exists(item.checkpoint_path), "helper failure cannot silently fall back to application file writes");
        }
        {
            reset(); const auto item = request(); std::atomic<bool> cancel{false}; error_at = 4ll << 20;
            const auto result = download_transfer(item, {}, cancel);
            check(result.status == DownloadTransferStatus::Error && result.done == (4ll << 20), "a torrent read failure preserves prior acknowledged data");
            check(checkpoint_bytes(item) == result.done, "read failure commits the safe prefix before closing");
            resume(item, result.done);
        }
        {
            reset(); const auto item = request();
            std::ofstream(item.partial_path, std::ios::binary).write(media_bytes.data(), 123);
            std::ofstream(item.checkpoint_path) << json{{"version", 1}, {"kind", "torrent"}, {"source", "0123456789abcdef0123456789abcdef01234567:0"}, {"bytes", 12345}, {"total", media_bytes.size()}, {"extension", ".mp4"}};
            resume(item, 0);
        }
        check(readers_opened == readers_closed, "all torrent reader lifetimes end on success, cancellation and failure");
        download_writer::set_test_channel_factory(nullptr);
        std::cout << "Transfer/helper integration: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Transfer/helper integration failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
