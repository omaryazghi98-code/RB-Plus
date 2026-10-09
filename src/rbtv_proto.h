#pragma once
// RBTV+ protobuf wire shapes observed in the supplied APK. This module only
// decodes bounded byte buffers; it does not execute code or bypass activation.
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace rbtv {

struct Team {
    uint64_t id = 0;
    std::string name;
    std::map<uint64_t, std::string> names;
    std::string logo;
};
struct League {
    uint64_t id = 0;
    uint64_t sport_type = 0;
    std::string name;
    std::map<uint64_t, std::string> names;
    std::string logo;
    uint64_t level = 0;
};
struct Match {
    uint64_t id = 0;
    uint64_t sport_type = 0;
    uint64_t date = 0;
    uint64_t status = 0;
    League league;
    bool has_league = false;
    std::string name;
    Team home, away;
    bool has_home = false, has_away = false;
    uint64_t home_score = 0, away_score = 0;
    bool hot = false;
    bool stream_operability = false;
    uint64_t stream_operate_date = 0, stream_last_alive_date = 0;
};
struct Stream {
    uint64_t id = 0;
    uint64_t sport_type = 0;
    std::string name, url;
    uint64_t status = 0, available_type = 0;
    bool recommended = false;
    uint64_t priority = 0, site_type = 0;
    std::string full_name;
    uint64_t cdn_type = 0;
    std::vector<std::string> backup_domains;
    std::map<std::string, std::string> headers;
    uint64_t match_id = 0;
    std::string page_url;
};
struct UserInfo {
    std::string ip, country, continent, iframe_host;
};

bool parse_pb_response(std::string_view bytes, uint64_t& code, std::string& message,
                       std::string& data, std::string& error);
bool parse_signature_response(std::string_view bytes, std::map<uint64_t, std::string>& versions,
                              std::string& error);
bool parse_live_response(std::string_view bytes, uint64_t language, std::vector<Match>& matches,
                         std::vector<Stream>& streams, std::string& error);
bool parse_match_detail_response(std::string_view bytes, uint64_t language, Match& match,
                                 bool& has_match, std::vector<Stream>& streams, std::string& error);
bool parse_stream_response(std::string_view bytes, Stream& stream, bool& has_stream, std::string& error);
bool parse_user_info(std::string_view bytes, UserInfo& info, std::string& error);

} // namespace rbtv
