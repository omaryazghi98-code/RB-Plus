// Stream badges extracted from provider metadata, without changing playback.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream_presentation.h"
#include "stremio.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>

namespace {
constexpr size_t kTextLimit = 16384;
constexpr size_t kLanguageLimit = 32;

std::string ascii_lower(std::string_view value) {
    std::string out(value.substr(0, kTextLimit));
    for (auto& c : out) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return out;
}
bool word(unsigned char c) { return std::isalnum(c) || c == '_'; }
bool edge(std::string_view text, size_t pos) { return pos >= text.size() || !word(text[pos]); }
bool has_word(std::string_view text, std::string_view needle) {
    for (size_t p = 0; (p = text.find(needle, p)) != std::string_view::npos; ++p)
        if ((!p || !word(text[p - 1])) && edge(text, p + needle.size())) return true;
    return false;
}
std::string strip(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return std::string(value);
}
const json& field(const json& value, const char* key) {
    static const json empty;
    if (!value.is_object()) return empty;
    auto it = value.find(key);
    return it == value.end() ? empty : *it;
}
std::string string_value(const json& value) {
    return value.is_string() ? value.get_ref<const std::string&>().substr(0, kTextLimit) : "";
}

struct Language { const char* code; const char* aliases; };
// Codes and names only. Free text is parsed under an explicit language/audio
// label, or as complete uppercase release tags, never by substring matching.
constexpr Language kLanguages[] = {
    {"ita", "it|ita|italian|italiano"}, {"eng", "en|eng|english|inglese|ingles"},
    {"fre", "fr|fre|fra|french|français|francais|francese"},
    {"ger", "de|ger|deu|german|deutsch|tedesco"},
    {"spa", "es|spa|spanish|español|espanol|castellano|castilian|latino|latin american|spagnolo"},
    {"por", "pt|por|portuguese|português|portugues|brazilian|portoghese"},
    {"jpn", "ja|jp|jpn|japanese|giapponese|日本語"},
    {"kor", "ko|kr|kor|korean|coreano|한국어"}, {"chi", "zh|chi|zho|chinese|mandarin|cinese|中文"},
    {"yue", "yue|cantonese"}, {"rus", "ru|rus|russian|russo|русский"},
    {"ukr", "uk|ukr|ukrainian|ucraino|українська"}, {"ara", "ar|ara|arabic|arabo|العربية"},
    {"hin", "hi|hin|hindi|हिन्दी"}, {"ben", "bn|ben|bengali|bangla"},
    {"tam", "ta|tam|tamil"}, {"tel", "te|tel|telugu"}, {"mal", "ml|mal|malayalam"},
    {"kan", "kn|kan|kannada"}, {"mar", "mr|mar|marathi"}, {"pan", "pa|pan|punjabi"},
    {"urd", "ur|urd|urdu"}, {"guj", "gu|guj|gujarati"},
    {"dut", "nl|dut|nld|dutch|nederlands|olandese"}, {"swe", "sv|swe|swedish|svenska|svedese"},
    {"nor", "no|nor|nb|nob|nn|nno|norwegian|norsk|norvegese"},
    {"dan", "da|dan|danish|dansk|danese"}, {"fin", "fi|fin|finnish|suomi|finlandese"},
    {"pol", "pl|pol|polish|polski|polacco"}, {"cze", "cs|cze|ces|czech|čeština|cestina|ceco"},
    {"slo", "sk|slo|slk|slovak|slovenčina|slovencina|slovacco"},
    {"hun", "hu|hun|hungarian|magyar|ungherese"}, {"rum", "ro|rum|ron|romanian|română|romana|rumeno"},
    {"tur", "tr|tur|turkish|türkçe|turkce|turco"}, {"gre", "el|gre|ell|greek|greco|ελληνικά"},
    {"heb", "he|iw|heb|hebrew|ebraico|עברית"}, {"per", "fa|per|fas|persian|farsi|persiano"},
    {"tha", "th|tha|thai|thailandese"}, {"vie", "vi|vie|vietnamese"},
    {"ind", "id|ind|indonesian|indonesiano"}, {"may", "ms|may|msa|malay|malese"},
    {"fil", "tl|tgl|fil|tagalog|filipino"}, {"bul", "bg|bul|bulgarian|bulgaro"},
    {"hrv", "hr|hrv|croatian|hrvatski|croato"}, {"srp", "sr|srp|serbian|srpski|serbo"},
    {"bos", "bs|bos|bosnian|bosanski|bosniaco"}, {"slv", "sl|slv|slovenian|slovene|sloveno"},
    {"lit", "lt|lit|lithuanian|lituano"}, {"lav", "lv|lav|latvian|lettone"},
    {"est", "et|est|estonian|estone"}, {"ice", "is|ice|isl|icelandic|islandese"},
    {"alb", "sq|alb|sqi|albanian|albanese"}, {"mac", "mk|mac|mkd|macedonian|macedone"},
    {"cat", "ca|cat|catalan|català|catalano"}, {"baq", "eu|baq|eus|basque|basco"},
    {"glg", "gl|glg|galician|gallego"}, {"arm", "hy|arm|hye|armenian|armeno"},
    {"geo", "ka|geo|kat|georgian|georgiano"}, {"swa", "sw|swa|swahili"},
    {"afr", "af|afr|afrikaans"}, {"wel", "cy|wel|cym|welsh|gallese"},
    {"gle", "ga|gle|irish|irlandese"}, {"mul", "multi|multilang|multilingual|multiple languages|multi audio|mul"}
};
std::string language_code(std::string_view text) {
    auto token = ascii_lower(strip(text));
    // Explicit BCP-47 region/script subtags still describe the base language.
    const auto cut = token.find_first_of("-_");
    if (cut != std::string::npos && cut >= 2 && cut <= 3) token.resize(cut);
    for (const auto& language : kLanguages) {
        std::string_view aliases(language.aliases);
        for (size_t start = 0; start <= aliases.size();) {
            const auto end = aliases.find('|', start);
            const auto alias = aliases.substr(start, end == std::string_view::npos ? end : end - start);
            if (token == alias) return language.code;
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
    }
    return "";
}
void add_language(std::vector<std::string>& out, const std::string& code) {
    if (!code.empty() && out.size() < kLanguageLimit && std::find(out.begin(), out.end(), code) == out.end())
        out.push_back(code);
}
void language_text(std::vector<std::string>& out, std::string_view text) {
    if (auto code = language_code(text); !code.empty()) { add_language(out, code); return; }
    // Only called for explicitly labelled / structured language fields.
    size_t start = 0;
    while (start < text.size()) {
        while (start < text.size() && (std::isspace(static_cast<unsigned char>(text[start])) ||
               std::string_view(",;/|[]()+").find(text[start]) != std::string_view::npos)) ++start;
        size_t end = start;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) &&
               std::string_view(",;/|[]()+").find(text[end]) == std::string_view::npos) ++end;
        if (end > start) add_language(out, language_code(text.substr(start, end - start)));
        start = end;
    }
}
void language_value(std::vector<std::string>& out, const json& value) {
    if (value.is_string()) language_text(out, string_value(value));
    else if (value.is_array()) {
        const size_t count = std::min<size_t>(value.size(), 64);
        for (size_t i = 0; i < count; ++i) {
            if (value[i].is_string()) language_text(out, string_value(value[i]));
            else if (value[i].is_object()) {
                for (const char* key : {"code", "lang", "name"})
                    if (field(value[i], key).is_string()) {
                        language_text(out, string_value(field(value[i], key))); break;
                    }
            }
        }
    }
}
void language_fields(std::vector<std::string>& out, const json& object) {
    for (const char* key : {"audioLanguages", "audio_languages", "languages", "language"})
        language_value(out, field(object, key));
}
uint32_t next_utf8(std::string_view text, size_t& at) {
    if (at >= text.size()) return 0;
    const auto c = static_cast<unsigned char>(text[at++]);
    if (c < 0x80) return c;
    const int count = c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xc2 && c <= 0xdf ? 1 : 0;
    if (!count || at + count > text.size()) return 0xfffd;
    uint32_t value = c & ((1u << (6 - count)) - 1);
    for (int i = 0; i < count; ++i) {
        const auto more = static_cast<unsigned char>(text[at]);
        if ((more & 0xc0) != 0x80) return 0xfffd;
        ++at; value = (value << 6) | (more & 0x3f);
    }
    return value;
}
void flag_languages(std::vector<std::string>& out, std::string_view text) {
    // Addons conventionally use country flags to label stream languages.
    // This mapping is not used for countryWhitelist or inferred from URLs.
    static const std::pair<const char*, const char*> flags[] = {
        {"IT","ita"},{"GB","eng"},{"US","eng"},{"AU","eng"},{"NZ","eng"},
        {"FR","fre"},{"DE","ger"},{"AT","ger"},{"ES","spa"},{"MX","spa"},{"AR","spa"},
        {"CO","spa"},{"CL","spa"},{"PT","por"},{"BR","por"},{"JP","jpn"},{"KR","kor"},
        {"CN","chi"},{"TW","chi"},{"HK","yue"},{"RU","rus"},{"UA","ukr"},
        {"SA","ara"},{"AE","ara"},{"EG","ara"},{"IN","hin"},{"BD","ben"},{"PK","urd"},
        {"NL","dut"},{"SE","swe"},{"NO","nor"},{"DK","dan"},{"FI","fin"},{"PL","pol"},
        {"CZ","cze"},{"SK","slo"},{"HU","hun"},{"RO","rum"},{"TR","tur"},{"GR","gre"},
        {"IL","heb"},{"IR","per"},{"TH","tha"},{"VN","vie"},{"ID","ind"},{"MY","may"},
        {"PH","fil"},{"BG","bul"},{"HR","hrv"},{"RS","srp"},{"BA","bos"},{"SI","slv"},
        {"LT","lit"},{"LV","lav"},{"EE","est"},{"IS","ice"},{"AL","alb"},{"MK","mac"},
        {"AM","arm"},{"GE","geo"}
    };
    for (size_t at = 0; at < text.size();) {
        const auto first = next_utf8(text, at);
        if (first < 0x1f1e6 || first > 0x1f1ff) continue;
        size_t second_at = at;
        const auto second = next_utf8(text, second_at);
        if (second < 0x1f1e6 || second > 0x1f1ff) continue;
        at = second_at;
        const char country[3] = {char('A' + first - 0x1f1e6), char('A' + second - 0x1f1e6), 0};
        for (const auto& flag : flags) if (std::string_view(flag.first) == country) { add_language(out, flag.second); break; }
    }
}
void display_languages(std::vector<std::string>& out, std::string_view text, bool release_tags) {
    flag_languages(out, text);
    const auto lowered = ascii_lower(text);
    for (const char* label : {"audio:", "audio languages:", "languages:", "language:", "langs:", "lang:", "lingue:", "lingua:"}) {
        size_t pos = 0;
        while ((pos = lowered.find(label, pos)) != std::string::npos) {
            const size_t begin = pos; pos += std::char_traits<char>::length(label);
            if (begin && word(lowered[begin - 1])) continue;
            const auto end = lowered.find_first_of("\r\n|", pos);
            language_text(out, std::string_view(text).substr(pos, end == std::string::npos ? end : end - pos));
        }
    }
    if (!release_tags) return;
    // Release tags such as ITA.ENG are useful without mistaking the movie "It"
    // or the title "The English Patient" for audio metadata. Two-letter tags
    // require an explicit audio label above; unlabelled tags must be uppercase
    // ISO3, and be adjacent to release delimiters rather than ordinary prose.
    for (size_t start = 0; start < text.size();) {
        while (start < text.size() && !word(text[start])) ++start;
        size_t end = start;
        while (end < text.size() && word(text[end])) ++end;
        const auto token = text.substr(start, end - start);
        const bool upper = token.size() >= 3 && std::all_of(token.begin(), token.end(), [](unsigned char c) { return c >= 'A' && c <= 'Z'; });
        const bool tagged = (start && std::string_view(".[(/+|").find(text[start - 1]) != std::string_view::npos) ||
                            (end < text.size() && std::string_view(".])/+|").find(text[end]) != std::string_view::npos);
        if (upper && tagged && (token.size() == 3 || token == "MULTI")) add_language(out, language_code(token));
        start = end + (end == start);
    }
}

