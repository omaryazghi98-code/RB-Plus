#include "client.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
namespace wire = download_writer::wire;
int checks = 0, failures = 0;

void check(bool value, const char* description) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}

struct Script {
    std::vector<unsigned char> input, output;
    std::vector<wire::Message> requests;
    std::vector<std::string> payloads;
    std::function<void(wire::Message&, const wire::Message&)> reply;
    std::size_t output_at = 0, fragment = 17, truncate_reply = 0;
    unsigned sends = 0, receives = 0, destroyed = 0;
    bool interrupt = false, fail_send = false, fail_receive = false;

    void prepare() {
        check(input.size() >= sizeof(wire::Message), "client sent a complete request header");
        if (input.size() < sizeof(wire::Message)) return;
        wire::Message request;
        std::memcpy(&request, input.data(), sizeof(request));
        check(wire::request(request) && input.size() == sizeof(request) + request.payload_bytes,
              "client sent exactly one bounded request and its complete payload");
        requests.push_back(request);
        payloads.emplace_back(reinterpret_cast<const char*>(input.data() + sizeof(request)), request.payload_bytes);
        input.clear();
        wire::Message response = request;
        response.operation = wire::Operation::response;
        response.payload_bytes = 0;
        if (request.operation == wire::Operation::write) {
            response.offset += request.payload_bytes;
            response.media_us = 1234;
        } else if (request.operation == wire::Operation::checkpoint) {
            response.media_us = 2345;
            response.state_us = 3456;
        }
        if (reply) reply(response, request);
        const auto* bytes = reinterpret_cast<const unsigned char*>(&response);
        output.assign(bytes, bytes + sizeof(response));
        output_at = 0;
    }
};

class Channel final : public download_writer::Channel {
public:
    explicit Channel(std::shared_ptr<Script> value) : script_(std::move(value)) {}
    ~Channel() override { ++script_->destroyed; }
    std::int64_t send(const void* data, std::size_t size) override {
        if (script_->fail_send) { errno = EIO; return -1; }
        if (script_->interrupt && ++script_->sends % 5 == 1) { errno = EINTR; return -1; }
        const auto count = std::min(size, script_->fragment);
        const auto* bytes = static_cast<const unsigned char*>(data);
        script_->input.insert(script_->input.end(), bytes, bytes + count);
        return static_cast<std::int64_t>(count);
    }
    std::int64_t receive(void* data, std::size_t size) override {
        if (script_->fail_receive) return 0;
        if (script_->interrupt && ++script_->receives % 5 == 1) { errno = EINTR; return -1; }
        if (script_->output_at == script_->output.size()) script_->prepare();
        const auto limit = script_->truncate_reply ? script_->truncate_reply : script_->output.size();
        if (script_->output_at >= limit) return 0;
        const auto count = std::min({size, script_->fragment, limit - script_->output_at});
        std::memcpy(data, script_->output.data() + script_->output_at, count);
        script_->output_at += count;
        return static_cast<std::int64_t>(count);
    }
private:
    std::shared_ptr<Script> script_;
};

constexpr char job[] = "d0123456789abcdef0123456789abcdef";

void normal_exchange() {
    auto script = std::make_shared<Script>();
    script->fragment = 3;
    script->interrupt = true;
    download_writer::Client client(std::make_unique<Channel>(script));
    check(client.begin(job, 0, 4097) && client.healthy(), "fragmented interrupted begin succeeds");
    download_writer::CommitTimes timing;
    check(client.checkpoint("{}", timing) && timing.media_ms == 2.345 && timing.state_ms == 3.456,
          "checkpoint preserves helper media and state timings");
    std::vector<unsigned char> payload(4096, 0x6d);
    check(client.write(payload.data(), payload.size()) && client.offset() == 4096,
          "fragmented interrupted write advances only to its exact acknowledgement");
    check(client.last_write_ms() == 1.234, "write exposes helper-only disk latency");
    check(client.write(payload.data(), 1) && client.offset() == 4097, "final unaligned byte retains exact offset");
    check(client.checkpoint("{}", timing) && client.close() && !client.healthy(), "committed stream closes cleanly");
    check(script->destroyed == 1 && script->requests.size() == 6,
          "successful close immediately destroys the sole channel");
    for (std::size_t i = 0; i < script->requests.size(); ++i)
        check(script->requests[i].sequence == i + 1, "request sequence is strictly monotonic");
    check(script->requests[3].offset == 4096 && script->requests.back().offset == 4097,
          "subsequent requests carry only acknowledged offsets");
}

