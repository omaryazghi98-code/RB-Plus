#include "stremio.h"
#include "http.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
int checks = 0;
void expect(bool value, const char* message) {
	++checks;
	if (!value) throw std::runtime_error(message);
}

json manifest() {
	return json{{"id", "test.fixture"}, {"name", "Fixture"}, {"version", "1.0.0"},
	            {"types", {"movie", "series"}}, {"idPrefixes", {"tt"}},
	            {"resources", {"stream", "meta", "subtitles"}}, {"catalogs", json::array()}};
}

void manifest_routing() {
	Addon addon;
	auto input = manifest();
	expect(parse_addon(input, "https://addon.example/config/manifest.json", addon), "manifest parses");
	expect(addon.supports("stream", "movie", "tt0123456"), "short resources use global types/prefixes");
	expect(!addon.supports("stream", "movie", "kitsu:123"), "global prefix excludes unrelated ID");
	expect(!addon.supports("stream", "other", "tt0123456"), "global types exclude other type");
	input["resources"] = json::array({json{{"name", "stream"}, {"types", {"series"}}}});
	expect(parse_addon(input, addon.transport_url, addon), "full resource reparses");
	expect(addon.resources.size() == 1, "reparse replaces resources instead of appending");
	expect(addon.supports("stream", "series", "kitsu:123:1"), "full resource without prefixes ignores global tt filter");
	expect(!addon.supports("stream", "movie", "tt0123456"), "full resource types remain independent");
	input["resources"][0]["idPrefixes"] = json::array();
	expect(parse_addon(input, addon.transport_url, addon) && addon.supports("stream", "series", "mal:5"),
	       "explicitly empty prefix list accepts all IDs");
	input["resources"][0]["types"] = json::array();
	expect(parse_addon(input, addon.transport_url, addon) && !addon.supports("stream", "series", "tt5"),
	       "empty full types is not an all-types wildcard");
	input["resources"][0].erase("types");
	expect(parse_addon(input, addon.transport_url, addon) && !addon.supports("stream", "series", "tt5"),
	       "missing full types follows Stremio core behavior");

	// Shape from Torrentio's actual manifest builder: Debrid catalogs exist
	// even though resources contains only stream/meta and the type is other.
	input = manifest();
	input["resources"] = json::array({json{{"name", "stream"}, {"types", {"movie", "series", "anime"}},
	                                             {"idPrefixes", {"tt", "kitsu"}}}});
	input["catalogs"] = json::array({json{{"id", "torrentio-service-downloads"}, {"type", "other"},
	    {"name", "Downloads"}, {"extra", json::array({json{{"name", "skip"}}})}}});
	expect(parse_addon(input, "https://addon.example/private-config/manifest.json", addon), "Torrentio-shaped manifest parses");
	expect(addon.supports("catalog", "other", "torrentio-service-downloads"), "catalog does not need resources.catalog");
	expect(!addon.supports("catalog", "movie", "torrentio-service-downloads"), "catalog type must match");
	expect(addon.supports("stream", "anime", "kitsu:123:4"), "Torrentio anime resource is routed");
	expect(!addon.supports("stream", "movie", "tmdb:123"), "unsupported provider ID is not routed speculatively");
	const auto previous_url = addon.transport_url;
	expect(!parse_addon(json{{"id", 9}}, "https://addon.example/manifest.json", addon), "invalid manifest is rejected");
	expect(addon.transport_url == previous_url && addon.catalogs.size() == 1, "failed parse leaves previous state intact");
	addon.from_account = true;
	expect(parse_addon(input, previous_url, addon) && addon.from_account && addon.catalogs.size() == 1,
	       "reparse preserves account provenance and replaces catalogs");

	input["addonCatalogs"] = json::array({json{{"type", "all"}, {"id", "collection"}}});
	input["behaviorHints"] = json{{"configurable", true}, {"configurationRequired", true}, {"epgProvider", true}};
	expect(parse_addon(input, previous_url, addon) && addon.supports("addon_catalog", "all", "collection"),
	       "addon_catalog uses its own declarations");
	expect(addon.configurable && addon.configuration_required && addon.epg_provider, "manifest behavior hints retained");
}

