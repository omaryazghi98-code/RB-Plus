#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

extern int __wrap_fcntl(int descriptor, int command, ...);
extern int test_console_nbio_option(void);
extern int __real_getsockopt(int, int, int, void *, socklen_t *);
extern int __real_setsockopt(int, int, int, const void *, socklen_t);

static int checks;
static int fcntl_error;
static int forced_result;
static int probe_error;
static int option_error;
static int real_calls;
static int probe_calls;
static int option_get_calls;
static int option_set_calls;
static int last_descriptor;
static int last_command;
static intptr_t last_argument;

#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        ++checks;                                                               \
        if (!(condition))                                                       \
        {                                                                       \
            fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__,          \
                    #condition, errno);                                         \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

static void reset(int error)
{
    fcntl_error = error;
    forced_result = 0;
    probe_error = 0;
    option_error = 0;
    real_calls = 0;
    probe_calls = 0;
    option_get_calls = 0;
    option_set_calls = 0;
    errno = 0;
}

int __real_fcntl(int descriptor, int command, ...)
{
    va_list arguments;
    va_start(arguments, command);
    last_argument = va_arg(arguments, intptr_t);
    va_end(arguments);
    last_descriptor = descriptor;
    last_command = command;
    ++real_calls;
    errno = fcntl_error;
    return fcntl_error ? -1 : forced_result;
}

int __wrap_getsockopt(int descriptor, int level, int option, void *value,
                     socklen_t *length)
{
    if (level == SOL_SOCKET && option == SO_TYPE)
    {
        ++probe_calls;
        if (probe_error)
        {
            errno = probe_error;
            return -1;
        }
    }
    else if (level == SOL_SOCKET && option == test_console_nbio_option())
    {
        ++option_get_calls;
        int type = 0;
        socklen_t type_length = sizeof(type);
        if (__real_getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &type_length) < 0)
            return -1;
        if (option_error)
        {
            errno = option_error;
            return -1;
        }
        const int flags = fcntl(descriptor, F_GETFL);
        if (flags < 0)
            return -1;
        CHECK(*length == sizeof(int));
        *(int *)value = (flags & O_NONBLOCK) != 0;
        *length = sizeof(int);
        return 0;
    }
    return __real_getsockopt(descriptor, level, option, value, length);
}

int __wrap_setsockopt(int descriptor, int level, int option, const void *value,
                     socklen_t length)
{
    if (level == SOL_SOCKET && option == test_console_nbio_option())
    {
        ++option_set_calls;
        int type = 0;
        socklen_t type_length = sizeof(type);
        if (__real_getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &type_length) < 0)
            return -1;
        if (option_error)
        {
            errno = option_error;
            return -1;
        }
        CHECK(length == sizeof(int));
        const int flags = fcntl(descriptor, F_GETFL);
        if (flags < 0)
            return -1;
        return fcntl(descriptor, F_SETFL,
                     *(const int *)value ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
    }
    return __real_setsockopt(descriptor, level, option, value, length);
}