void malformed_replies() {
    using Mutation = std::function<void(wire::Message&)>;
    const std::vector<Mutation> mutations{
        [](auto& r) { ++r.signature; }, [](auto& r) { ++r.protocol; },
        [](auto& r) { --r.header_bytes; }, [](auto& r) { r.operation = wire::Operation::write; },
        [](auto& r) { ++r.pid; }, [](auto& r) { ++r.sequence; },
        [](auto& r) { ++r.payload_bytes; }, [](auto& r) { r.stage = wire::Stage::close; },
        [](auto& r) { r.error = EIO; r.stage = static_cast<wire::Stage>(16); },
        [](auto& r) { ++r.total; }, [](auto& r) { r.error = -1; },
        [](auto& r) { --r.offset; }, [](auto& r) { ++r.offset; }
    };
    for (const auto& mutation : mutations) {
        auto script = std::make_shared<Script>();
        download_writer::Client client(std::make_unique<Channel>(script));
        check(client.begin(job, 0, 16), "malformed reply fixture begins normally");
        script->reply = [&](auto& response, const auto&) { mutation(response); };
        const unsigned char data = 3;
        check(!client.write(&data, 1) && client.error() == EPROTO && client.offset() == 0,
              "malformed write reply cannot publish a byte prefix");
        check(!client.healthy() && script->destroyed == 1,
              "malformed reply permanently closes the transport");
        const auto count = script->requests.size();
        check(!client.write(&data, 1) && !client.close() && client.error() == EPROTO &&
              script->requests.size() == count && script->destroyed == 1,
              "failure remains sticky without another request or double-close");
    }
}

void remote_and_transport_failures() {
    for (int mode = 0; mode < 4; ++mode) {
        auto script = std::make_shared<Script>();
        download_writer::Client client(std::make_unique<Channel>(script));
        check(client.begin(job, 0, 16), "transport failure fixture begins normally");
        if (mode == 0) script->reply = [](auto& response, const auto&) {
            response.error = ENOSPC; response.stage = wire::Stage::media_write;
        };
        if (mode == 1) script->fail_send = true;
        if (mode == 2) script->fail_receive = true;
        if (mode == 3) script->truncate_reply = 13;
        const unsigned char data = 0;
        check(!client.write(&data, 1) && client.offset() == 0 && !client.healthy(),
              "remote failure or lost reply retains the last confirmed offset");
        check(client.error() == (mode == 0 ? ENOSPC : EIO) && script->destroyed == 1,
              "failure preserves its error and closes the channel");
        check(client.error_stage() == (mode == 0 ? wire::Stage::media_write : wire::Stage::transport),
              "failure distinguishes helper file operations from transport failure");
    }
}

void checkpoint_error_stages() {
    for (const auto stage : {wire::Stage::state_validate, wire::Stage::media_sync, wire::Stage::state_unlink,
                            wire::Stage::state_open, wire::Stage::state_check, wire::Stage::state_write,
                            wire::Stage::state_sync, wire::Stage::state_close, wire::Stage::state_rename,
                            wire::Stage::directory_sync}) {
        auto script = std::make_shared<Script>();
        download_writer::Client client(std::make_unique<Channel>(script));
        check(client.begin(job, 0, 16), "checkpoint stage fixture begins normally");
        script->reply = [stage](auto& response, const auto&) { response.error = EIO; response.stage = stage; };
        download_writer::CommitTimes timing;
        check(!client.checkpoint("{}", timing) && client.error() == EIO && client.error_stage() == stage,
              "checkpoint failure preserves the exact helper operation");
        check(!client.close() && client.error_stage() == stage && script->destroyed == 1,
              "cleanup cannot replace the original checkpoint error stage");
    }
}

