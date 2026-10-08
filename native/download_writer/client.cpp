// Stremio Plus loader-backed download writer. SPDX-License-Identifier: GPL-3.0-or-later
#include "client.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace download_writer {
namespace {

std::string helper_path;
#ifdef STREMIO_DOWNLOAD_WRITER_TEST
TestChannelFactory test_factory = nullptr;
#endif

template <typename Byte, typename Function>
bool transfer(Byte* bytes, std::size_t left, Function operation) {
    while (left) {
        const auto count = operation(bytes, left);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || static_cast<std::uint64_t>(count) > left) return false;
        bytes += count;
        left -= static_cast<std::size_t>(count);
    }
    return true;
}

#ifdef PLATFORM_PS5_NATIVE
extern "C" {
int sceNetConnect(int socket, const void* address, std::uint32_t length);
int sceNetSend(int socket, const void* data, std::size_t length, int flags);
int sceNetRecv(int socket, void* data, std::size_t length, int flags);
int sceNetSetsockopt(int socket, int level, int option, const void* value, std::uint32_t length);
int sceNetSocket(const char* name, int domain, int type, int protocol);
int sceNetSocketClose(int socket);
}

struct Address {
    std::uint8_t length;
    std::uint8_t family;
    std::uint16_t port;
    std::uint32_t address;
    std::uint16_t virtual_port;
    std::uint8_t zero[6];
};

class NativeChannel final : public Channel {
public:
    explicit NativeChannel(int socket) : socket_(socket) {}
    ~NativeChannel() override {
        const int saved = errno;
        if (socket_ >= 0) sceNetSocketClose(socket_);
        errno = saved;
    }
    std::int64_t send(const void* data, std::size_t size) override {
        const auto count = sceNetSend(socket_, data, size, 0);
        if (count < 0) errno = EIO;
        return count;
    }
    std::int64_t receive(void* data, std::size_t size) override {
        const auto count = sceNetRecv(socket_, data, size, 0);
        if (count < 0) errno = EIO;
        return count;
    }
private:
    int socket_;
};
#endif

} // namespace

void configure_helper(std::string path) { helper_path = std::move(path); }

#ifdef STREMIO_DOWNLOAD_WRITER_TEST
void set_test_channel_factory(TestChannelFactory factory) { test_factory = factory; }
#endif

std::unique_ptr<Channel> launch_helper() {
#ifdef STREMIO_DOWNLOAD_WRITER_TEST
    if (test_factory) return test_factory();
#endif
#ifdef PLATFORM_PS5_NATIVE
    if (helper_path.empty()) { errno = ENOENT; return {}; }
    const int descriptor = ::open(helper_path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0) return {};
    struct File { int fd; ~File() { const int saved = errno; ::close(fd); errno = saved; } } file{descriptor};
    struct stat info{};
    if (::fstat(file.fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 64 ||
        info.st_size > (16ll << 20)) { errno = EINVAL; return {}; }
    const int socket = sceNetSocket("stremio_download_writer", 2, 1, 6);
    if (socket < 0) { errno = EIO; return {}; }
    auto channel = std::make_unique<NativeChannel>(socket);
    constexpr int level = 0xffff;
    constexpr int data_timeout = wire::client_timeout_us;
    constexpr int connect_timeout = 5'000'000;
    if (sceNetSetsockopt(socket, level, 0x1105, &data_timeout, sizeof(data_timeout)) < 0 ||
        sceNetSetsockopt(socket, level, 0x1106, &data_timeout, sizeof(data_timeout)) < 0 ||
        sceNetSetsockopt(socket, level, 0x1109, &connect_timeout, sizeof(connect_timeout)) < 0) {
        errno = EIO; return {};
    }
    // This loopback connection carries an acknowledged command after each block.
    // Keep control frames immediate; the media stream stays bounded to one block.
    constexpr int enabled = 1, send_buffer = 4 << 20, receive_buffer = 64 << 10;
    (void)sceNetSetsockopt(socket, 6, 1, &enabled, sizeof(enabled));
    (void)sceNetSetsockopt(socket, level, 0x1001, &send_buffer, sizeof(send_buffer));
    (void)sceNetSetsockopt(socket, level, 0x1002, &receive_buffer, sizeof(receive_buffer));
    constexpr std::uint16_t port = 9021;
    const Address address{sizeof(Address), 2,
        static_cast<std::uint16_t>((port << 8) | (port >> 8)), 0x0100007f, 0, {0}};
    if (sceNetConnect(socket, &address, sizeof(address)) < 0) { errno = ECONNREFUSED; return {}; }
    std::array<std::uint8_t, 64u << 10> bytes{};
    std::int64_t sent = 0;
    while (sent < info.st_size) {
        const auto wanted = static_cast<std::size_t>(std::min<std::int64_t>(bytes.size(), info.st_size - sent));
        const auto count = ::read(file.fd, bytes.data(), wanted);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || !transfer(bytes.data(), static_cast<std::size_t>(count),
            [&](const auto* at, auto left) { return channel->send(at, left); })) {
            errno = EIO; return {};
        }
        sent += count;
    }
    // The validated ELF ends at its section table. elfldr then gives this same
    // connection to the helper as stdin/stdout; never half-close it here.
    return channel;
#else
    errno = ENOSYS;
    return {};
#endif
}

