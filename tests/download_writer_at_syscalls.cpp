// Host implementation of the PS5 syscall result convention for adapter tests.
#include "download_writer/posix_at.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace at = download_writer::posix_at;

namespace {
bool mocked = false;
at::RawResult next_result{};
long last_call[5]{};
unsigned fault_calls = 0;

int injected_error(long number, long path) {
    const char* fault = std::getenv("STREMIO_TEST_AT_FAULT");
    if (!fault) return 0;
    const char* name = reinterpret_cast<const char*>(path);
    int error = 0;
    if (number == 503 && std::strcmp(fault, "unlink-temp-denied") == 0 &&
        std::strcmp(name, "transfer.json.tmp") == 0) error = EACCES;
    if (number == 499 && std::strcmp(fault, "open-temp-full") == 0 &&
        std::strcmp(name, "transfer.json.tmp") == 0) error = ENOSPC;
    if (number == 501 && std::strcmp(fault, "rename-state-denied") == 0 &&
        std::strcmp(name, "transfer.json.tmp") == 0) error = EACCES;
    if (!error) return 0;
    const char* selected = std::getenv("STREMIO_TEST_AT_FAULT_AT");
    return ++fault_calls == (selected ? std::strtoul(selected, nullptr, 10) : 1) ? error : 0;
}
}

extern "C" at::RawResult stremio_download_writer_raw_at(long number, long a1, long a2, long a3, long a4) {
    if (mocked) {
        last_call[0] = number; last_call[1] = a1; last_call[2] = a2;
        last_call[3] = a3; last_call[4] = a4;
        return next_result;
    }
    if (const int error = injected_error(number, a2)) {
        errno = 0;
        return {error, true};
    }
    long result = -1;
    errno = 0;
    switch (number) {
    case 499: result = ::openat(int(a1), reinterpret_cast<const char*>(a2), int(a3), mode_t(a4)); break;
    case 501: result = ::renameat(int(a1), reinterpret_cast<const char*>(a2), int(a3), reinterpret_cast<const char*>(a4)); break;
    case 503: result = ::unlinkat(int(a1), reinterpret_cast<const char*>(a2), int(a3)); break;
    default: errno = ENOSYS; break;
    }
    const int error = errno;
    // A FreeBSD raw syscall reports errno in its value and carry, leaving the
    // userspace errno untouched. Deliberately leave it at zero for this test.
    errno = 0;
    return result < 0 ? at::RawResult{error, true} : at::RawResult{result, false};
}

#ifdef STREMIO_DOWNLOAD_WRITER_AT_UNIT_TEST
int main() {
    int checks = 0, failures = 0;
    const auto check = [&](bool condition, const char* description) {
        ++checks;
        if (!condition) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
    };
    for (const long descriptor : {0L, 2L, 13L, 28L, 511L}) {
        errno = EDOM;
        check(at::normalize({descriptor, false}) == descriptor && errno == EDOM,
              "success preserves valid descriptors even when they equal errno numbers");
    }
    for (const int error : {ENOENT, EACCES, ENOSPC, EINTR, EIO}) {
        errno = 0;
        check(at::normalize({error, true}) == -1 && errno == error,
              "carry-marked positive errors become POSIX failure with the exact errno");
    }
    mocked = true;
    const char first[] = "transfer.json.tmp", second[] = "transfer.json";
    next_result = {2, false}; errno = 0;
    check(at::openat(17, first, O_RDWR | O_CREAT, 0600) == 2 && errno == 0,
          "openat accepts descriptor two when carry is clear");
    check(last_call[0] == 499 && last_call[1] == 17 && last_call[2] == reinterpret_cast<long>(first) &&
          last_call[3] == (O_RDWR | O_CREAT) && last_call[4] == 0600,
          "openat forwards all native syscall arguments");
    next_result = {ENOENT, true}; errno = 0;
    check(at::openat(17, first, O_RDONLY, 0) == -1 && errno == ENOENT,
          "openat never interprets an error number as a file descriptor");
    next_result = {0, false};
    check(at::renameat(17, first, 18, second) == 0 && last_call[0] == 501 && last_call[1] == 17 &&
          last_call[2] == reinterpret_cast<long>(first) && last_call[3] == 18 &&
          last_call[4] == reinterpret_cast<long>(second), "renameat forwards both anchored paths");
    next_result = {EACCES, true}; errno = 0;
    check(at::renameat(17, first, 18, second) == -1 && errno == EACCES,
          "renameat reports the original permission failure");
    next_result = {ENOENT, true}; errno = 0;
    check(at::unlinkat(17, first, 0) == -1 && errno == ENOENT,
          "missing temporary checkpoint reports normalized ENOENT");
    check(last_call[0] == 503 && last_call[1] == 17 && last_call[2] == reinterpret_cast<long>(first) &&
          last_call[3] == 0, "unlinkat forwards directory, path and flags");
    std::cout << "Download writer native at adapters: " << checks - failures << '/' << checks << " checks passed\n";
    return failures ? 1 : 0;
}
#endif