void catalog_extras_and_urls() {
	Addon addon;
	auto input = manifest();
	input["catalogs"] = json::array({json{{"type", "series"}, {"id", "custom/catalog"},
	    {"extra", json::array({json{{"name", "genre"}, {"isRequired", true}, {"options", {"Drama", "Anime"}}, {"optionsLimit", 2}},
	                           json{{"name", "search"}}, json{{"name", "skip"}}})}}});
	expect(parse_addon(input, "https://addon.example/s/token/config%2Fopaque/manifest.json?auth=A%2BB#ignored", addon), "configured URL parses");
	const Catalog& catalog = addon.catalogs.front();
	expect(catalog.extra("genre")->options_limit == 2, "optionsLimit is preserved");
	expect(!catalog.supports_extras({}), "required catalog extra cannot be omitted");
	expect(catalog.supports_extras({{"genre", "Drama"}, {"genre", "Anime"}}), "multiple options respect optionsLimit");
	expect(!catalog.supports_extras({{"genre", "Drama"}, {"genre", "Drama"}, {"genre", "Drama"}}), "optionsLimit is enforced");
	expect(!catalog.supports_extras({{"genre", "Unknown"}}), "enum values come from manifest");
	expect(!catalog.supports_extras({{"genre", "Drama"}, {"unsupported", "1"}}), "undeclared extra is rejected");
	expect(catalog.default_extras() == AddonExtras{{"genre", "Drama"}}, "required enum gets explicit first default");
	expect(addon.configure_url() == "https://addon.example/s/token/config%2Fopaque/configure?auth=A%2BB", "configuration URL preserves token path and query");
	const auto encoded = addon.resource_url_with_extras("catalog", "custom/type", "id:/%è", {{"search", "Lupin & Jigen+ #="}, {"skip", "100"}});
	expect(encoded == "https://addon.example/s/token/config%2Fopaque/catalog/custom%2Ftype/id%3A%2F%25%C3%A8/search=Lupin%20%26%20Jigen%2B%20%23%3D&skip=100.json?auth=A%2BB",
	       "path components and extras encoded once; configured prefix/query unchanged");
	expect(addon.resource_url("stream", "series", "kitsu:123:4") ==
	       "https://addon.example/s/token/config%2Fopaque/stream/series/kitsu%3A123%3A4.json?auth=A%2BB", "video ID stays in its own ID space");
	expect(addon.resource_url("catalog", "series", "x", "search=../x").empty(), "legacy raw extras cannot insert path separators");
	expect(addon.resource_url("catalog", "series", "x", "search=x%2Fy").find("search=x%2Fy.json") != std::string::npos,
	       "already-encoded legacy extras are not encoded twice");
	expect(addon.resource_url("catalog", "series", "x", "search=x%ZZ").empty(), "malformed percent escape rejected");

	expect(normalize_addon_url("  stremio://addon.example/config/manifest.json  ") == "https://addon.example/config/manifest.json", "stremio install URL normalized");
	expect(normalize_addon_url("https://addon.example/configure?key=opaque%2Ftoken") == "https://addon.example/manifest.json?key=opaque%2Ftoken", "configure endpoint resolves to sibling manifest");
	expect(normalize_addon_url("addon.example/config/") == "https://addon.example/config/manifest.json", "bare hostname/config is supported");
	expect(normalize_addon_url("http://127.0.0.1:11470/config/manifest.json/#fragment") == "http://127.0.0.1:11470/config/manifest.json", "explicit LAN HTTP and trailing slash preserved");
	expect(normalize_addon_url("https://addon.example/config/manifest.json?token=x&flag=y") ==
	       "https://addon.example/config/manifest.json?token=x&flag=y", "query is not mistaken for manifest path");
	expect(normalize_addon_url("ipfs://hash/manifest.json").empty(), "unsupported IPFS does not get fake https prefix");
	expect(normalize_addon_url("file:///etc/example").empty(), "file transport cannot be installed");
	expect(normalize_addon_url("https://addon.example/path\r\nX-Test: bad").empty(), "control characters cannot enter URL");
	expect(normalize_addon_url("https://").empty(), "missing hostname rejected");
	expect(normalize_addon_url("https://addon.example:70000/").empty(), "invalid port rejected");
	expect(http_valid_url("https://[::1]:11470/x"), "bracketed IPv6 URL syntax accepted");
	expect(http_log_target("https://user:password@addon.example/token/manifest.json?secret=value") ==
	       "https://addon.example/[redacted]", "logs do not expose credentials/configuration/query");

	input["catalogs"][0].erase("extra");
	input["catalogs"][0]["extraSupported"] = {"search", "skip"};
	input["catalogs"][0]["extraRequired"] = {"search"};
	expect(parse_addon(input, "https://addon.example/manifest.json", addon), "legacy extra shape parses");
	expect(addon.catalogs[0].requires_other_than("genre") && addon.catalogs[0].supports_extras({{"search", "query"}}),
	       "search-only catalog remains search-only with legacy extras");
	input["catalogs"][0]["extra"] = json::array();
	expect(parse_addon(input, addon.transport_url, addon) && addon.catalogs[0].extras.empty(),
	       "explicit modern extras take precedence over obsolete legacy declarations");
}