int64_t count_value(std::string text) {
    text = strip(text);
    if (text.empty() || text.size() > 40) return -1;
    uint64_t multiplier = 1;
    if (text.back() == 'k' || text.back() == 'K') { multiplier = 1000; text.pop_back(); }
    else if (text.back() == 'm' || text.back() == 'M') { multiplier = 1000000; text.pop_back(); }
    text = strip(text);
    if (text.empty() || text.front() < '0' || text.front() > '9') return -1;
    const auto separator = text.find_first_of(",. ");
    bool decimal = false;
    if (separator != std::string::npos && multiplier > 1 && text[separator] != ' ' &&
        text.find_first_of(",. ", separator + 1) == std::string::npos && text.size() - separator - 1 <= 2)
        decimal = true;
    uint64_t integer = 0, fraction = 0, denominator = 1;
    if (decimal) {
        for (size_t i = 0; i < text.size(); ++i) {
            if (i == separator) continue;
            if (text[i] < '0' || text[i] > '9') return -1;
            if (i < separator) integer = integer * 10 + text[i] - '0';
            else { fraction = fraction * 10 + text[i] - '0'; denominator *= 10; }
            if (integer > uint64_t(std::numeric_limits<int64_t>::max()) / multiplier) return -1;
        }
    } else {
        size_t group = 0;
        char grouping = 0;
        for (char c : text) {
            if (c == ',' || c == '.' || c == ' ') {
                if (!group || (!grouping && group > 3) || (grouping && (c != grouping || group != 3))) return -1;
                grouping = c; group = 0; continue;
            }
            if (c < '0' || c > '9') return -1;
            const uint64_t digit = c - '0';
            if (integer > (uint64_t(std::numeric_limits<int64_t>::max()) / multiplier - digit) / 10) return -1;
            integer = integer * 10 + digit; ++group;
        }
        if (grouping && group != 3) return -1;
    }
    const uint64_t extra = fraction * multiplier / denominator;
    const uint64_t maximum = std::numeric_limits<int64_t>::max();
    if (integer > (maximum - extra) / multiplier) return -1;
    return int64_t(integer * multiplier + extra);
}
int64_t count_value(const json& value) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        return number <= uint64_t(std::numeric_limits<int64_t>::max()) ? int64_t(number) : -1;
    }
    if (value.is_number_integer()) return std::max<int64_t>(-1, value.get<int64_t>());
    if (value.is_string()) return count_value(value.get_ref<const std::string&>());
    return -1;
}
int64_t object_seeders(const json& object) {
    for (const char* key : {"seeders", "seeds", "seedCount", "seed_count"}) {
        auto count = count_value(field(object, key));
        if (count >= 0) return count;
    }
    return -1;
}
int64_t count_after(std::string_view text, size_t pos) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == ':' || text[pos] == '=')) ++pos;
    if (text.substr(pos, 3) == "\xef\xb8\x8f") pos += 3;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
    const size_t start = pos;
    while (pos < text.size() && ((text[pos] >= '0' && text[pos] <= '9') || text[pos] == ',' || text[pos] == '.')) ++pos;
    // A space-separated group is numeric only with exactly three digits.
    while (pos + 3 < text.size() && text[pos] == ' ' && std::isdigit(static_cast<unsigned char>(text[pos + 1])) &&
           std::isdigit(static_cast<unsigned char>(text[pos + 2])) && std::isdigit(static_cast<unsigned char>(text[pos + 3])) &&
           (pos + 4 == text.size() || !std::isdigit(static_cast<unsigned char>(text[pos + 4])))) pos += 4;
    size_t unit = pos;
    while (unit < text.size() && (text[unit] == ' ' || text[unit] == '\t')) ++unit;
    if (unit < text.size() && (text[unit] == 'k' || text[unit] == 'K' || text[unit] == 'm' || text[unit] == 'M')) pos = unit + 1;
    if (start == pos || !edge(text, pos)) return -1;
    return count_value(std::string(text.substr(start, pos - start)));
}
int64_t display_seeders(std::string_view text) {
    const auto lowered = ascii_lower(text);
    for (const char* marker : {"seeders", "seeder", "seeds", "seed count", "👤", "👥", "🌱"}) {
        size_t pos = 0;
        while ((pos = lowered.find(marker, pos)) != std::string::npos) {
            const auto begin = pos; pos += std::char_traits<char>::length(marker);
            const bool letters = static_cast<unsigned char>(marker[0]) < 0x80;
            if (letters && ((begin && word(lowered[begin - 1])) || !edge(lowered, pos))) continue;
            const auto number = count_after(text, pos);
            if (number >= 0) return number;
            if (!letters) continue;
            // The other common form is "123 seeders".
            size_t last = begin;
            while (last && (text[last - 1] == ' ' || text[last - 1] == '\t')) --last;
            size_t first = last;
            while (first && (std::isdigit(static_cast<unsigned char>(text[first - 1])) ||
                   std::string_view(",.kKmM").find(text[first - 1]) != std::string_view::npos)) --first;
            if (first < last && (!first || !word(text[first - 1]))) {
                const auto previous = count_value(std::string(text.substr(first, last - first)));
                if (previous >= 0) return previous;
            }
        }
    }
    return -1;
}

