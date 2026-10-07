#include "stream_presentation.h"
#include "stremio.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int checks = 0;
void expect(bool value, const char* description) {
    ++checks;
    if (!value) throw std::runtime_error(description);
}
bool language(const StreamPresentation& p, const std::string& value) {
    return std::find(p.languages.begin(), p.languages.end(), value) != p.languages.end();
}
Stream torrent() {
    Stream stream;
    stream.kind = StreamKind::Torrent;
    stream.info_hash = "0123456789012345678901234567890123456789";
    return stream;
}
Stream direct() {
    Stream stream;
    stream.kind = StreamKind::Direct;
    stream.url = "https://cdn.example/private/token/2160p/ita?seeders=900&service=realdebrid";
    return stream;
}
void conventional_labels() {
    auto stream = torrent();
    stream.name = "Torrentio\n4K HDR";
    stream.description = "Example.2026.2160p.ITA.ENG.mkv\n👤 1,234 💾 24.6 GB\n🇮🇹 / 🇬🇧";
    auto p = stream_presentation(stream);
    expect(p.seeders == 1234, "Torrentio emoji count is readable");
    expect(p.type == "torrent", "native infoHash remains torrent");
    expect(p.quality == "4K" && p.size == "24.6 GB", "quality and size badges parsed independently");
    expect(p.languages == std::vector<std::string>({"ita", "eng"}), "flags and release tags deduplicate languages");
    stream.name = "AIOStreams [RD+]\n1080p";
    stream.kind = StreamKind::Direct;
    p = stream_presentation(stream);
    expect(p.type == "debrid" && p.cached, "explicit RD+ marker identifies provider-reported cache");
    stream.name = "Comet [RD download] 1080p";
    p = stream_presentation(stream);
    expect(p.type == "debrid" && !p.cached, "download marker is not cached");
    stream.name = "[TB+] 4K uncached";
    expect(!stream_presentation(stream).cached, "uncached overrides a positive display marker");
    stream.name = "Torrentio";
    stream.addon = "Torrentio + Real-Debrid";
    expect(stream_presentation(stream).type == "direct", "addon name does not imply debrid");
    stream.name = "Ordinary stream";
    stream.description = "English Patient (1996)\nIt (2017)\nAn Italian Job\n1080p 7.1 8 GB 2026";
    stream.filename = "Seeders.1996.It.EN.mkv";
    p = stream_presentation(stream);
    expect(p.languages.empty(), "movie titles and ambiguous two-letter filename tokens are not languages");
    expect(p.seeders == -1, "years, sizes and filename digits are not seeds");
    expect(!p.cached && p.type == "direct", "credentials and URL query fields are not badge inputs");
}
void structured_extensions() {
    auto stream = direct();
    stream.name = "Whatever a custom formatter chooses";
    stream.raw = json{{"streamData", {
        {"type", "debrid"}, {"service", {{"id", "realdebrid"}, {"cached", true}}},
        {"torrent", {{"seeders", 0}}}, {"size", uint64_t(50) << 30},
        {"parsedFile", {{"resolution", "2160p"}, {"languages", {"Italian", "eng", "deu", "fr-FR", "pt-BR"}}}}
    }}};
    auto p = stream_presentation(stream);
    expect(p.type == "debrid" && p.cached, "AIO structured service metadata works without text markers");
    expect(p.seeders == 0, "a structured zero stays zero");
    expect(p.quality == "4K" && p.size == "50.0 GiB", "structured size uses full 64-bit range");
    expect(p.languages == std::vector<std::string>({"ita", "eng", "ger", "fre", "por"}), "names, aliases and BCP47 normalize to ISO3");
    stream.name = "[RD+]";
    stream.raw["streamData"]["service"]["cached"] = false;
    expect(!stream_presentation(stream).cached, "structured false overrides cached display text");
    stream.raw["streamData"]["type"] = "usenet";
    stream.raw["streamData"].erase("service");
    stream.name = "AIO";
    expect(stream_presentation(stream).type == "usenet", "HTTP usenet extension receives correct transport badge");
    stream = direct();
    stream.raw = json{{"seeders", std::numeric_limits<uint64_t>::max()}, {"languages", {false, 4, json::object()}}};
    stream.behavior_hints = json{{"seeders", "28"}, {"countryWhitelist", {"ita", "fra"}}};
    stream.subtitles.push_back(SubtitleTrack{"it", "https://sub.example/it.srt", "ita", "Italian", "Fixture", {}});
    p = stream_presentation(stream);
    expect(p.seeders == 28, "overflowing malformed fields fall back safely");
    expect(p.languages.empty(), "subtitle tracks and country whitelist are not audio languages");
    stream.raw = json::array({3, "bad"});
    stream.behavior_hints = "bad";
    expect(stream_presentation(stream).seeders == -1, "non-object extensions are tolerated");
}
void numeric_guards() {
    auto stream = torrent();
    const std::pair<const char*, int64_t> cases[] = {
        {"👥 1.2k", 1200}, {"Seeds: 0", 0}, {"Seeders = 1 234", 1234}, {"Seeds: 1.234", 1234},
        {"👤 1,25K", 1250}, {"42 seeders", 42}, {"Seeders: 4.6 GB", -1}, {"👥 1080p", -1},
        {"Seeds: 99MB", -1}, {"Seeders: -5", -1}, {"Seeders: 1.2", -1},
        {"Seeders: 9223372036854775807", std::numeric_limits<int64_t>::max()},
        {"Seeders: 9223372036854775808", -1}, {"Seeders: 9999999999999999999999999", -1},
        {"Seeders: 1234,567", -1}, {"Seeds: N/A", -1}, {"No seeders available (1080p)", -1},
        {"Seeders: 1,234.567", -1}, {"Seeders: 1.2MB", -1}
    };
    for (const auto& item : cases) {
        stream.description = item.first;
        if (stream_presentation(stream).seeders != item.second) throw std::runtime_error(std::string("count guard: ") + item.first);
        ++checks;
    }
    stream.description = "Bitrate: 10 MB/s\nSize: 1.20 GiB";
    expect(stream_presentation(stream).size == "1.20 GiB", "bandwidth is not file size");
    stream.description = "film4kremux abc1080pfoo";
    expect(stream_presentation(stream).quality.empty(), "resolution substrings are not badges");
    stream.description = "Language: Italiano, en-US, deu / 日本語\nAudio: Español | Codec: AAC";
    auto p = stream_presentation(stream);
    expect(language(p,"ita") && language(p,"eng") && language(p,"ger") && language(p,"jpn") && language(p,"spa"), "explicit multilingual fields and native names parse");
    stream.description = "🇵🇹 🇧🇷 🇬🇧 🇺🇸 🇫🇷";
    expect(stream_presentation(stream).languages == std::vector<std::string>({"por", "eng", "fre"}), "regional flag duplicates remain readable single language badges");
    stream.description = "\xff\xf0\x9f\x87";
    expect(stream_presentation(stream).languages.empty(), "truncated or invalid Unicode flags are ignored");
    stream.description = "Movie.MULTI.mkv";
    expect(stream_presentation(stream).languages == std::vector<std::string>({"mul"}), "MULTI stays unspecified multilingual, not invented tracks");
    stream.description += "\n🇮🇹 🇬🇧";
    expect(stream_presentation(stream).languages == std::vector<std::string>({"ita", "eng"}), "explicit languages replace generic MULTI");
    stream.kind = StreamKind::YouTube;
    expect(stream_presentation(stream).type == "youtube", "YouTube is distinct from direct video");
    stream.kind = StreamKind::Nzb;
    expect(stream_presentation(stream).type == "usenet", "NZB transport is explicit");
    stream.kind = StreamKind::Zip;
    expect(stream_presentation(stream).type == "archive", "archive transport is explicit");
    stream.kind = StreamKind::External;
    expect(stream_presentation(stream).type == "external", "external playback is explicit");
}
} // namespace

int main() {
    try {
        conventional_labels(); structured_extensions(); numeric_guards();
        std::cout << "PASS " << checks << " stream presentation checks (offline; no provider accounts).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
