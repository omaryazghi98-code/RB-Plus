// Descriptor-relative filesystem calls. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cerrno>
#include <climits>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#ifndef STREMIO_DOWNLOAD_WRITER_TEST
#include <sys/syscall.h>
#endif

namespace download_writer::posix_at {

struct RawResult {
    long value;
    bool failed;
};

inline long normalize(RawResult result) noexcept {
    if (!result.failed) return result.value;
    errno = result.value > 0 && result.value <= INT_MAX ? static_cast<int>(result.value) : EIO;
    return -1;
}

#if defined(STREMIO_DOWNLOAD_WRITER_AT_TEST) || !defined(STREMIO_DOWNLOAD_WRITER_TEST)

inline constexpr long open_number = 499;
inline constexpr long rename_number = 501;
inline constexpr long unlink_number = 503;

#ifdef STREMIO_DOWNLOAD_WRITER_AT_TEST
extern "C" RawResult stremio_download_writer_raw_at(long, long, long, long, long);

inline RawResult raw(long number, long first, long second, long third, long fourth) noexcept {
    return stremio_download_writer_raw_at(number, first, second, third, fourth);
}
#else
static_assert(SYS_openat == open_number && SYS_renameat == rename_number && SYS_unlinkat == unlink_number);

inline RawResult raw(long number, long first, long second, long third, long fourth) noexcept {
    // The payload SDK's bare syscall stubs do not convert FreeBSD's carry flag
    // into POSIX errno. Capture it before any C++ expression can change flags.
    register long fourth_argument asm("r10") = fourth;
    unsigned char failed;
    asm volatile("syscall\n\tsetc %1"
                 : "+a"(number), "=qm"(failed), "+d"(third)
                 : "D"(first), "S"(second), "r"(fourth_argument)
                 : "rcx", "r11", "memory", "cc");
    return {number, failed != 0};
}
#endif

inline int openat(int directory, const char* path, int flags, mode_t mode = 0) noexcept {
    return static_cast<int>(normalize(raw(open_number, directory, reinterpret_cast<long>(path), flags, mode)));
}

inline int renameat(int from_directory, const char* from, int to_directory, const char* to) noexcept {
    return static_cast<int>(normalize(raw(rename_number, from_directory, reinterpret_cast<long>(from),
                                          to_directory, reinterpret_cast<long>(to))));
}

inline int unlinkat(int directory, const char* path, int flags) noexcept {
    return static_cast<int>(normalize(raw(unlink_number, directory, reinterpret_cast<long>(path), flags, 0)));
}

#else

inline int openat(int directory, const char* path, int flags, mode_t mode = 0) noexcept {
    return ::openat(directory, path, flags, mode);
}

inline int renameat(int from_directory, const char* from, int to_directory, const char* to) noexcept {
    return ::renameat(from_directory, from, to_directory, to);
}

inline int unlinkat(int directory, const char* path, int flags) noexcept {
    return ::unlinkat(directory, path, flags);
}

#endif

} // namespace download_writer::posix_at
