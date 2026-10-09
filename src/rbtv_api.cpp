#include "rbtv_api.h"

#include "http.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace rbtv {
namespace {
constexpr uint32_t kMd5Shift[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5,9,14,20, 5,9,14,20, 5,9,14,20, 5,9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};
constexpr uint32_t kMd5K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
uint32_t rotl(uint32_t value, uint32_t count) {
    return (value << count) | (value >> (32 - count));
}
uint32_t read_le32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void write_le32(unsigned char* p, uint32_t value) {
    p[0] = static_cast<unsigned char>(value);
    p[1] = static_cast<unsigned char>(value >> 8);
    p[2] = static_cast<unsigned char>(value >> 16);
    p[3] = static_cast<unsigned char>(value >> 24);
}
std::string encode_component(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) out.push_back(static_cast<char>(c));
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
    }
    return out;
}
std::string strip_trailing_slashes(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}
bool config_valid(const ApiConfig& config, std::string& error) {
    if (config.data_api.rfind("https://", 0) != 0 || !http_valid_url(config.data_api)) {
        error = "RBTV data API must be an explicitly configured HTTPS URL";
        return false;
    }
    if (config.web_origin.rfind("https://", 0) != 0 || !http_valid_url(config.web_origin)) {
        error = "RBTV web origin must be an explicitly configured HTTPS URL";
        return false;
    }
    return true;
}
std::string make_url(const std::string& base, const std::string& path,
                     const std::vector<std::pair<std::string, std::string>>& params) {
    std::string url = strip_trailing_slashes(base) + path;
    bool first = true;
    for (const auto& p : params) {
        url += first ? '?' : '&';
        first = false;
        url += encode_component(p.first) + "=" + encode_component(p.second);
    }
    return url;
}
std::vector<std::string> request_headers(const ApiConfig& config) {
    return {
        "User-Agent: " + config.user_agent,
        "Origin: " + strip_trailing_slashes(config.web_origin),
        "Referer: " + strip_trailing_slashes(config.web_origin) + "/",
        "Accept: */*"
    };
}
bool get(const ApiConfig& config, const std::string& path,
         const std::vector<std::pair<std::string, std::string>>& params,
         std::string& body, std::string& error, const std::atomic<bool>* cancel) {
    if (!config_valid(config, error)) return false;
    if (cancel && cancel->load()) { error = "Cancelled"; return false; }
    const std::string url = make_url(config.data_api, path, params);
    const auto response = http_get(url, config.timeout_seconds, cancel, request_headers(config),
                                   config.max_response_bytes);
    if (!response.ok()) {
        error = "RBTV request failed: " + response.describe();
        return false;
    }
    body = response.body;
    return true;
}
bool get_pb(const ApiConfig& config, const std::string& path,
            const std::vector<std::pair<std::string, std::string>>& params,
            std::string& data, std::string& error, const std::atomic<bool>* cancel) {
    std::string response;
    if (!get(config, path, params, response, error, cancel)) return false;
    uint64_t code = 0;
    std::string message;
    if (!parse_pb_response(response, code, message, data, error)) return false;
    if (code != 0) {
        error = "RBTV API code " + std::to_string(code);
        if (!message.empty()) error += ": " + message;
        return false;
    }
    return true;
}
bool signature_version(const ApiConfig& config, const std::vector<uint64_t>& codes,
                       const std::vector<std::pair<std::string, std::string>>& args,
                       std::string& version, std::string& error,
                       const std::atomic<bool>* cancel) {
    std::vector<std::pair<std::string, std::string>> params;
    for (uint64_t code : codes) params.emplace_back("code", std::to_string(code));
    params.insert(params.end(), args.begin(), args.end());
    params.emplace_back("stream", "true");
    std::string data;
    if (!get_pb(config, "/api/common/bs", params, data, error, cancel)) return false;
    std::map<uint64_t, std::string> versions;
    if (!parse_signature_response(data, versions, error)) return false;
    auto it = versions.find(codes.front());
    if (it == versions.end() || it->second.empty()) {
        error = "RBTV bootstrap did not include signature code " + std::to_string(codes.front());
        return false;
    }
    version = it->second;
    return true;
}
bool get_region(const ApiConfig& config, UserInfo& region, std::string& error,
                const std::atomic<bool>* cancel) {
    std::string data;
    if (!get_pb(config, "/api-cf/user/info", {}, data, error, cancel)) return false;
    if (!parse_user_info(data, region, error)) return false;
    return true;
}
void use_backup_domain(Stream& stream) {
    if (stream.backup_domains.empty() || stream.url.empty()) return;
    const std::string& backup = stream.backup_domains.front();
    if (backup.rfind("https://", 0) == 0 || backup.rfind("http://", 0) == 0) {
        stream.url = backup;
        return;
    }
    const auto scheme = stream.url.find("://");
    if (scheme == std::string::npos) return;
    const auto host_start = scheme + 3;
    const auto path_start = stream.url.find_first_of("/?#", host_start);
    const std::string suffix = path_start == std::string::npos ? "/" : stream.url.substr(path_start);
    if (backup.empty() || backup.find_first_of("/?#@") != std::string::npos) return;
    stream.url = stream.url.substr(0, host_start) + backup + suffix;
}
} // namespace

