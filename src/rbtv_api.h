#pragma once
#include "rbtv_proto.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rbtv {

// Endpoint defaults deliberately stay empty: the APK contained unusual
// third-party domains that must be reviewed and explicitly approved by the UI.
struct ApiConfig {
    std::string data_api;
    std::string web_origin;
    std::string digit = "snd";
    std::string user_agent = "RBTV-PS5-Port/0.4 (native; user initiated)";
    long timeout_seconds = 12;
    size_t max_response_bytes = 20u * 1024u * 1024u;
};
struct LiveResult {
    std::vector<Match> matches;
    std::vector<Stream> streams;
};

std::string md5_hex(std::string_view input);
std::string signature_prefix(std::string_view input); // first 6 lowercase MD5 hex chars

// Blocking functions: call from a worker thread, never on the UI/render thread.
// These functions perform network requests only when explicitly called by the
// caller. They neither skip activation checks nor implement DRM/P2P bypasses.
bool get_live_matches(const ApiConfig& config, uint64_t sport_type, uint64_t language,
                      LiveResult& output, std::string& error,
                      const std::atomic<bool>* cancel = nullptr);
bool get_match_detail(const ApiConfig& config, uint64_t match_id, uint64_t sport_type,
                      uint64_t language, Match& match, bool& has_match,
                      std::vector<Stream>& streams, std::string& error,
                      const std::atomic<bool>* cancel = nullptr);
bool resolve_stream(const ApiConfig& config, uint64_t match_id, uint64_t sport_type,
                    uint64_t stream_id, uint64_t site_type, const UserInfo& region,
                    Stream& output, std::string& error,
                    const std::atomic<bool>* cancel = nullptr);
bool get_user_info(const ApiConfig& config, UserInfo& info, std::string& error,
                   const std::atomic<bool>* cancel = nullptr);

} // namespace rbtv