std::string quality(std::string_view text) {
    const auto s = ascii_lower(text);
    for (const char* q : {"2160p", "4k", "uhd"}) if (has_word(s, q)) return "4K";
    for (const char* q : {"1440p", "2k"}) if (has_word(s, q)) return "1440p";
    for (const char* q : {"1080p", "1080i", "fhd"}) if (has_word(s, q)) return "1080p";
    if (has_word(s, "720p")) return "720p";
    for (const char* q : {"576p", "480p", "360p", "240p", "dvdrip", "dvdscr", "sd"}) if (has_word(s, q)) return "SD";
    return "";
}
uint64_t byte_size(const json& value) {
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    if (value.is_number_integer()) return uint64_t(std::max<int64_t>(0, value.get<int64_t>()));
    return 0;
}
std::string display_size(uint64_t bytes) {
    if (!bytes) return "";
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    unsigned unit = 0;
    double value = double(bytes);
    while (value >= 1024 && unit < 6) { value /= 1024; ++unit; }
    char text[48];
    if (!unit) std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
    else std::snprintf(text, sizeof(text), value < 10 ? "%.2f %s" : "%.1f %s", value, units[unit]);
    return text;
}
std::string text_size(std::string_view text) {
    const auto lowered = ascii_lower(text);
    for (size_t start = 0; start < text.size(); ++start) {
        if (!std::isdigit(static_cast<unsigned char>(text[start])) || (start && word(text[start - 1]))) continue;
        size_t end = start;
        bool dot = false;
        while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) ||
               (!dot && (text[end] == '.' || text[end] == ',')))) {
            if (text[end] == '.' || text[end] == ',') dot = true;
            ++end;
        }
        const auto number_end = end;
        while (end < text.size() && (text[end] == ' ' || text[end] == '\t')) ++end;
        for (const char* unit : {"tib", "gib", "mib", "kib", "tb", "gb", "mb", "kb"}) {
            const size_t length = std::char_traits<char>::length(unit);
            if (lowered.compare(end, length, unit) != 0 || !edge(text, end + length)) continue;
            if (lowered.compare(end + length, 2, "/s") == 0) continue; // bandwidth, not file size
            std::string name(unit);
            name.front() -= 'a' - 'A'; name.back() = 'B';
            return std::string(text.substr(start, number_end - start)) + " " + name;
        }
        start = number_end ? number_end - 1 : start;
    }
    return "";
}
bool debrid_service(std::string text) {
    text = ascii_lower(strip(text));
    for (const char* value : {"realdebrid", "real-debrid", "alldebrid", "all-debrid", "premiumize", "premiumize.me",
                             "debridlink", "debrid-link", "torbox", "offcloud", "easydebrid", "debrider", "pikpak", "putio", "put.io", "seedr"})
        if (text == value) return true;
    return false;
}
std::string stream_type(StreamKind kind) {
    switch (kind) {
    case StreamKind::Direct: return "direct";
    case StreamKind::Torrent: return "torrent";
    case StreamKind::YouTube: return "youtube";
    case StreamKind::External: case StreamKind::PlayerFrame: return "external";
    case StreamKind::Nzb: return "usenet";
    case StreamKind::Rar: case StreamKind::Zip: case StreamKind::SevenZip:
    case StreamKind::Tar: case StreamKind::Tgz: return "archive";
    default: return "unavailable";
    }
}
} // namespace

