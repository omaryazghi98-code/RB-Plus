#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "util.h"

// ---------------------------------------------------------------------------
// Addons (https://github.com/Stremio/stremio-addon-sdk/blob/master/docs/protocol.md)

using AddonExtras = std::vector<std::pair<std::string, std::string>>;

struct CatalogExtra {
	std::string name;
	bool required = false;
	std::vector<std::string> options;
	size_t options_limit = 1;
};

struct Catalog {
	std::string type, id, name;
	std::vector<CatalogExtra> extras;
	bool has_extra(const std::string& n) const;
	bool requires_other_than(const std::string& allowed) const;  // a required extra other than `allowed`
	const CatalogExtra* extra(const std::string& n) const;
	bool supports_extras(const AddonExtras& values) const;
	// Supplies explicit choices only where the manifest offers a default option.
	// Required search/free-text extras still need a value from the caller.
	AddonExtras default_extras() const;
};

struct AddonResource {
	std::string name;
	std::vector<std::string> types;
	std::vector<std::string> id_prefixes;
	// A string resource inherits manifest filters. A full object has independent
	// filters: omitted/empty prefixes mean all IDs, never manifest.idPrefixes.
	bool full = false;
};

struct Addon {
	std::string transport_url;  // .../manifest.json
	std::string base;           // transport_url without /manifest.json
	std::string transport_query;  // kept verbatim, outside the resource path
	std::string id, name, version, description;
	std::vector<std::string> types;
	std::vector<std::string> id_prefixes;
	std::vector<AddonResource> resources;
	std::vector<Catalog> catalogs;
	std::vector<Catalog> addon_catalogs;
	bool from_account = false;
	bool configurable = false, configuration_required = false, epg_provider = false;
	json manifest;

	bool supports(const std::string& resource, const std::string& type, const std::string& id) const;
	std::string resource_url(const std::string& resource, const std::string& type, const std::string& id,
	                         const std::string& extra = "") const;
	std::string resource_url_with_extras(const std::string& resource, const std::string& type,
	                                    const std::string& id, const AddonExtras& extra) const;
	std::string configure_url() const;
};

bool parse_addon(const json& manifest, const std::string& transport_url, Addon& out);
// Empty result means invalid/unsupported transport. Does not modify opaque
// configuration path segments or query strings.
std::string normalize_addon_url(std::string url);
std::string encode_addon_extras(const AddonExtras& extra);

// ---------------------------------------------------------------------------
// Streams and subtitles. Unknown fields are retained in raw/behavior_hints so
// the UI can report unsupported sources instead of claiming they do not exist.

struct SubtitleTrack {
	std::string id, url, lang, label, addon;
	json raw;
};

enum class StreamKind {
	Direct, Torrent, YouTube, External, PlayerFrame, Nzb, Rar, Zip, SevenZip, Tar, Tgz, Unsupported
};

struct Stream {
	std::string name, description;  // display
	std::string addon;              // addon name
	std::string addon_url;          // installation identity; never shown in the interface
	std::string url;                // direct HTTP(S) URL
	std::string info_hash;          // torrent
	int file_idx = -1;
	std::vector<std::string> sources;
	std::string filename;
	std::string binge_group;
	bool not_web_ready = false;
	std::vector<std::string> request_headers;  // behaviorHints.proxyHeaders.request
	std::vector<std::string> response_headers; // proxy response overrides, NOT request headers
	std::vector<SubtitleTrack> subtitles;
	std::string video_hash;
	uint64_t video_size = 0;
	bool has_video_size = false;
	std::vector<std::string> country_whitelist;
	StreamKind kind = StreamKind::Unsupported;
	std::string yt_id, external_url, player_frame_url, unsupported_reason;
	json raw, behavior_hints;
	bool playable() const { return unsupported_reason.empty() && (!url.empty() || !info_hash.empty()); }
};

// The optional flag lets the UI display recognized non-native sources with a
// clear explanation. The two-argument form retains the previous playable-only API.
std::vector<Stream> parse_streams(const json& j, const std::string& addon_name, bool include_unsupported = false);
std::vector<SubtitleTrack> parse_subtitles(const json& j, const std::string& addon_name);
AddonExtras stream_subtitle_extras(const Stream& stream);
// Stremio service proxy: request headers are h= and response overrides r=.
// Keeps the media path/query verbatim; returns empty for an invalid base/source.
std::string stream_proxy_url(const Stream& stream, const std::string& server_url);

// Recognizes only the Stremio local torrent-subtitle route; not arbitrary
// localhost URLs. Native callers can read this file through their torrent engine.
bool parse_local_torrent_subtitle_url(const std::string& url, std::string& info_hash, int& file_idx);

// ---------------------------------------------------------------------------
// Metadata

struct Video {
	std::string id, title, thumbnail, overview, released;
	int season = 0, episode = 0;
	bool has_inline_streams = false;
	std::vector<Stream> streams;
	bool available = false;
	std::string start_time, end_time;
	json raw;
};

struct MetaLink {
	std::string name, category, url;
};

struct Meta {
	std::string id, type, name;
	std::string poster, background, logo;
	std::string description, release_info, runtime, imdb_rating, year;
	std::vector<std::string> genres, cast, directors;
	std::vector<Video> videos;
	std::string poster_shape = "poster";
	std::string released, language, country, awards, website, default_video_id;
	bool is_live = false, has_scheduled_videos = false;
	std::vector<MetaLink> links;
	std::vector<Stream> trailers;
	json raw, behavior_hints, ids;
};

Meta parse_meta(const json& j);

// ---------------------------------------------------------------------------
// Account API (api.strem.io). All blocking; call from worker threads.

struct ApiResult {
	bool ok = false;
	json result;
	std::string error;
};

ApiResult api_call(const std::string& method, json body);
ApiResult api_login(const std::string& email, const std::string& password);
ApiResult api_get_user(const std::string& auth_key);
ApiResult api_addon_collection(const std::string& auth_key);
ApiResult api_library_get(const std::string& auth_key);
ApiResult api_library_put(const std::string& auth_key, const json& items);
ApiResult api_logout(const std::string& auth_key);

// Sign-in with a code approved on another device (link.stremio.com).
struct LinkCode {
	bool ok = false;
	std::string code, link, error;
};
LinkCode link_create();
// Returns ok with result.authKey once approved; ok=false, error="" while pending.
ApiResult link_read(const std::string& code);

// Fetches a manifest and parses it.
bool fetch_addon(const std::string& url, Addon& out, std::string& err, const std::atomic<bool>* cancel = nullptr);

// GET an addon resource as JSON.
bool fetch_json(const std::string& url, json& out, std::string& err, long timeout_s = 20,
                const std::atomic<bool>* cancel = nullptr);
// Bounded, provider-error-aware JSON decoding, also used by offline fixtures.
bool parse_addon_response(const std::string& body, json& out, std::string& err);

extern const char* kCinemetaUrl;
extern const char* kOpenSubtitlesUrl;

// Language codes: "eng" -> "English", "en" -> "eng".
std::string language_name(const std::string& code);
std::string language_to_iso639_2(const std::string& code);