Client::Client(std::unique_ptr<Channel> channel)
    : channel_(std::move(channel)), pid_(static_cast<std::uint32_t>(::getpid())) {
    if (!channel_) { error_ = errno ? errno : EIO; error_stage_ = wire::Stage::transport; }
}

bool Client::fail(int error, wire::Stage stage) {
    if (!error_) { error_ = error ? error : EIO; error_stage_ = stage; }
    channel_.reset();
    errno = error_;
    return false;
}

bool Client::exchange(wire::Operation operation, const void* data, std::size_t size, wire::Message& reply) {
    if (!channel_ || error_ || sequence_ == std::numeric_limits<std::uint32_t>::max())
        return fail(error_ ? error_ : EOVERFLOW);
    if (size > wire::max_block || (size && !data)) return fail(EINVAL);
    wire::Message request;
    request.operation = operation;
    request.pid = pid_;
    request.sequence = ++sequence_;
    request.payload_bytes = static_cast<std::uint32_t>(size);
    request.offset = offset_;
    request.total = total_;
    if (!wire::request(request)) return fail(EINVAL);
    const auto send = [&](const void* bytes, std::size_t count) {
        return transfer(static_cast<const std::uint8_t*>(bytes), count,
            [&](const auto* at, auto left) { return channel_->send(at, left); });
    };
    if (!send(&request, sizeof(request)) || (size && !send(data, size)) ||
        !transfer(reinterpret_cast<std::uint8_t*>(&reply), sizeof(reply),
            [&](auto* at, auto left) { return channel_->receive(at, left); })) return fail(EIO);
    if (!wire::response(reply, request)) return fail(EPROTO, wire::Stage::protocol);
    if (reply.error) return fail(reply.error, reply.stage);
    const auto expected = operation == wire::Operation::write ? offset_ + static_cast<std::int64_t>(size) : offset_;
    if (reply.offset != expected && !(operation == wire::Operation::begin && reply.offset == 0))
        return fail(EPROTO, wire::Stage::protocol);
    return true;
}

bool Client::begin(std::string_view job, std::int64_t candidate, std::int64_t total) {
    return begin(wire::root, job, candidate, total);
}

bool Client::begin(std::string_view directory, std::string_view job, std::int64_t candidate, std::int64_t total) {
    if (begun_ || sequence_ || !wire::job_id(job) || !wire::directory_path(directory) ||
        candidate < 0 || total <= 0 || candidate > total)
        return fail(EINVAL);
#ifdef PLATFORM_PS5_NATIVE
    if (!wire::download_directory(directory)) return fail(EINVAL);
#endif
    offset_ = candidate; total_ = total;
    std::string payload(job);
    payload.push_back('\0');
    payload.append(directory);
    wire::Message reply;
    if (!exchange(wire::Operation::begin, payload.data(), payload.size(), reply)) return false;
    offset_ = reply.offset;
    begun_ = true;
    return true;
}

bool Client::write(const void* data, std::size_t bytes) {
    write_ms_ = 0;
    if (!healthy()) return fail(error_ ? error_ : EINVAL);
    wire::Message reply;
    if (!exchange(wire::Operation::write, data, bytes, reply)) return false;
    offset_ = reply.offset;
    write_ms_ = reply.media_us / 1000.0;
    return true;
}

bool Client::checkpoint(std::string_view state, CommitTimes& timings) {
    if (!healthy() || state.empty() || state.size() > wire::max_checkpoint) return fail(error_ ? error_ : EINVAL);
    wire::Message reply;
    if (!exchange(wire::Operation::checkpoint, state.data(), state.size(), reply)) return false;
    timings.media_ms = reply.media_us / 1000.0;
    timings.state_ms = reply.state_us / 1000.0;
    return true;
}

bool Client::close() {
    if (!healthy()) return fail(error_ ? error_ : EINVAL);
    wire::Message reply;
    if (!exchange(wire::Operation::close, nullptr, 0, reply)) return false;
    channel_.reset();
    begun_ = false;
    return true;
}

} // namespace download_writer