StreamPresentation stream_presentation(const Stream& stream) {
    StreamPresentation out;
    out.type = stream_type(stream.kind);
    const json& data = field(stream.raw, "streamData"); // AIOStreams' optional structured extension.
    const json& parsed = field(data, "parsedFile");
    const json& service = field(data, "service");
    language_fields(out.languages, parsed);
    language_fields(out.languages, data);
    language_fields(out.languages, stream.raw);
    language_fields(out.languages, stream.behavior_hints);
    display_languages(out.languages, stream.name.substr(0, kTextLimit), true);
    display_languages(out.languages, stream.description.substr(0, kTextLimit), true);
    display_languages(out.languages, stream.filename.substr(0, kTextLimit), true);
    if (out.languages.size() > 1) out.languages.erase(std::remove(out.languages.begin(), out.languages.end(), "mul"), out.languages.end());

    for (const json* object : {&field(data, "torrent"), &data, &stream.raw, &stream.behavior_hints}) {
        out.seeders = object_seeders(*object);
        if (out.seeders >= 0) break;
    }
    if (out.seeders < 0) out.seeders = display_seeders(stream.description.substr(0, kTextLimit));
    if (out.seeders < 0) out.seeders = display_seeders(stream.name.substr(0, kTextLimit));
    // The filename is deliberately never a seeder-count source.
    out.quality = quality(string_value(field(parsed, "resolution")));
    if (out.quality.empty()) out.quality = quality(string_value(field(data, "resolution")));
    if (out.quality.empty()) out.quality = quality(stream.name + " " + stream.filename + " " + stream.description);
    uint64_t size = stream.has_video_size ? stream.video_size : 0;
    if (!size) size = byte_size(field(data, "size"));
    if (!size) size = byte_size(field(stream.raw, "size"));
    out.size = display_size(size);
    if (out.size.empty()) out.size = text_size(stream.description.substr(0, kTextLimit));
    if (out.size.empty()) out.size = text_size(stream.name.substr(0, kTextLimit));

    if (stream.kind == StreamKind::Direct) {
        const auto supplied_type = ascii_lower(string_value(field(data, "type")));
        bool debrid = supplied_type == "debrid" || debrid_service(string_value(field(service, "id"))) ||
                      debrid_service(string_value(service)) || debrid_service(string_value(field(stream.raw, "service")));
        bool marker_cached = false;
        const auto name = ascii_lower(stream.name.substr(0, kTextLimit));
        // These are explicit service markers used in stream names, not addon
        // names. Plain "Torrentio" / "Comet" do not indicate a debrid service.
        for (const char* prefix : {"rd", "ad", "pm", "dl", "tb", "oc", "ed", "pp"}) {
            const std::string bracket = "[" + std::string(prefix);
            size_t pos = 0;
            while ((pos = name.find(bracket, pos)) != std::string::npos) {
                pos += bracket.size();
                if (pos < name.size() && (name[pos] == ']' || name[pos] == '+' || name[pos] == ' ')) {
                    const auto end = name.find(']', pos);
                    if (end == std::string::npos || end - pos > 24) continue;
                    debrid = true;
                    if (name[pos] == '+') marker_cached = true;
                }
            }
        }
        for (const char* value : {"real-debrid", "realdebrid", "all-debrid", "alldebrid", "premiumize", "torbox", "debrid-link", "debridlink", "easydebrid"})
            if (has_word(name, value)) debrid = true;
        if (debrid) {
            out.type = "debrid";
            int explicit_cache = -1;
            for (const json* object : {&service, &data, &stream.raw, &stream.behavior_hints}) {
                const auto& cached = field(*object, "cached");
                if (cached.is_boolean()) { explicit_cache = cached.get<bool>() ? 1 : 0; break; }
            }
            if (explicit_cache >= 0) out.cached = explicit_cache == 1;
            else if (!has_word(name, "uncached") && !has_word(name, "download") && name.find("not cached") == std::string::npos)
                out.cached = marker_cached || has_word(name, "cached") || name.find("⚡") != std::string::npos;
        } else if (supplied_type == "usenet" || supplied_type == "stremio-usenet") out.type = "usenet";
    }
    return out;
}