const std::string hash = "0123456789abcdef0123456789abcdef01234567";

void streams_and_subtitles() {
	// Generic direct-stream shape used by configured aggregators, including
	// opaque redirect URLs without a filename extension.
	json direct{{"name", "Provider\n4K HDR"}, {"description", "Titolo 🎬\nSeconda riga"}, {"title", "Old title"},
	    {"url", "https://media.example/playback/opaque?signature=A%2FB&expires=42"}, {"infoHash", hash},
	    {"behaviorHints", {{"filename", "Film & episode+1.mkv"}, {"bingeGroup", "provider|2160p"}, {"notWebReady", true},
	        {"videoHash", "0123456789abcdef"}, {"videoSize", uint64_t(50000000000ULL)}, {"countryWhitelist", {"ita"}},
	        {"proxyHeaders", {{"request", {{"User-Agent", "Fixture Player"}, {"Referer", "https://media.example/"}}},
	                          {"response", {{"Content-Type", "video/mp4"}}}}}, {"futureHint", {{"value", 7}}}}},
	    {"subtitles", json::array({json{{"id", "it1"}, {"url", "https://sub.example/ita.srt"}, {"lang", "it-IT"}, {"label", "Italiano [CC]"}}})}};
	auto streams = parse_streams(json{{"streams", json::array({direct})}}, "AIOStreams");
	expect(streams.size() == 1 && streams[0].kind == StreamKind::Direct, "direct URL wins over descriptive torrent hash");
	const auto& stream = streams[0];
	expect(stream.description == "Titolo 🎬\nSeconda riga", "modern stream description and UTF-8 preserved");
	expect(stream.url == direct["url"] && stream.not_web_ready && stream.playable(), "native playback accepts notWebReady URL without extension");
	expect(stream.filename == "Film & episode+1.mkv" && stream.binge_group == "provider|2160p", "filename and binge identity preserved");
	expect(stream.video_size == 50000000000ULL && stream.has_video_size && stream.video_hash == "0123456789abcdef", "subtitle parameters preserve size beyond 32 bits");
	expect(stream.request_headers.size() == 2 && stream.response_headers == std::vector<std::string>{"Content-Type: video/mp4"},
	       "request and response proxy headers stay separate");
	expect(stream.subtitles.size() == 1 && stream.subtitles[0].label == "Italiano [CC]", "inline subtitle label is not discarded");
	expect(stream.behavior_hints["futureHint"]["value"] == 7 && stream.raw == direct, "unknown fields and source object retained");
	expect(encode_addon_extras(stream_subtitle_extras(stream)) ==
	       "videoHash=0123456789abcdef&videoSize=50000000000&filename=Film%20%26%20episode%2B1.mkv", "subtitle extras use video hash, never torrent infoHash");
	const auto proxy = stream_proxy_url(stream, "http://127.0.0.1:11470/prefix/");
	expect(proxy.find("/prefix/proxy/d=https%3A%2F%2Fmedia.example&h=") != std::string::npos &&
	       proxy.find("&r=Content-Type%3Avideo%2Fmp4/") != std::string::npos,
	       "service proxy distinguishes request headers from response overrides");
	expect(proxy.find("/playback/opaque?signature=A%2FB&expires=42") != std::string::npos &&
	       proxy.find("User-Agent%3AFixture%20Player") != std::string::npos,
	       "proxy retains media path/query encoding and serializes headers safely");
	expect(stream_proxy_url(stream, "file:///tmp").empty() && stream_proxy_url(stream, "http://localhost/?token=x").empty(),
	       "invalid proxy base cannot turn path into a query or a local-file access");

	json torrent{{"name", "Torrentio"}, {"title", "1080p\nrelease"}, {"infoHash", hash}, {"fileIdx", 0},
	    {"sources", {"tracker:udp://tracker.example:6969/announce", "dht:" + hash}},
	    {"announce", {"https://tracker.example/announce"}}};
	streams = parse_streams(json::array({torrent}), "Torrentio");
	expect(streams.size() == 1 && streams[0].file_idx == 0 && streams[0].kind == StreamKind::Torrent, "torrent file index zero is valid");
	expect(streams[0].sources.size() == 3 && streams[0].sources[1] == "dht:" + hash, "tracker and DHT sources are preserved; announce alias merged");
	torrent.erase("fileIdx");
	expect(parse_streams(json::array({torrent}), "P2P")[0].file_idx == -1, "missing torrent index remains automatic");
	torrent["fileIdx"] = nullptr;
	expect(parse_streams(json::array({torrent}), "P2P")[0].file_idx == -1, "null torrent index remains automatic");
	torrent["fileIdx"] = -1;
	expect(parse_streams(json::array({torrent}), "P2P")[0].file_idx == -1, "conventional -1 automatic sentinel accepted");
	torrent["fileIdx"] = -2;
	expect(parse_streams(json::array({torrent}), "P2P").empty(), "invalid negative index does not select an unintended file");
	torrent["fileIdx"] = 1.5;
	expect(parse_streams(json::array({torrent}), "P2P").empty(), "fractional index is not truncated");
	torrent["fileIdx"] = uint64_t(1ULL << 40);
	expect(parse_streams(json::array({torrent}), "P2P").empty(), "out-of-range index cannot overflow");
	torrent["fileIdx"] = "0";
	expect(parse_streams(json::array({torrent}), "P2P")[0].file_idx == 0, "legacy decimal index string accepted exactly");
	torrent["fileIdx"] = "2garbage";
	expect(parse_streams(json::array({torrent}), "P2P").empty(), "invalid number suffix is not parsed partially");
	torrent["fileIdx"] = 0;
	torrent["infoHash"] = "not a hash";
	expect(parse_streams(json::array({torrent}), "P2P").empty(), "invalid torrent hash rejected");

	json magnet{{"url", "magnet:?xt=urn%3Abtih%3AAERUKZ4JVPG66AJDIVTYTK6N54ASGRLH&tr=udp%3A%2F%2Ftracker.example%3A6969%2Fannounce&tr=https%3A%2F%2Ftracker.example%2Fannounce&dn=Episode%20One.mkv"}};
	streams = parse_streams(json::array({magnet}), "P2P");
	expect(streams.size() == 1 && streams[0].info_hash == hash && streams[0].url.empty(), "percent-encoded base32 btih maps to the expected 20-byte hash");
	expect(streams[0].sources.size() == 2 && streams[0].filename == "Episode One.mkv", "magnet tracker query and display name survive conversion");
	magnet["url"] = "magnet:?xt=urn:btih:" + hash + "&tr=udp%3A%2F%2Ftracker.example%3A80";
	expect(parse_streams(json::array({magnet}), "P2P")[0].info_hash == hash, "hex magnet accepted");
	magnet["url"] = "magnet:?xt=urn:btih:invalid";
	expect(parse_streams(json::array({magnet}), "P2P").empty(), "malformed magnet is not a playable empty URL");

	json invalid_headers = direct;
	invalid_headers["behaviorHints"]["proxyHeaders"]["request"]["Referer"] = "https://example.test/\r\nInjected: true";
	streams = parse_streams(json::array({invalid_headers}), "Header fixture", true);
	expect(streams.size() == 1 && !streams[0].playable() && !streams[0].unsupported_reason.empty(), "CRLF header injection is rejected with a source-local reason");
	expect(parse_streams(json::array({invalid_headers, direct}), "Mixed").size() == 1, "one invalid provider result does not discard another valid stream");

	json extended = json::array({json{{"ytId", "dQw4w9WgXcQ"}}, json{{"externalUrl", "https://example.test/watch"}},
	    json{{"nzbUrl", "https://example.test/source.nzb"}, {"servers", {"nntps://example.test:563"}}},
	    json{{"rarUrls", json::array({json{{"url", "https://example.test/video.rar"}, {"bytes", 1000}}})}},
	    json{{"playerFrameUrl", "https://example.test/player"}}, json{{"url", "file:///unused"}}});
	expect(parse_streams(extended, "Extended").empty(), "old playable-only API does not feed external/archive sources to native player");
	streams = parse_streams(extended, "Extended", true);
	expect(streams.size() == 6 && streams[0].kind == StreamKind::YouTube && streams[1].kind == StreamKind::External &&
	       streams[2].kind == StreamKind::Nzb && streams[3].kind == StreamKind::Rar && streams[4].kind == StreamKind::PlayerFrame,
	       "extended sources stay visible as separate capabilities");
	expect(!streams[5].playable(), "local file URL is not a supported stream transport");

	json subtitles = json::array({json{{"id", "a"}, {"url", "https://sub.example/a.srt"}, {"lang", "ita"}},
	    json{{"id", "b"}, {"url", "https://sub.example/b.srt"}, {"lang", "it"}, {"label", "Italiano alternativo"}},
	    json{{"url", "https://sub.example/c.vtt"}, {"lang", "custom-language"}}, json{{"url", "javascript:alert(1)"}, {"lang", "it"}}});
	auto tracks = parse_subtitles(subtitles, "Community");
	expect(tracks.size() == 3 && tracks[0].label == "Italian" && tracks[1].label == "Italiano alternativo", "same-language alternatives and labels are preserved");
	expect(!tracks[2].id.empty() && tracks[2].label == "custom-language", "unknown language and missing subtitle ID have useful fallbacks");
	expect(language_to_iso639_2("it-IT") == "ita" && language_to_iso639_2("PT_BR") == "pob" &&
	       language_to_iso639_2("fra") == "fre" && language_to_iso639_2("zh-Hans") == "chi", "ISO aliases and regional subtitle languages normalized");
	std::string subtitle_hash;
	int subtitle_index;
	expect(parse_local_torrent_subtitle_url("http://localhost:11470/" + hash + "/0/italiano.srt", subtitle_hash, subtitle_index) &&
	       subtitle_hash == hash && subtitle_index == 0, "Torrentio local subtitle route parsed including index zero");
	expect(!parse_local_torrent_subtitle_url("http://localhost.evil.example:11470/" + hash + "/0", subtitle_hash, subtitle_index), "look-alike localhost is not rewritten");
	expect(!parse_local_torrent_subtitle_url("http://localhost:11470/subtitles.vtt?from=https://example.test/file", subtitle_hash, subtitle_index), "non-torrent localhost route is not treated as a torrent file");
}

