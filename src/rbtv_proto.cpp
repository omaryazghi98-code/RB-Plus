#include "rbtv_proto.h"

#include <algorithm>
#include <limits>
#include <sstream>

namespace rbtv {
namespace {
struct Field {
    uint32_t number = 0;
    uint8_t wire = 0;
    uint64_t integer = 0;
    std::string bytes;
};
using Fields = std::vector<Field>;

bool read_varint(std::string_view input, size_t& pos, uint64_t& value) {
    value = 0;
    unsigned shift = 0;
    for (unsigned i = 0; i < 10 && pos < input.size(); ++i) {
        const uint8_t byte = static_cast<uint8_t>(input[pos++]);
        if (shift == 63 && (byte & 0xfe) != 0) return false;
        value |= uint64_t(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) return true;
        shift += 7;
    }
    return false;
}

bool parse_fields(std::string_view input, Fields& fields, std::string& error) {
    fields.clear();
    size_t pos = 0;
    while (pos < input.size()) {
        uint64_t tag = 0;
        if (!read_varint(input, pos, tag) || tag == 0 || tag > std::numeric_limits<uint32_t>::max()) {
            error = "Malformed protobuf tag";
            return false;
        }
        Field field;
        field.number = static_cast<uint32_t>(tag >> 3);
        field.wire = static_cast<uint8_t>(tag & 7);
        switch (field.wire) {
        case 0:
            if (!read_varint(input, pos, field.integer)) {
                error = "Malformed protobuf varint";
                return false;
            }
            break;
        case 1:
            if (input.size() - pos < 8) { error = "Truncated fixed64"; return false; }
            field.bytes.assign(input.substr(pos, 8));
            pos += 8;
            break;
        case 2: {
            uint64_t length = 0;
            if (!read_varint(input, pos, length) || length > input.size() - pos) {
                error = "Invalid protobuf length";
                return false;
            }
            field.bytes.assign(input.substr(pos, static_cast<size_t>(length)));
            pos += static_cast<size_t>(length);
            break;
        }
        case 5:
            if (input.size() - pos < 4) { error = "Truncated fixed32"; return false; }
            field.bytes.assign(input.substr(pos, 4));
            pos += 4;
            break;
        default:
            error = "Unsupported protobuf wire type";
            return false;
        }
        fields.push_back(std::move(field));
        if (fields.size() > 50000) { error = "Protobuf field limit exceeded"; return false; }
    }
    return true;
}

const Field* first(const Fields& fields, uint32_t number, int wire = -1) {
    auto it = std::find_if(fields.begin(), fields.end(), [=](const Field& f) {
        return f.number == number && (wire < 0 || f.wire == wire);
    });
    return it == fields.end() ? nullptr : &*it;
}
std::vector<const Field*> all(const Fields& fields, uint32_t number, int wire = -1) {
    std::vector<const Field*> result;
    for (const auto& field : fields)
        if (field.number == number && (wire < 0 || field.wire == wire)) result.push_back(&field);
    return result;
}
uint64_t number(const Fields& fields, uint32_t n, uint64_t fallback = 0) {
    const Field* f = first(fields, n, 0);
    return f ? f->integer : fallback;
}
std::string str(const Fields& fields, uint32_t n) {
    const Field* f = first(fields, n, 2);
    return f ? f->bytes : std::string();
}

bool parse_string_map(const Fields& fields, uint32_t field_number,
                      std::map<std::string, std::string>& output, bool numeric_key = false,
                      std::map<uint64_t, std::string>* numeric_output = nullptr) {
    for (const Field* entry : all(fields, field_number, 2)) {
        Fields pair;
        std::string error;
        if (!parse_fields(entry->bytes, pair, error)) return false;
        const Field* key = first(pair, 1);
        const Field* value = first(pair, 2);
        if (!key || !value) continue;
        if (numeric_key) {
            if (key->wire != 0 || value->wire != 2 || !numeric_output) continue;
            (*numeric_output)[key->integer] = value->bytes;
        } else {
            if (key->wire != 2 || value->wire != 2) continue;
            output[key->bytes] = value->bytes;
        }
    }
    return true;
}
std::string localized_name(const std::map<uint64_t, std::string>& names, uint64_t language) {
    auto it = names.find(language);
    if (it != names.end() && !it->second.empty()) return it->second;
    it = names.find(0);
    if (it != names.end() && !it->second.empty()) return it->second;
    return names.empty() ? std::string() : names.begin()->second;
}

bool parse_team(std::string_view bytes, uint64_t language, Team& team, std::string& error) {
    Fields fields;
    if (!parse_fields(bytes, fields, error)) return false;
    team.id = number(fields, 1);
    std::map<std::string, std::string> unused;
    if (!parse_string_map(fields, 3, unused, true, &team.names)) {
        // The helper needs an unused string-map reference for the numeric-key path.
        error = "Malformed team name map";
        return false;
    }
    team.name = localized_name(team.names, language);
    team.logo = str(fields, 4);
    return true;
}
bool parse_league(std::string_view bytes, uint64_t language, League& league, std::string& error) {
    Fields fields;
    if (!parse_fields(bytes, fields, error)) return false;
    league.id = number(fields, 1);
    league.sport_type = number(fields, 2);
    std::map<std::string, std::string> unused;
    if (!parse_string_map(fields, 3, unused, true, &league.names)) {
        error = "Malformed league name map";
        return false;
    }
    league.name = localized_name(league.names, language);
    league.logo = str(fields, 4);
    league.level = number(fields, 5);
    return true;
}
bool parse_match(std::string_view bytes, uint64_t language, Match& match, std::string& error) {
    Fields fields;
    if (!parse_fields(bytes, fields, error)) return false;
    match.id = number(fields, 1);
    match.sport_type = number(fields, 2);
    match.date = number(fields, 3);
    match.status = number(fields, 4);
    if (const Field* f = first(fields, 10, 2)) {
        if (!parse_league(f->bytes, language, match.league, error)) return false;
        match.has_league = true;
    }
    match.name = str(fields, 19);
    if (const Field* f = first(fields, 20, 2)) {
        if (!parse_team(f->bytes, language, match.home, error)) return false;
        match.has_home = true;
    }
    if (const Field* f = first(fields, 21, 2)) {
        if (!parse_team(f->bytes, language, match.away, error)) return false;
        match.has_away = true;
    }
    match.home_score = number(fields, 22);
    match.away_score = number(fields, 23);
    match.hot = number(fields, 80) != 0;
    match.stream_operability = number(fields, 90) != 0;
    match.stream_operate_date = number(fields, 91);
    match.stream_last_alive_date = number(fields, 92);
    return true;
}
bool parse_stream(std::string_view bytes, Stream& stream, std::string& error) {
    Fields fields;
    if (!parse_fields(bytes, fields, error)) return false;
    stream.id = number(fields, 1);
    stream.sport_type = number(fields, 2);
    stream.name = str(fields, 3);
    stream.url = str(fields, 4);
    stream.status = number(fields, 5);
    stream.available_type = number(fields, 6);
    stream.recommended = number(fields, 7) != 0;
    stream.priority = number(fields, 8);
    stream.site_type = number(fields, 9);
    stream.full_name = str(fields, 10);
    stream.cdn_type = number(fields, 11);
    for (const Field* f : all(fields, 12, 2)) stream.backup_domains.push_back(f->bytes);
    if (!parse_string_map(fields, 20, stream.headers)) {
        error = "Malformed stream header map";
        return false;
    }
    stream.match_id = number(fields, 50);
    stream.page_url = str(fields, 120);
    return true;
}
} // namespace

bool parse_pb_response(std::string_view bytes, uint64_t& code, std::string& message,
                       std::string& data, std::string& error) {
    Fields fields;
    if (!parse_fields(bytes, fields, error)) return false;
    code = number(fields, 2);
    message = str(fields, 3);
    data = str(fields, 10);
    return true;
}

bool parse_signature_response(std::string_view bytes, std::map<uint64_t, std::string>& versions,
                              std::string& error) {
    Fields fields;
    if (!parse_fields(bytes, fields, error)) return false;
    versions.clear();
    for (const Field* entry : all(fields, 1, 2)) {
        Fields pair;
        if (!parse_fields(entry->bytes, pair, error)) return false;
        const Field* key = first(pair, 1, 0);
        const Field* value = first(pair, 2, 2);
        if (key && value) versions[key->integer] = value->bytes;
    }
    return true;
}

bool parse_live_response(std::string_view bytes, uint64_t language, std::vector<Match>& matches,
                         std::vector<Stream>& streams, std::string& error) {
    Fields fields;
    if (!parse_fields(bytes, fields, error)) return false;
    matches.clear(); streams.clear();
    for (const Field* f : all(fields, 1, 2)) {
        Match match;
        if (!parse_match(f->bytes, language, match, error)) return false;
        matches.push_back(std::move(match));
    }
    for (const Field* f : all(fields, 2, 2)) {
        Stream stream;
        if (!parse_stream(f->bytes, stream, error)) return false;
        streams.push_back(std::move(stream));
    }
    return true;
}

bool parse_match_detail_response(std::string_view bytes, uint64_t language, Match& match,
                                 bool& has_match, std::vector<Stream>& streams, std::string& error) {
    Fields fields;
    if (!parse_fields(bytes, fields, error)) return false;
    const Field* raw_match = first(fields, 1, 2);
    has_match = raw_match != nullptr;
    if (raw_match && !parse_match(raw_match->bytes, language, match, error)) return false;
    streams.clear();
    for (const Field* f : all(fields, 2, 2)) {
        Stream stream;
        if (!parse_stream(f->bytes, stream, error)) return false;
        streams.push_back(std::move(stream));
    }
    return true;
}

bool parse_stream_response(std::string_view bytes, Stream& stream, bool& has_stream, std::string& error) {
    Fields fields;
    if (!parse_fields(bytes, fields, error)) return false;
    const Field* raw = first(fields, 2, 2);
    has_stream = raw != nullptr;
    return !raw || parse_stream(raw->bytes, stream, error);
}

bool parse_user_info(std::string_view bytes, UserInfo& info, std::string& error) {
    Fields fields;
    if (!parse_fields(bytes, fields, error)) return false;
    info.ip = str(fields, 1);
    info.country = str(fields, 2);
    info.continent = str(fields, 3);
    info.iframe_host = str(fields, 10);
    return true;
}
} // namespace rbtv