std::string md5_hex(std::string_view input) {
    std::vector<unsigned char> bytes(input.begin(), input.end());
    const uint64_t bit_length = static_cast<uint64_t>(bytes.size()) * 8u;
    bytes.push_back(0x80);
    while ((bytes.size() % 64) != 56) bytes.push_back(0);
    for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<unsigned char>(bit_length >> (i * 8)));

    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    for (size_t offset = 0; offset < bytes.size(); offset += 64) {
        uint32_t m[16];
        for (unsigned i = 0; i < 16; ++i) m[i] = read_le32(bytes.data() + offset + i * 4);
        uint32_t a = a0, b = b0, c = c0, d = d0;
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t f = 0, g = 0;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const uint32_t next_d = d;
            d = c; c = b;
            b += rotl(a + f + kMd5K[i] + m[g], kMd5Shift[i]);
            a = next_d;
        }
        a0 += a; b0 += b; c0 += c; d0 += d;
    }
    unsigned char digest[16];
    write_le32(digest, a0); write_le32(digest + 4, b0);
    write_le32(digest + 8, c0); write_le32(digest + 12, d0);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char c : digest) out << std::setw(2) << unsigned(c);
    return out.str();
}
std::string signature_prefix(std::string_view input) { return md5_hex(input).substr(0, 6); }

bool get_live_matches(const ApiConfig& config, uint64_t sport_type, uint64_t language,
                      LiveResult& output, std::string& error, const std::atomic<bool>* cancel) {
    const std::string sport = std::to_string(sport_type), lang = std::to_string(language);
    std::string version;
    if (!signature_version(config, {100}, {{"sportType", sport}}, version, error, cancel)) return false;
    const std::string query = "language=" + lang + "&sportType=" + sport + "&stream=true";
    const std::string path = "/sfver" + signature_prefix(query) + version + "/api/match/live";
    std::string data;
    if (!get_pb(config, path, {{"language", lang}, {"sportType", sport}, {"stream", "true"}},
                data, error, cancel)) return false;
    LiveResult parsed;
    if (!parse_live_response(data, language, parsed.matches, parsed.streams, error)) return false;
    output = std::move(parsed);
    return true;
}

bool get_match_detail(const ApiConfig& config, uint64_t match_id, uint64_t sport_type,
                      uint64_t language, Match& match, bool& has_match,
                      std::vector<Stream>& streams, std::string& error,
                      const std::atomic<bool>* cancel) {
    const std::string id = std::to_string(match_id), sport = std::to_string(sport_type);
    const std::string lang = std::to_string(language);
    std::string version;
    if (!signature_version(config, {102, 103, 104, 105, 106, 107},
                           {{"sportType", sport}, {"matchId", id}}, version, error, cancel)) return false;
    const std::string query = "matchId=" + id + "&language=" + lang +
                              "&sportType=" + sport + "&stream=true";
    const std::string path = "/sfver" + signature_prefix(query) + version + "/api/match/detail";
    std::string data;
    if (!get_pb(config, path, {{"matchId", id}, {"language", lang},
                               {"sportType", sport}, {"stream", "true"}},
                data, error, cancel)) return false;
    return parse_match_detail_response(data, language, match, has_match, streams, error);
}

bool get_user_info(const ApiConfig& config, UserInfo& info, std::string& error,
                   const std::atomic<bool>* cancel) {
    UserInfo parsed;
    if (!get_region(config, parsed, error, cancel)) return false;
    info = std::move(parsed);
    return true;
}

bool resolve_stream(const ApiConfig& config, uint64_t match_id, uint64_t sport_type,
                    uint64_t stream_id, uint64_t site_type, const UserInfo& supplied_region,
                    Stream& output, std::string& error, const std::atomic<bool>* cancel) {
    UserInfo region = supplied_region;
    if (region.continent.empty() || region.country.empty()) {
        if (!get_region(config, region, error, cancel)) return false;
    }
    std::string data;
    const std::vector<std::pair<std::string, std::string>> params = {
        {"matchId", std::to_string(match_id)},
        {"sportType", std::to_string(sport_type)},
        {"streamId", std::to_string(stream_id)},
        {"siteType", std::to_string(site_type)},
        {"continent", region.continent},
        {"country", region.country},
        {"digit", config.digit},
        {"withOriginal", "true"}
    };
    if (!get_pb(config, "/api/stream/detail", params, data, error, cancel)) return false;
    Stream parsed;
    bool found = false;
    if (!parse_stream_response(data, parsed, found, error)) return false;
    if (!found) { error = "RBTV stream-detail response contained no stream"; return false; }
    use_backup_domain(parsed);
    output = std::move(parsed);
    return true;
}
} // namespace rbtv