void metadata_identity_and_errors() {
	json meta{{"id", "tt123"}, {"type", "series"}, {"name", "Anime"}, {"posterShape", "landscape"},
	    {"behaviorHints", {{"defaultVideoId", "kitsu:9:1"}}}, {"ids", {{"mal", "7"}}},
	    {"links", json::array({json{{"category", "actor"}, {"name", "Actor"}, {"url", "https://example.test/actor"}},
	                           json{{"category", "director"}, {"name", "Director"}}, json{{"category", "genre"}, {"name", "Anime"}}})},
	    {"videos", json::array({json{{"id", "kitsu:9:2"}, {"season", 1}, {"episode", 2}},
	                            json{{"id", "kitsu:9:1"}, {"season", 1}, {"episode", 1}, {"streams", json::array({json{{"url", "https://media.example/video"}}})}},
	                            json{{"id", "special:0"}, {"season", 0}, {"episode", 1}, {"streams", json::array()}}})},
	    {"trailers", json::array({json{{"source", "dQw4w9WgXcQ"}, {"type", "Trailer"}}})}};
	const auto parsed = parse_meta(meta);
	expect(parsed.videos.size() == 3 && parsed.videos[0].id == "special:0" && parsed.videos[1].id == "kitsu:9:1", "metadata preserves custom episode IDs and season zero");
	expect(parsed.videos[0].has_inline_streams && parsed.videos[0].streams.empty(), "explicitly empty inline stream list is distinguished from absence");
	expect(parsed.videos[1].has_inline_streams && parsed.videos[1].streams.size() == 1 && !parsed.videos[2].has_inline_streams,
	       "consumer can respect inline-stream exclusivity");
	expect(parsed.default_video_id == "kitsu:9:1" && parsed.poster_shape == "landscape" && parsed.ids["mal"] == "7", "metadata behavior and mapping/artwork fields retained");
	expect(parsed.cast == std::vector<std::string>{"Actor"} && parsed.directors == std::vector<std::string>{"Director"} && parsed.genres == std::vector<std::string>{"Anime"},
	       "modern lowercase link categories populate metadata");
	expect(parsed.trailers.size() == 1 && parsed.trailers[0].kind == StreamKind::YouTube, "legacy trailer source converted without claiming native support");
	meta["type"] = "tv";
	meta["behaviorHints"]["hasScheduledVideos"] = true;
	const auto live = parse_meta(meta);
	expect(live.is_live && live.has_scheduled_videos && live.videos[0].id == "kitsu:9:2", "live schedule order is not sorted as episodes");

	json response;
	std::string error;
	expect(parse_addon_response("{\"streams\":[]}", response, error) && error.empty(), "empty resource is a successful answer");
	expect(!parse_addon_response("{\"error\":{\"code\":42,\"message\":\"secret-token\"}}", response, error) &&
	       error == "addon reported an error (code 42)", "provider error is explicit and does not echo sensitive source text");
	expect(parse_addon_response("{\"streams\":[{\"url\":\"https://media.example/play\"}],\"error\":\"one upstream failed\"}", response, error),
	       "aggregator partial result survives one provider error");
	expect(!parse_addon_response("<html>not JSON</html>", response, error), "HTML response is not silently treated as an empty catalog");
	expect(!parse_addon_response("[]", response, error), "resource response envelope must be an object");
	expect(!parse_addon_response(std::string(16u * 1024 * 1024 + 1, ' '), response, error), "JSON memory budget enforced before parsing");
	std::string deep = "{\"value\":";
	deep += std::string(70, '[') + "0" + std::string(70, ']') + "}";
	expect(!parse_addon_response(deep, response, error) && error.find("nesting") != std::string::npos, "deep JSON cannot be accepted as a truncated valid response");
}

