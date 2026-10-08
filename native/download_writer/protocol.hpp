// Stremio Plus download writer protocol. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace download_writer::wire {

inline constexpr std::uint32_t magic = 0x31574453; // SDW1
inline constexpr std::uint16_t version = 3;
inline constexpr std::size_t max_block = 4u << 20;
inline constexpr std::size_t max_checkpoint = 8u << 10;
inline constexpr int client_timeout_us = 30'000'000;
inline constexpr int helper_idle_seconds = 180;
inline constexpr char root[] = "/data/Stremio/downloads";
inline constexpr std::size_t max_directory_length = 1023;
inline constexpr std::size_t max_begin = 34 + max_directory_length;
inline constexpr char title_id[] = "PPSA74126";

enum class Operation : std::uint32_t {
    begin = 1, write = 2, checkpoint = 3, close = 4, response = 5
};

enum class Stage : std::uint32_t {
    none = 0, protocol, begin, media_write, state_validate, media_sync,
    state_unlink, state_open, state_check, state_write, state_sync,
    state_close, state_rename, directory_sync, close, transport
};

constexpr const char* stage_name(Stage stage) noexcept {
    switch (stage) {
    case Stage::none: return "none";
    case Stage::protocol: return "protocol";
    case Stage::begin: return "begin";
    case Stage::media_write: return "media_write";
    case Stage::state_validate: return "state_validate";
    case Stage::media_sync: return "media_sync";
    case Stage::state_unlink: return "state_unlink";
    case Stage::state_open: return "state_open";
    case Stage::state_check: return "state_check";
    case Stage::state_write: return "state_write";
    case Stage::state_sync: return "state_sync";
    case Stage::state_close: return "state_close";
    case Stage::state_rename: return "state_rename";
    case Stage::directory_sync: return "directory_sync";
    case Stage::close: return "close";
    case Stage::transport: return "transport";
    }
    return "unknown";
}

// Fixed-width little-endian frames; a request has no timings or error value.
// Begin carries a 33-byte job ID, a NUL separator and its exact parent directory.
// Its offset is a validated candidate
// checkpoint; the helper returns zero if that prefix is absent from the file.
// The first command after Begin must commit an initial Checkpoint.
// Write carries a complete block and advances the offset only after writing it.
// Checkpoint carries the app's bounded JSON state and flushes media, state and
// directory in that order. Close requires the written prefix to be committed.
struct Message {
    std::uint32_t signature = magic;
    std::uint16_t protocol = version;
    std::uint16_t header_bytes = 64;
    Operation operation = Operation::begin;
    std::uint32_t pid = 0;
    std::uint32_t sequence = 0;
    std::uint32_t payload_bytes = 0;
    std::int32_t error = 0;
    Stage stage = Stage::none;
    std::int64_t offset = 0;
    std::int64_t total = 0;
    std::uint64_t media_us = 0;
    std::uint64_t state_us = 0;
};
static_assert(sizeof(Message) == 64);
static_assert(offsetof(Message, offset) == 32);
static_assert(std::endian::native == std::endian::little);

constexpr bool job_id(std::string_view value) noexcept {
    if (value.size() != 33 || value.front() != 'd') return false;
    for (const char c : value.substr(1))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

constexpr bool directory_path(std::string_view path) noexcept {
    if (path.size() < 2 || path.size() > max_directory_length || path.front() != '/' || path.back() == '/')
        return false;
    for (std::size_t at = 1; at < path.size();) {
        const auto end = path.find('/', at);
        const auto part = path.substr(at, end == path.npos ? path.size() - at : end - at);
        if (part.empty() || part.size() > 255 || part == "." || part == "..") return false;
        for (const unsigned char c : part) if (c < 32 || c == 127 || c == '\\') return false;
        if (end == path.npos) break;
        at = end + 1;
    }
    return true;
}

constexpr bool download_directory(std::string_view path) noexcept {
    return directory_path(path) &&
        (path == "/data" || path.starts_with("/data/") || path.starts_with("/mnt/"));
}

constexpr bool envelope(const Message& value) noexcept {
    return value.signature == magic && value.protocol == version &&
           value.header_bytes == sizeof(Message) &&
           std::uint32_t(value.stage) <= std::uint32_t(Stage::transport) &&
           value.pid > 0 && value.pid <= INT32_MAX && value.sequence > 0 &&
           value.offset >= 0 && value.total > 0 && value.offset <= value.total;
}

constexpr bool request(const Message& value) noexcept {
    if (!envelope(value) || value.error || value.stage != Stage::none || value.media_us || value.state_us) return false;
    switch (value.operation) {
    case Operation::begin:
        return value.sequence == 1 && value.payload_bytes >= 36 && value.payload_bytes <= max_begin;
    case Operation::write:
        return value.payload_bytes > 0 && value.payload_bytes <= max_block &&
               value.payload_bytes <= std::uint64_t(value.total - value.offset);
    case Operation::checkpoint: return value.payload_bytes > 0 && value.payload_bytes <= max_checkpoint;
    case Operation::close: return value.payload_bytes == 0;
    default: return false;
    }
}

constexpr bool response(const Message& value, const Message& sent) noexcept {
    return envelope(value) && value.operation == Operation::response &&
           value.payload_bytes == 0 && value.pid == sent.pid &&
           value.sequence == sent.sequence && value.total == sent.total &&
           value.error >= 0 && (value.error != 0 || value.stage == Stage::none);
}

} // namespace download_writer::wire
