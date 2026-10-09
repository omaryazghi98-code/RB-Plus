#include "rbtv_proto.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace rbtv;

static void varint(std::string& out, uint64_t n) {
    while (n >= 0x80) { out.push_back(static_cast<char>((n & 0x7f) | 0x80)); n >>= 7; }
    out.push_back(static_cast<char>(n));
}
static std::string vfield(uint32_t field, uint64_t n) {
    std::string out; varint(out, (uint64_t(field) << 3) | 0); varint(out, n); return out;
}
static std::string sfield(uint32_t field, const std::string& value) {
    std::string out; varint(out, (uint64_t(field) << 3) | 2); varint(out, value.size()); out += value; return out;
}
static std::string map_entry(uint64_t key, const std::string& value) {
    return vfield(1, key) + sfield(2, value);
}
static std::string team(uint64_t id, const std::string& name, const std::string& logo) {
    return vfield(1, id) + sfield(3, map_entry(0, name)) + sfield(4, logo);
}
static std::string league(uint64_t id, const std::string& name) {
    return vfield(1, id) + vfield(2, 1) + sfield(3, map_entry(0, name)) + sfield(4, "league-logo") + vfield(5, 2);
}
static std::string match() {
    return vfield(1, 8001) + vfield(2, 1) + vfield(3, 1760000000) + vfield(4, 2) +
           sfield(10, league(91, "League")) + sfield(19, "Match label") +
           sfield(20, team(11, "Home", "home-logo")) +
           sfield(21, team(12, "Away", "away-logo")) +
           vfield(22, 2) + vfield(23, 1) + vfield(80, 1);
}
static std::string stream() {
    return vfield(1, 44) + vfield(2, 1) + sfield(3, "Primary") + sfield(4, "https://media.example/live.m3u8") +
           vfield(5, 1) + vfield(6, 2) + vfield(7, 1) + vfield(8, 5) + vfield(9, 10) +
           sfield(10, "Primary stream") + vfield(11, 3) + sfield(12, "edge.example") +
           sfield(20, sfield(1, "Referer") + sfield(2, "https://origin.example/")) + vfield(50, 8001);
}

int main() {
    {
        std::string bytes = vfield(2, 0) + sfield(3, "ok") + sfield(10, "payload");
        uint64_t code = 99; std::string message, data, error;
        assert(parse_pb_response(bytes, code, message, data, error));
        assert(code == 0 && message == "ok" && data == "payload");
    }
    {
        std::string bytes = sfield(1, map_entry(100, "v7"));
        std::map<uint64_t, std::string> versions; std::string error;
        assert(parse_signature_response(bytes, versions, error));
        assert(versions.size() == 1 && versions.at(100) == "v7");
    }
    {
        std::string bytes = sfield(1, match()) + sfield(2, stream());
        std::vector<Match> matches; std::vector<Stream> streams; std::string error;
        assert(parse_live_response(bytes, 0, matches, streams, error));
        assert(matches.size() == 1 && streams.size() == 1);
        assert(matches[0].id == 8001 && matches[0].name == "Match label");
        assert(matches[0].has_league && matches[0].league.name == "League");
        assert(matches[0].has_home && matches[0].home.name == "Home");
        assert(matches[0].has_away && matches[0].away.name == "Away");
        assert(matches[0].home_score == 2 && matches[0].away_score == 1 && matches[0].hot);
        assert(streams[0].id == 44 && streams[0].match_id == 8001);
        assert(streams[0].headers.at("Referer") == "https://origin.example/");
        assert(streams[0].backup_domains.size() == 1 && streams[0].recommended);
    }
    {
        std::string bytes = sfield(1, match()) + sfield(2, stream());
        Match match_out; bool has_match = false; std::vector<Stream> streams; std::string error;
        assert(parse_match_detail_response(bytes, 0, match_out, has_match, streams, error));
        assert(has_match && match_out.id == 8001 && streams.size() == 1);
        Stream resolved; bool has_stream = false;
        assert(parse_stream_response(sfield(2, stream()), resolved, has_stream, error));
        assert(has_stream && resolved.url == "https://media.example/live.m3u8");
        UserInfo info;
        assert(parse_user_info(sfield(1, "127.0.0.1") + sfield(2, "MA") + sfield(3, "AF") + sfield(10, "frame.example"), info, error));
        assert(info.country == "MA" && info.continent == "AF");
    }
    {
        std::string error; uint64_t code; std::string message, data;
        assert(!parse_pb_response(std::string("\x12\x05x", 3), code, message, data, error));
        assert(!error.empty());
    }
    std::cout << "RBTV protobuf tests passed\n";
    return 0;
}
