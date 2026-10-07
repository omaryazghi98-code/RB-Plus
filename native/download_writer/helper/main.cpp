// Stremio Plus download writer. SPDX-License-Identifier: GPL-3.0-or-later
#include "../protocol.hpp"
#include "../posix_at.hpp"
#include "../../../third_party/json.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef STREMIO_DOWNLOAD_WRITER_TEST
extern "C" int sceKernelGetAppInfo(pid_t, void*);
#endif

namespace {
namespace wire = download_writer::wire;
namespace descriptor = download_writer::posix_at;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

struct File {
    int fd = -1;
    explicit File(int value = -1) : fd(value) {}
    ~File() { if (fd >= 0) ::close(fd); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    bool close() {
        const int value = fd;
        fd = -1;
        return value < 0 || ::close(value) == 0;
    }
};

int failure() { return errno > 0 ? errno : EIO; }

std::uint64_t elapsed(Clock::time_point start) {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

bool read_exact(void* data, std::size_t size) {
    auto* bytes = static_cast<unsigned char*>(data);
    while (size) {
        const auto count = ::read(STDIN_FILENO, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || std::size_t(count) > size) return false;
        bytes += count;
        size -= std::size_t(count);
    }
    return true;
}

bool write_all(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (size) {
        const auto count = ::write(fd, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || std::size_t(count) > size) {
            if (count == 0) errno = EIO;
            return false;
        }
        bytes += count;
        size -= std::size_t(count);
    }
    return true;
}

bool target(std::uint32_t pid) {
#ifdef STREMIO_DOWNLOAD_WRITER_TEST
    (void)pid;
    return true;
#else
    struct AppInfo {
        std::uint32_t app_id;
        std::uint64_t unknown;
        char title_id[14];
        char remainder[0x3c];
    };
    AppInfo info{};
    return sceKernelGetAppInfo(static_cast<pid_t>(pid), &info) == 0 &&
           std::memcmp(info.title_id, wire::title_id, sizeof(wire::title_id)) == 0;
#endif
}

int open_root(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.back() == '/') { errno = EINVAL; return -1; }
    File directory(::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
    if (directory.fd < 0) return -1;
    for (std::size_t begin = 1; begin < path.size();) {
        const auto end = path.find('/', begin);
        const std::string component(path.substr(begin, end == path.npos ? path.size() - begin : end - begin));
        if (component.empty() || component == "." || component == "..") { errno = EINVAL; return -1; }
        const int next = descriptor::openat(directory.fd, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (next < 0) return -1;
        directory.close();
        directory.fd = next;
        if (end == path.npos) break;
        begin = end + 1;
    }
    const int result = directory.fd;
    directory.fd = -1;
    return result;
}

bool regular(int fd, std::int64_t* size = nullptr) {
    struct stat info{};
    if (::fstat(fd, &info) != 0) return false;
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 0) { errno = EINVAL; return false; }
    if (size) *size = info.st_size;
    return true;
}

bool integer(const Json& state, const char* name, std::int64_t expected) {
    const auto it = state.find(name);
    if (it == state.end()) return false;
    if (it->is_number_unsigned()) return it->get<std::uint64_t>() == std::uint64_t(expected);
    return it->is_number_integer() && it->get<std::int64_t>() == expected;
}

bool identity(std::string_view source) {
    if (source.size() < 42 || source.size() > 51 || source[40] != ':') return false;
    for (const char c : source.substr(0, 40))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    const auto index = source.substr(41);
    if (index.size() > 1 && index.front() == '0') return false;
    std::uint64_t value = 0;
    for (const char c : index) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + unsigned(c - '0');
        if (value > INT32_MAX) return false;
    }
    return true;
}

bool extension(std::string_view value) {
    if (value.size() < 2 || value.size() > 8 || value.front() != '.') return false;
    for (const char c : value.substr(1))
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

struct Session {
    File directory, media;
    std::int64_t written = 0, committed = -1, total = 0;
    std::string source, suffix;
    std::unique_ptr<unsigned char, decltype(&std::free)> buffer{nullptr, &std::free};

    int begin(const wire::Message& request, const std::string& job, std::string_view root) {
        if (!wire::job_id(job) || !target(request.pid)) return EACCES;
        void* bytes = nullptr;
        const int allocation = ::posix_memalign(&bytes, 16u << 10, wire::max_block);
        if (allocation != 0) return allocation;
        buffer.reset(static_cast<unsigned char*>(bytes));
        File parent(open_root(root));
        if (parent.fd < 0) return failure();
        directory.fd = descriptor::openat(parent.fd, job.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (directory.fd < 0 || ::flock(directory.fd, LOCK_EX | LOCK_NB) != 0) return failure();
        media.fd = descriptor::openat(directory.fd, "media.part", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);
        std::int64_t size = 0;
        if (media.fd < 0 || !regular(media.fd, &size) || ::flock(media.fd, LOCK_EX | LOCK_NB) != 0 ||
            ::fchmod(media.fd, 0600) != 0) return failure();
        written = request.offset <= size ? request.offset : 0;
        total = request.total;
        if (::ftruncate(media.fd, written) != 0 || ::lseek(media.fd, written, SEEK_SET) != written) return failure();
        return 0;
    }

    bool valid_state(const std::string& bytes) {
        const Json state = Json::parse(bytes, nullptr, false);
        if (!state.is_object() || !integer(state, "version", 1) || !integer(state, "bytes", written) ||
            !integer(state, "total", total)) return false;
        const auto kind = state.find("kind"), from = state.find("source"), ext = state.find("extension");
        if (kind == state.end() || !kind->is_string() || kind->get_ref<const std::string&>() != "torrent" ||
            from == state.end() || !from->is_string() || ext == state.end() || !ext->is_string()) return false;
        const auto& next_source = from->get_ref<const std::string&>();
        const auto& next_suffix = ext->get_ref<const std::string&>();
        if (!identity(next_source) || !extension(next_suffix) ||
            (!source.empty() && (source != next_source || suffix != next_suffix))) return false;
        source = next_source;
        suffix = next_suffix;
        return true;
    }

    int checkpoint(const std::string& bytes, wire::Message& reply) {
        reply.stage = wire::Stage::state_validate;
        if (!valid_state(bytes)) return EINVAL;
        reply.stage = wire::Stage::media_sync;
        const auto media_at = Clock::now();
        const int synced = ::fsync(media.fd);
        reply.media_us = elapsed(media_at);
        if (synced != 0) return failure();
        const auto state_at = Clock::now();
        const int result = save_state(bytes, reply.stage);
        reply.state_us = elapsed(state_at);
        if (result == 0) committed = written;
        return result;
    }

    int save_state(const std::string& bytes, wire::Stage& stage) {
        stage = wire::Stage::state_unlink;
        if (descriptor::unlinkat(directory.fd, "transfer.json.tmp", 0) != 0 && errno != ENOENT) return failure();
        stage = wire::Stage::state_open;
        File temporary(descriptor::openat(directory.fd, "transfer.json.tmp", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
        int error = temporary.fd < 0 ? failure() : 0;
        if (error == 0) {
            stage = wire::Stage::state_check;
            if (!regular(temporary.fd)) error = failure();
        }
        if (error == 0) {
            stage = wire::Stage::state_write;
            if (!write_all(temporary.fd, bytes.data(), bytes.size())) error = failure();
        }
        if (error == 0) {
            stage = wire::Stage::state_sync;
            if (::fsync(temporary.fd) != 0) error = failure();
        }
        if (error == 0) stage = wire::Stage::state_close;
        if (!temporary.close() && error == 0) error = failure();
        if (error == 0) {
            stage = wire::Stage::state_rename;
            if (descriptor::renameat(directory.fd, "transfer.json.tmp", directory.fd, "transfer.json") != 0) error = failure();
        }
        if (error == 0) {
            stage = wire::Stage::directory_sync;
            if (::fsync(directory.fd) != 0) error = failure();
        }
        if (error != 0) (void)descriptor::unlinkat(directory.fd, "transfer.json.tmp", 0);
        return error;
    }

    int close() {
        if (committed != written) return EBUSY;
        std::int64_t size = 0;
        if (!regular(media.fd, &size)) return failure();
        if (size != written) return EIO;
        int error = media.close() ? 0 : failure();
        if (!directory.close() && error == 0) error = failure();
        return error;
    }
};

bool timeouts() {
    timeval receive{wire::helper_idle_seconds, 0};
    timeval send{wire::client_timeout_us / 1'000'000, 0};
    return ::setsockopt(STDIN_FILENO, SOL_SOCKET, SO_RCVTIMEO, &receive, sizeof(receive)) == 0 &&
           ::setsockopt(STDOUT_FILENO, SOL_SOCKET, SO_SNDTIMEO, &send, sizeof(send)) == 0;
}

int run(std::string_view root) {
    Session session;
    std::uint32_t pid = 0, sequence = 1;
    for (;;) {
        wire::Message request;
        if (!read_exact(&request, sizeof(request))) return 1;
        wire::Message reply = request;
        reply.operation = wire::Operation::response;
        reply.payload_bytes = 0;
        reply.stage = wire::Stage::none;
        reply.media_us = reply.state_us = 0;
        if (!wire::request(request) || request.sequence != sequence ||
            (pid != 0 && (request.pid != pid || request.total != session.total || request.offset != session.written)) ||
            (sequence == 1 ? request.operation != wire::Operation::begin : request.operation == wire::Operation::begin)) {
            reply.error = EINVAL;
            reply.stage = wire::Stage::protocol;
        } else if (request.operation == wire::Operation::begin) {
            std::string job(request.payload_bytes, '\0');
            if (!read_exact(job.data(), job.size())) return 1;
            reply.stage = wire::Stage::begin;
            reply.error = session.begin(request, job, root);
            pid = request.pid;
            reply.offset = session.written;
        } else if (request.operation == wire::Operation::write) {
            reply.stage = wire::Stage::media_write;
            if (session.committed < 0) { reply.error = EINVAL; reply.stage = wire::Stage::protocol; }
            else {
                if (!read_exact(session.buffer.get(), request.payload_bytes)) return 1;
                const auto start = Clock::now();
                if (!write_all(session.media.fd, session.buffer.get(), request.payload_bytes)) reply.error = failure();
                else session.written += request.payload_bytes;
                reply.media_us = elapsed(start);
                reply.offset = session.written;
            }
        } else if (request.operation == wire::Operation::checkpoint) {
            std::string state(request.payload_bytes, '\0');
            if (!read_exact(state.data(), state.size())) return 1;
            reply.error = session.checkpoint(state, reply);
        } else {
            reply.stage = wire::Stage::close;
            reply.error = session.close();
        }
        if (reply.error == 0) reply.stage = wire::Stage::none;
        if (!write_all(STDOUT_FILENO, &reply, sizeof(reply)) || reply.error != 0) return 1;
        if (request.operation == wire::Operation::close) return 0;
        if (sequence == UINT32_MAX) return 1;
        ++sequence;
    }
}
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    if (!timeouts()) return 1;
#ifdef STREMIO_DOWNLOAD_WRITER_TEST
    if (argc != 2) return 1;
    return run(argv[1]);
#else
    (void)argc;
    (void)argv;
    return run(wire::root);
#endif
}