void local_transport(const std::string& base) {
	http_init("");
	auto response = http_get(base + "/redirect", 5);
	expect(response.ok() && response.body == "{\"metas\":[]}", "redirect body is replaced by final JSON response");
	expect(response.cache_control == "max-age=60" && response.etag == "\"fixture\"", "final cache headers captured");
	response = http_get(base + "/gzip", 5);
	expect(response.ok() && response.body == "{\"metas\":[]}", "compressed addon JSON decoded");
	response = http_get(base + "/headers", 5, nullptr, {"User-Agent: Fixture Player", "Referer: https://media.example/"});
	expect(response.ok() && response.body == "{\"ok\":true}", "custom media/provider headers sent intact");
	response = http_get(base + "/oversize", 5, nullptr, {}, 1024);
	expect(!response.ok() && response.body.size() <= 1024 && response.error.find("size limit") != std::string::npos,
	       "advertised oversized response refused");
	response = http_get(base + "/chunked-large", 5, nullptr, {}, 1024);
	expect(!response.ok() && response.body.size() <= 1024, "unknown-length body cannot allocate beyond response budget");
	response = http_get(base + "/gzip-large", 5, nullptr, {}, 1024);
	expect(!response.ok() && response.body.size() <= 1024, "decompressed response is bounded even when transfer is small");
	response = http_get(base + "/unavailable", 5);
	expect(!response.ok() && response.status == 503 && response.retry_after == "30", "provider HTTP status and retry hint preserved");
	response = http_post_json(base + "/post", "{\"request\":7}", 5);
	expect(response.ok() && response.body == "{\"received\":7}", "JSON POST behavior remains compatible");
	response = http_get(base + "/non-http-redirect", 5);
	expect(!response.ok(), "redirect cannot switch to a local-file transport");
	std::atomic<bool> cancel{true};
	response = http_get(base + "/ok", 5, &cancel);
	expect(!response.ok() && response.error == "cancelled", "pre-cancelled request does not start network work");
	response = http_get(base + "/ok", 5, nullptr, {"X-Test: value\r\nInjected: header"});
	expect(!response.ok() && response.error == "invalid HTTP request header", "invalid headers rejected before network request");
	json data;
	std::string error;
	expect(!fetch_json(base + "/provider-error", data, error, 5) && error == "addon reported an error (code 42)",
	       "fetch_json reports provider envelope errors to that request only");
	expect(fetch_json(base + "/ok", data, error, 5) && error.empty(), "later provider success clears previous failure");

	std::string file_hash;
	uint64_t file_size = 0;
	const std::vector<std::string> media_headers = {"Authorization: Bearer fixture", "Range: bytes=9-19", "Accept-Encoding: gzip"};
	expect(http_opensubtitles_hash(base + "/range-file", media_headers, file_hash, file_size, error, nullptr, 5) &&
	       file_hash == "5f20e0a06022c000" && file_size == 196608,
	       "two exact ranges produce independently calculated OpenSubtitles hash with media auth intact");
	expect(!http_opensubtitles_hash(base + "/range-file", {}, file_hash, file_size, error, nullptr, 5) && error == "HTTP 401",
	       "range hashing never invents missing media credentials");
	expect(!http_opensubtitles_hash(base + "/range-ignored", media_headers, file_hash, file_size, error, nullptr, 5) &&
	       file_hash.empty() && error.find("size limit") != std::string::npos,
	       "server ignoring Range is bounded before a whole video can be downloaded");
	expect(!http_opensubtitles_hash(base + "/range-changed", media_headers, file_hash, file_size, error, nullptr, 5) &&
	       file_hash.empty() && file_size == 196608,
	       "head and tail with different ETags never produce a plausible but incorrect subtitle hash");
	expect(!http_opensubtitles_hash(base + "/range-wrong", media_headers, file_hash, file_size, error, nullptr, 5) && file_hash.empty(),
	       "response with incorrect Content-Range cannot enter subtitle matching");
	expect(!http_opensubtitles_hash(base + "/range-gzip", media_headers, file_hash, file_size, error, nullptr, 5) && file_hash.empty(),
	       "compressed range representation is rejected even when the decompressed length matches");
	expect(!http_opensubtitles_hash(base + "/range-tail-fails", media_headers, file_hash, file_size, error, nullptr, 5) &&
	       file_hash.empty() && file_size == 196608 && error == "HTTP 503",
	       "size learned from the first range survives unavailable tail without inventing a hash");
	expect(!http_opensubtitles_hash(base + "/range-file", media_headers, file_hash, file_size, error, &cancel, 5) &&
	       file_hash.empty() && error == "cancelled", "file hashing obeys playback cancellation");
}

void subtitle_hash_vectors() {
	std::string first(65536, '\0'), last(65536, '\0');
	expect(opensubtitles_hash(first, last, 131072) == "0000000000020000", "zero blocks retain file length in subtitle hash");
	for (size_t i = 0; i < 8; i++) first[i] = char(8 - i);
	expect(opensubtitles_hash(first, last, 131072) == "0102030405080708", "subtitle hash reads unsigned little-endian words");
	expect(opensubtitles_hash(std::string(65536, char(255)), std::string(65536, char(255)), 131072) == "000000000001c000",
	       "subtitle hash uses defined modulo-64-bit overflow");
	expect(opensubtitles_hash(first.substr(1), last, 131072).empty() && opensubtitles_hash(first, last, 100000).empty(),
	       "short blocks/files cannot be hashed as though missing bytes were zero");
}
}

int main(int argc, char** argv) {
	try {
		manifest_routing();
		catalog_extras_and_urls();
		streams_and_subtitles();
		metadata_identity_and_errors();
		subtitle_hash_vectors();
		if (argc > 1) local_transport(argv[1]);
		std::cout << "PASS: " << checks << " addon protocol and transport assertions\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "FAIL after " << checks << " assertions: " << error.what() << '\n';
		return 1;
	}
}