int main(void)
{
    char path[] = "/tmp/stremio-native-fcntl-XXXXXX";
    const int file = mkstemp(path);
    CHECK(file >= 0);
    CHECK(unlink(path) == 0);
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    /* A regular-file O_DIRECT rejection must reach the buffered writer. */
    reset(EINVAL);
    CHECK(__wrap_fcntl(file, F_SETFL, O_RDWR | O_DIRECT) == -1);
    CHECK(errno == EINVAL);
    CHECK(probe_calls == 1 && option_set_calls == 0);
    CHECK(real_calls == 1 && last_descriptor == file && last_command == F_SETFL);
    CHECK(last_argument == (O_RDWR | O_DIRECT));

    reset(EINVAL);
    CHECK(__wrap_fcntl(file, F_GETFD) == -1 && errno == EINVAL);
    CHECK(probe_calls == 1 && option_get_calls == 0);
    reset(EINVAL);
    CHECK(__wrap_fcntl(file, F_SETFD, FD_CLOEXEC) == -1 && errno == EINVAL);
    CHECK(probe_calls == 1 && option_set_calls == 0);
    reset(EINVAL);
    CHECK(__wrap_fcntl(file, F_GETFL) == -1 && errno == EINVAL);
    CHECK(probe_calls == 1 && option_get_calls == 0);

    /* Socket-only fallbacks still set and clear real nonblocking flags. */
    reset(EINVAL);
    CHECK(__wrap_fcntl(sockets[0], F_SETFL, O_RDWR | O_NONBLOCK) == 0);
    CHECK((fcntl(sockets[0], F_GETFL) & O_NONBLOCK) != 0);
    CHECK(probe_calls == 1 && option_set_calls == 1);
    reset(EINVAL);
    CHECK(__wrap_fcntl(sockets[0], F_GETFL) == (O_RDWR | O_NONBLOCK));
    CHECK(last_argument == 0 && probe_calls == 1 && option_get_calls == 1);
    reset(EINVAL);
    CHECK(__wrap_fcntl(sockets[0], F_SETFL, O_RDWR) == 0);
    CHECK((fcntl(sockets[0], F_GETFL) & O_NONBLOCK) == 0);
    reset(EINVAL);
    CHECK(__wrap_fcntl(sockets[0], F_GETFL) == O_RDWR);

    reset(EINVAL);
    CHECK(__wrap_fcntl(sockets[0], F_GETFD) == 0);
    CHECK(last_argument == 0 && probe_calls == 1);
    reset(EINVAL);
    CHECK(__wrap_fcntl(sockets[0], F_SETFD, FD_CLOEXEC) == 0);
    CHECK(probe_calls == 1 && option_get_calls == 0 && option_set_calls == 0);

    /* Successful libc calls and unrelated errors never use socket fallbacks. */
    reset(0);
    forced_result = 37;
    CHECK(__wrap_fcntl(file, F_GETFL) == 37);
    CHECK(probe_calls == 0 && errno == 0);
    const int errors[] = {EBADF, EIO, ENOSPC, EINTR, ENOTSUP};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i)
    {
        reset(errors[i]);
        CHECK(__wrap_fcntl(sockets[0], F_SETFL, O_NONBLOCK) == -1);
        CHECK(errno == errors[i] && probe_calls == 0 && option_set_calls == 0);
    }

    reset(EINVAL);
    CHECK(__wrap_fcntl(-1, F_GETFL) == -1 && errno == EINVAL);
    CHECK(probe_calls == 1 && option_get_calls == 0);
    reset(EINVAL);
    probe_error = ENOPROTOOPT;
    CHECK(__wrap_fcntl(sockets[0], F_SETFL, O_NONBLOCK) == -1 && errno == EINVAL);
    CHECK(probe_calls == 1 && option_set_calls == 0);
    reset(EINVAL);
    option_error = ENOBUFS;
    CHECK(__wrap_fcntl(sockets[0], F_SETFL, O_NONBLOCK) == -1 && errno == ENOBUFS);
    CHECK(probe_calls == 1 && option_set_calls == 1);
    reset(EINVAL);
    option_error = ENOPROTOOPT;
    CHECK(__wrap_fcntl(sockets[0], F_GETFL) == -1 && errno == ENOPROTOOPT);
    CHECK(probe_calls == 1 && option_get_calls == 1);

    reset(EINVAL);
    CHECK(__wrap_fcntl(sockets[0], F_DUPFD, (intptr_t)0) == -1 && errno == EINVAL);
    CHECK(probe_calls == 0 && option_get_calls == 0 && option_set_calls == 0);
    CHECK(close(file) == 0 && close(sockets[0]) == 0 && close(sockets[1]) == 0);
    printf("{\"result\":\"PASS\",\"assertions\":%d}\n", checks);
    return 0;
}
