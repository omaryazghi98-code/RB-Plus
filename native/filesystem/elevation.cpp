/*
 * ps5-native-app-boilerplate - elfldr elevation client.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "elevation.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace
{
#ifndef STREMIO_TITLE_ID
#define STREMIO_TITLE_ID "PPSA98273"
#endif

constexpr char kSandboxHelperPath[] =
    "/mnt/sandbox/" STREMIO_TITLE_ID "_000/app0/lapy.elf";
constexpr char kInstalledHelperPath[] =
    "/data/homebrew/" STREMIO_TITLE_ID "/lapy.elf";
char g_helper_open_diagnostic[224] = "Helper open: not attempted";

struct NetSockaddrIn
{
    std::uint8_t length;
    std::uint8_t family;
    std::uint16_t port;
    std::uint32_t address;
    std::uint16_t virtual_port;
    std::uint8_t zero[6];
};

extern "C"
{
    int sceKernelOpen(const char *path, int flags, mode_t mode);
    int sceKernelClose(int descriptor);
    std::int64_t sceKernelRead(int descriptor, void *buffer, std::size_t length);
    int sceNetConnect(int socket, const void *address, std::uint32_t address_length);
    int sceNetSend(int socket, const void *data, std::size_t length, int flags);
    int sceNetRecv(int socket, void *data, std::size_t length, int flags);
    int sceNetSetsockopt(int socket, int level, int option, const void *value, std::uint32_t size);
    int sceNetSocket(const char *name, int domain, int type, int protocol);
    int sceNetSocketClose(int socket);
}

bool send_all(int socket, const void *data, std::size_t size) noexcept
{
    return elevation::wire::transfer(static_cast<const std::uint8_t *>(data), size,
                                     [socket](const auto *bytes, std::size_t remaining)
                                     { return sceNetSend(socket, bytes, remaining, 0); });
}

bool receive(int socket, elevation::wire::Message &message) noexcept
{
    return elevation::wire::transfer(reinterpret_cast<std::uint8_t *>(&message), sizeof(message),
                                     [socket](auto *bytes, std::size_t remaining)
                                     { return sceNetRecv(socket, bytes, remaining, 0); });
}

elevation::Status exchange(int socket, const elevation::wire::Message &request) noexcept
{
    using namespace elevation;
    using wire::Kind;
    if (!send_all(socket, &request, sizeof(request)))
        return Status::transport_error;
    wire::Message reply{};
    if (!receive(socket, reply))
        return Status::transport_error;
    if (wire::matches(reply, request, Kind::response) && reply.status != Status::ok)
        return reply.status;
    if (!wire::matches(reply, request, Kind::prepare) || reply.status != Status::ok)
        return Status::protocol_error;

    // The native same-UID syscall clones credentials before the helper edits them.
    // The helper independently verifies that p_ucred actually changed.
    wire::Message prepared = request;
    prepared.kind = Kind::prepared;
    if (seteuid(geteuid()) != 0)
        prepared.status = Status::prepare_failed;
    if (!send_all(socket, &prepared, sizeof(prepared)) || !receive(socket, reply))
        return Status::transport_error;
    if (!wire::matches(reply, request, Kind::response))
        return Status::protocol_error;
    if (prepared.status != Status::ok)
        return Status::prepare_failed;
    return reply.status;
}

int open_helper(const char *preferred_path) noexcept
{
    struct Candidate
    {
        const char *name;
        const char *path;
    };
    const std::array<Candidate, 3> candidates{{
        {"app0", preferred_path},
        {"sandbox", kSandboxHelperPath},
        {"homebrew", kInstalledHelperPath},
    }};
    std::array<int, 3> kernel_result{};
    std::array<int, 3> posix_errno{};

    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        errno = 0;
        const int descriptor = sceKernelOpen(candidates[i].path, O_RDONLY, 0);
        kernel_result[i] = descriptor;
        if (descriptor >= 0)
        {
            std::snprintf(g_helper_open_diagnostic, sizeof(g_helper_open_diagnostic),
                          "Helper open: %s path succeeded (sceKernelOpen)", candidates[i].name);
            return descriptor;
        }

        // A normal POSIX open is a valid fallback for the packaged read-only
        // ELF. Some loader/mount combinations expose app0 through libc first.
        errno = 0;
        const int posix_descriptor = open(candidates[i].path, O_RDONLY);
        posix_errno[i] = errno;
        if (posix_descriptor >= 0)
        {
            std::snprintf(g_helper_open_diagnostic, sizeof(g_helper_open_diagnostic),
                          "Helper open: %s path succeeded (open)", candidates[i].name);
            return posix_descriptor;
        }
    }

    std::snprintf(g_helper_open_diagnostic, sizeof(g_helper_open_diagnostic),
                  "Helper open failed: app0 K%d/E%d; sandbox K%d/E%d; home K%d/E%d",
                  kernel_result[0], posix_errno[0], kernel_result[1], posix_errno[1],
                  kernel_result[2], posix_errno[2]);
    return -1;
}

elevation::Status submit(int socket, int helper, const elevation::wire::Message &request) noexcept
{
    using elevation::Status;
    // SceNet uses integer microseconds and its own timeout options, not BSD timeval.
    constexpr int socket_level = 0xffff;
    constexpr int timeout_us = elevation::wire::io_timeout_us;
    for (const int option : {0x1105, 0x1106, 0x1109}) // send, receive, connect
    {
        if (sceNetSetsockopt(socket, socket_level, option, &timeout_us, sizeof(timeout_us)) < 0)
            return Status::transport_error;
    }
    constexpr std::uint16_t port = 9021;
    const NetSockaddrIn address{sizeof(NetSockaddrIn),
                                2,
                                static_cast<std::uint16_t>((port << 8) | (port >> 8)),
                                0x0100007f,
                                0,
                                {0}};
    if (sceNetConnect(socket, &address, sizeof(address)) < 0)
        return Status::transport_error;

    std::array<std::uint8_t, 4096> buffer{};
    for (;;)
    {
        const auto count = sceKernelRead(helper, buffer.data(), buffer.size());
        if (count == 0)
            break;
        if (count < 0 || !send_all(socket, buffer.data(), static_cast<std::size_t>(count)))
            return Status::transport_error;
    }
    // elfldr consumes the ELF's section extent, then hands this same connection
    // to the helper as stdin/stdout. Keep it open for the protocol exchange.
    return exchange(socket, request);
}
} // namespace

const char *elevation::helper_open_diagnostic() noexcept
{
    return g_helper_open_diagnostic;
}

elevation::Status elevation::request(Capability capability, const char *helper_path) noexcept
{
    wire::Message message{};
    message.pid = static_cast<std::uint32_t>(getpid());
    message.capability = capability;
    if (const auto error = wire::validate(message); error != Status::ok)
        return error;
    if (helper_path == nullptr)
        return Status::invalid_request;
    const int helper = open_helper(helper_path);
    // Keep a missing/inaccessible bundled ELF distinct from a helper that
    // starts but cannot obtain kernel state or filesystem access.
    if (helper < 0)
        return Status::helper_open_failed;
    const int socket = sceNetSocket("sandbox_elevator", 2, 1, 6);
    const auto result = socket < 0 ? Status::transport_error : submit(socket, helper, message);
    if (socket >= 0)
        (void)sceNetSocketClose(socket);
    (void)sceKernelClose(helper);
    return result;
}
