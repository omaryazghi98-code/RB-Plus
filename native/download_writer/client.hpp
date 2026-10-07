// Stremio Plus loader-backed download writer. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace download_writer {

class Channel {
public:
    virtual ~Channel() = default;
    virtual std::int64_t send(const void* data, std::size_t bytes) = 0;
    virtual std::int64_t receive(void* data, std::size_t bytes) = 0;
};

struct CommitTimes {
    double media_ms = 0;
    double state_ms = 0;
};

// Called once during native startup, before the download worker starts.
void configure_helper(std::string path);
std::unique_ptr<Channel> launch_helper();

#ifdef STREMIO_DOWNLOAD_WRITER_TEST
using TestChannelFactory = std::unique_ptr<Channel>(*)();
void set_test_channel_factory(TestChannelFactory factory);
#endif

class Client {
public:
    explicit Client(std::unique_ptr<Channel> channel);
    ~Client() = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool begin(std::string_view job_id, std::int64_t candidate, std::int64_t total);
    bool write(const void* data, std::size_t bytes);
    bool checkpoint(std::string_view state, CommitTimes& timings);
    bool close();
    bool healthy() const noexcept { return channel_ && begun_ && error_ == 0; }
    int error() const noexcept { return error_; }
    wire::Stage error_stage() const noexcept { return error_stage_; }
    std::int64_t offset() const noexcept { return offset_; }
    double last_write_ms() const noexcept { return write_ms_; }

private:
    bool exchange(wire::Operation op, const void* data, std::size_t bytes, wire::Message& reply);
    bool fail(int error, wire::Stage stage = wire::Stage::transport);
    std::unique_ptr<Channel> channel_;
    std::uint32_t pid_ = 0, sequence_ = 0;
    std::int64_t offset_ = 0, total_ = 0;
    bool begun_ = false;
    int error_ = 0;
    wire::Stage error_stage_ = wire::Stage::none;
    double write_ms_ = 0;
};

} // namespace download_writer
