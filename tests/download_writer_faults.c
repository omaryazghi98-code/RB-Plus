#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static unsigned read_calls, write_calls, sync_calls;
static uint64_t media_bytes;

ssize_t read(int fd, void *data, size_t size) {
    ssize_t (*real_read)(int, void *, size_t) = dlsym(RTLD_NEXT, "read");
    const char *mode = getenv("STREMIO_TEST_IO_FAULT");
    if (mode && strcmp(mode, "interrupt") == 0) {
        if (++read_calls % 7 == 1) { errno = EINTR; return -1; }
        if (size > 4093) size = 4093;
    }
    return real_read(fd, data, size);
}

ssize_t write(int fd, const void *data, size_t size) {
    ssize_t (*real_write)(int, const void *, size_t) = dlsym(RTLD_NEXT, "write");
    const char *mode = getenv("STREMIO_TEST_IO_FAULT");
    if (mode && strcmp(mode, "interrupt") == 0) {
        if (++write_calls % 7 == 1) { errno = EINTR; return -1; }
        if (size > 4093) size = 4093;
    }
    const char *limit_text = getenv("STREMIO_TEST_WRITE_LIMIT");
    if (fd > STDERR_FILENO && limit_text) {
        const uint64_t limit = strtoull(limit_text, NULL, 10);
        if (media_bytes >= limit) {
            const char *error_text = getenv("STREMIO_TEST_WRITE_ERRNO");
            errno = error_text ? atoi(error_text) : EIO;
            if (errno <= 0) errno = EIO;
            return -1;
        }
        if (size > limit - media_bytes) size = (size_t)(limit - media_bytes);
    }
    const ssize_t written = real_write(fd, data, size);
    if (fd > STDERR_FILENO && written > 0) media_bytes += (uint64_t)written;
    return written;
}

int fsync(int fd) {
    int (*real_fsync)(int) = dlsym(RTLD_NEXT, "fsync");
    const char *failure_text = getenv("STREMIO_TEST_SYNC_FAILURE");
    if (failure_text && ++sync_calls == strtoul(failure_text, NULL, 10)) {
        errno = EIO;
        return -1;
    }
    return real_fsync(fd);
}