void resume_and_local_bounds() {
    {
        auto script = std::make_shared<Script>();
        script->reply = [](auto& response, const auto& request) {
            if (request.operation == wire::Operation::begin) response.offset = 0;
        };
        download_writer::Client client(std::make_unique<Channel>(script));
        check(client.begin(job, 64, 128) && client.offset() == 0, "helper may roll back an absent resume prefix to zero");
    }
    {
        auto script = std::make_shared<Script>();
        script->reply = [](auto& response, const auto&) { response.offset = 32; };
        download_writer::Client client(std::make_unique<Channel>(script));
        check(!client.begin(job, 64, 128) && client.error() == EPROTO,
              "begin cannot acknowledge an arbitrary intermediate resume offset");
    }
    for (int mode = 0; mode < 5; ++mode) {
        auto script = std::make_shared<Script>();
        download_writer::Client client(std::make_unique<Channel>(script));
        bool accepted = false;
        if (mode == 0) accepted = client.begin("../invalid", 0, 8);
        if (mode == 1) accepted = client.begin(job, -1, 8);
        if (mode == 2) accepted = client.begin(job, 0, 0);
        if (mode == 3) accepted = client.begin(job, 9, 8);
        if (mode == 4) accepted = client.write("x", 1);
        check(!accepted && client.error() == EINVAL && script->requests.empty() && script->destroyed == 1,
              "invalid local request is rejected before sending anything");
    }
    for (int mode = 0; mode < 4; ++mode) {
        auto script = std::make_shared<Script>();
        download_writer::Client client(std::make_unique<Channel>(script));
        check(client.begin(job, 0, 8), "payload bounds fixture begins");
        download_writer::CommitTimes timing;
        bool accepted = false;
        if (mode == 0) accepted = client.write("x", wire::max_block + 1);
        if (mode == 1) accepted = client.write(nullptr, 1);
        if (mode == 2) accepted = client.write("x", 9);
        if (mode == 3) accepted = client.checkpoint(std::string(wire::max_checkpoint + 1, 'x'), timing);
        check(!accepted && client.error() == EINVAL && script->requests.size() == 1,
              "oversized, missing or out-of-file payload is never sent");
    }
}
void selected_directory() {
    const std::string destination = "/mnt/ext1/Videos/Stremio Plus Downloads";
    auto script = std::make_shared<Script>();
    download_writer::Client client(std::make_unique<Channel>(script));
    check(client.begin(destination, job, 0, 64), "selected mounted volume begins successfully");
    std::string expected(job);
    expected.push_back('\0'); expected += destination;
    check(script->payloads.size() == 1 && script->payloads.front() == expected,
          "begin transmits the exact selected directory without default substitution");
    check(wire::download_directory(destination) && wire::download_directory("/data/Videos"),
          "native writer accepts data and mounted-volume directories");
    check(!wire::download_directory("/user/system") && !wire::download_directory("/mnt") &&
          !wire::download_directory("/data-other/videos"), "native directory namespace stays bounded");
    const std::vector<std::string> invalid = {
        "relative/path", "/mnt/ext1/", "/mnt/ext1//Videos", "/mnt/ext1/../Videos",
        "/mnt/ext1/./Videos", std::string("/mnt/ext1\0ignored", 17),
        "/mnt/ext1/" + std::string(1024, 'a')};
    for (const auto& directory : invalid) {
        auto rejected = std::make_shared<Script>();
        download_writer::Client connection(std::make_unique<Channel>(rejected));
        check(!connection.begin(directory, job, 0, 64) && connection.error() == EINVAL &&
              rejected->requests.empty(), "invalid selected directory never reaches the transport");
    }
}
} // namespace

int main() {
    normal_exchange();
    malformed_replies();
    remote_and_transport_failures();
    checkpoint_error_stages();
    resume_and_local_bounds();
    selected_directory();
    std::cout << "Download writer client: " << checks - failures << '/' << checks << " checks passed\n";
    return failures ? 1 : 0;
}
