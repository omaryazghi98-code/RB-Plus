#include "stremio.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "http.h"

const char* kCinemetaUrl = "https://v3-cinemeta.strem.io/manifest.json";
const char* kOpenSubtitlesUrl = "https://opensubtitles-v3.strem.io/manifest.json";

static const char* kApiBase = "https://api.strem.io/api/";
static const char* kLinkBase = "https://link.stremio.com/api/v2/";

static std::vector<std::string> str_array(const json& j) {
	std::vector<std::string> out;
	if (j.is_array()) {
		for (auto& v : j)
			if (v.is_string()) out.push_back(v.get<std::string>());
	} else if (j.is_string()) {
		out.push_back(j.get<std::string>());
	}
	return out;
}

static bool uint_value(const json& value, uint64_t& result) {
	if (value.is_number_unsigned()) {
		result = value.get<uint64_t>();
		return true;
	}
	if (value.is_number_integer()) {
		auto n = value.get<int64_t>();
		if (n < 0) return false;
		result = static_cast<uint64_t>(n);
		return true;
	}
	if (value.is_string()) {
		const auto& s = value.get_ref<const std::string&>();
		if (s.empty()) return false;
		uint64_t n = 0;
		for (char c : s) {
			if (c < '0' || c > '9' || n > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10)
				return false;
			n = n * 10 + (c - '0');
		}
		result = n;
		return true;
	}
	return false;
}

static int bounded_int(const json& object, const char* key, int fallback = 0) {
	const auto& value = jobj(object, key);
	uint64_t n;
	return uint_value(value, n) && n <= static_cast<uint64_t>(std::numeric_limits<int>::max())
	           ? static_cast<int>(n) : fallback;
}

static std::string string_value(const json& object, const char* key, const std::string& fallback = "") {
	const auto& value = jobj(object, key);
	return value.is_string() ? value.get<std::string>() : fallback;
}

static int hex_digit(unsigned char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static std::string encode_component(const std::string& value) {
	static const char* hex = "0123456789ABCDEF";
	std::string result;
	for (unsigned char c : value) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		    c == '-' || c == '_' || c == '.' || c == '~') result += char(c);
		else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
	}
	return result;
}

static bool url_decode(const std::string& value, std::string& out) {
	out.clear();
	for (size_t i = 0; i < value.size(); ++i) {
		unsigned char c = value[i];
		if (c == '%') {
			if (i + 2 >= value.size()) return false;
			int a = hex_digit(value[i + 1]), b = hex_digit(value[i + 2]);
			if (a < 0 || b < 0) return false;
			c = static_cast<unsigned char>((a << 4) | b);
			i += 2;
		} else if (c == '+') c = ' ';
		if (c < 32 || c == 127) return false;
		out += char(c);
	}
	return true;
}

static bool encoded_extras_valid(const std::string& extra) {
	if (extra.empty()) return true;
	for (unsigned char c : extra)
		if (c <= 32 || c == 127 || c == '/' || c == '?' || c == '#' || c == '\\') return false;
	for (const auto& pair : split(extra, '&')) {
		auto eq = pair.find('=');
		if (eq == std::string::npos || eq == 0) return false;
		std::string decoded;
		if (!url_decode(pair.substr(0, eq), decoded) || !url_decode(pair.substr(eq + 1), decoded)) return false;
	}
	return true;
}

static void unique_append(std::vector<std::string>& values, const std::string& value) {
	if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
}

// ---------------------------------------------------------------------------
// Addons

bool Catalog::has_extra(const std::string& n) const { return extra(n) != nullptr; }

const CatalogExtra* Catalog::extra(const std::string& n) const {
	for (auto& e : extras)
		if (e.name == n) return &e;
	return nullptr;
}

bool Catalog::requires_other_than(const std::string& allowed) const {
	for (auto& e : extras)
		if (e.required && e.name != allowed) return true;
	return false;
}

bool Catalog::supports_extras(const AddonExtras& values) const {
	std::map<std::string, size_t> counts;
	for (const auto& value : values) {
		const auto* declaration = extra(value.first);
		if (!declaration) return false;
		if (++counts[value.first] > declaration->options_limit) return false;
		if (!declaration->options.empty() &&
		    std::find(declaration->options.begin(), declaration->options.end(), value.second) == declaration->options.end())
			return false;
	}
	for (const auto& declaration : extras)
		if (declaration.required && counts.find(declaration.name) == counts.end()) return false;
	return true;
}

AddonExtras Catalog::default_extras() const {
	AddonExtras values;
	for (const auto& declaration : extras)
		if (declaration.required && declaration.options_limit > 0) {
			if (!declaration.options.empty()) values.emplace_back(declaration.name, declaration.options.front());
			else if (declaration.name == "skip") values.emplace_back("skip", "0");
			else if (declaration.name == "date") values.emplace_back("date", iso8601_now().substr(0, 10));
		}
	return values;
}

std::string encode_addon_extras(const AddonExtras& extra) {
	std::string result;
	for (const auto& value : extra) {
		if (value.first.empty()) continue;
		if (!result.empty()) result += '&';
		result += encode_component(value.first) + "=" + encode_component(value.second);
	}
	return result;
}

std::string normalize_addon_url(std::string url) {
	url = trim(url);
	if (url.empty() || url.size() > 64u * 1024) return "";
	auto scheme = lower(url.substr(0, url.find(':')));
	if (starts_with(lower(url), "stremio://")) url = "https://" + url.substr(10);
	else if (scheme == "http" || scheme == "https") {
		url.replace(0, scheme.size(), scheme);
	} else {
		// Bare host[:port] is convenient input, but unsupported schemes must not
		// turn into https://ipfs://... or https://file:... .
		if (url.find("://") != std::string::npos || starts_with(lower(url), "file:") ||
		    starts_with(lower(url), "magnet:") || starts_with(lower(url), "javascript:") ||
		    starts_with(lower(url), "data:")) return "";
		url = "https://" + url;
	}
	if (!http_valid_url(url)) return "";
	const auto fragment = url.find('#');
	if (fragment != std::string::npos) url.resize(fragment);
	std::string query;
	const auto query_pos = url.find('?');
	if (query_pos != std::string::npos) {
		query = url.substr(query_pos);
		url.resize(query_pos);
	}
	while (!url.empty() && url.back() == '/') url.pop_back();
	if (ends_with(url, "/configure")) url.resize(url.size() - 10);
	if (!ends_with(url, "/manifest.json")) url += "/manifest.json";
	return url + query;
}

static std::vector<Catalog> parse_catalog_declarations(const json& cats) {
	std::vector<Catalog> result;
	std::set<std::pair<std::string, std::string>> seen;
	if (cats.is_array()) {
		for (const auto& c : cats) {
			Catalog cat;
			cat.type = string_value(c, "type");
			cat.id = string_value(c, "id");
			cat.name = jstr(c, "name");
			if (cat.type.empty() || cat.id.empty() || !seen.emplace(cat.type, cat.id).second) continue;
			const json& extra = jobj(c, "extra");
			if (extra.is_array()) {
				for (const auto& e : extra) {
					CatalogExtra ce;
					ce.name = string_value(e, "name");
					ce.required = jbool(e, "isRequired");
					ce.options = str_array(jobj(e, "options"));
					uint64_t limit;
					if (uint_value(jobj(e, "optionsLimit"), limit) && limit <= std::numeric_limits<size_t>::max())
						ce.options_limit = static_cast<size_t>(limit);
					if (!ce.name.empty() && !cat.has_extra(ce.name)) cat.extras.push_back(std::move(ce));
				}
			} else {
				// Modern extras take precedence, including an explicitly empty
				// array. Keep support for older configurable add-on manifests.
				for (const auto& n : str_array(jobj(c, "extraSupported")))
					if (!cat.has_extra(n)) cat.extras.push_back(CatalogExtra{n, false, {}});
				for (const auto& n : str_array(jobj(c, "extraRequired"))) {
					bool found = false;
					for (auto& e : cat.extras)
						if (e.name == n) e.required = found = true;
					if (!found) cat.extras.push_back(CatalogExtra{n, true, {}});
				}
			}
			if (const CatalogExtra* g = cat.extra("genre")) {
				if (g->options.empty()) {
					// Some addons list genres at the top level.
					for (auto& e : cat.extras)
						if (e.name == "genre") e.options = str_array(jobj(c, "genres"));
				}
			}
			result.push_back(std::move(cat));
		}
	}
	return result;
}

bool parse_addon(const json& m, const std::string& transport_url, Addon& out) {
	const std::string normalized = normalize_addon_url(transport_url);
	if (!m.is_object() || string_value(m, "id").empty() || normalized.empty()) return false;
	Addon a;
	a.from_account = out.from_account;
	a.manifest = m;
	a.transport_url = normalized;
	a.base = normalized;
	auto query = a.base.find('?');
	if (query != std::string::npos) {
		a.transport_query = a.base.substr(query);
		a.base.resize(query);
	}
	a.base.resize(a.base.size() - 14);  // normalized path ends in /manifest.json
	a.id = string_value(m, "id");
	a.name = jstr(m, "name", a.id);
	if (a.name.empty()) a.name = a.id;
	a.version = jstr(m, "version");
	a.description = jstr(m, "description");
	a.types = str_array(jobj(m, "types"));
	a.id_prefixes = str_array(jobj(m, "idPrefixes"));
	const auto& hints = jobj(m, "behaviorHints");
	a.configurable = jbool(hints, "configurable");
	a.configuration_required = jbool(hints, "configurationRequired");
	a.epg_provider = jbool(hints, "epgProvider");
	const json& resources = jobj(m, "resources");
	if (resources.is_array()) {
		for (const auto& resource : resources) {
			AddonResource ar;
			if (resource.is_string()) ar.name = resource.get<std::string>();
			else if (resource.is_object()) {
				ar.full = true;
				ar.name = string_value(resource, "name");
				ar.types = str_array(jobj(resource, "types"));
				ar.id_prefixes = str_array(jobj(resource, "idPrefixes"));
			}
			if (!ar.name.empty()) a.resources.push_back(std::move(ar));
		}
	}
	a.catalogs = parse_catalog_declarations(jobj(m, "catalogs"));
	a.addon_catalogs = parse_catalog_declarations(jobj(m, "addonCatalogs"));
	if (a.resources.empty() && a.catalogs.empty() && a.addon_catalogs.empty()) return false;
	out = std::move(a);
	return true;
}

bool Addon::supports(const std::string& resource, const std::string& type, const std::string& id) const {
	if (resource == "catalog" || resource == "addon_catalog") {
		const auto& list = resource == "catalog" ? catalogs : addon_catalogs;
		for (const auto& catalog : list)
			if (catalog.type == type && catalog.id == id) return true;
		return false;
	}
	for (const auto& r : resources) {
		if (r.name != resource) continue;
		const auto& accepted_types = r.full ? r.types : types;
		if (std::find(accepted_types.begin(), accepted_types.end(), type) == accepted_types.end()) continue;
		const auto& prefixes = r.full ? r.id_prefixes : id_prefixes;
		if (prefixes.empty()) return true;
		for (const auto& p : prefixes)
			if (starts_with(id, p)) return true;
	}
	return false;
}

std::string Addon::resource_url(const std::string& resource, const std::string& type, const std::string& id,
                                const std::string& extra) const {
	if (base.empty() || resource.empty() || type.empty() || id.empty() || !encoded_extras_valid(extra)) return "";
	std::string u = base + "/" + encode_component(resource) + "/" + encode_component(type) + "/" + encode_component(id);
	if (!extra.empty()) u += "/" + extra;
	return u + ".json" + transport_query;
}

std::string Addon::resource_url_with_extras(const std::string& resource, const std::string& type,
                                           const std::string& id, const AddonExtras& extra) const {
	return resource_url(resource, type, id, encode_addon_extras(extra));
}

std::string Addon::configure_url() const {
	return base.empty() ? "" : base + "/configure" + transport_query;
}

bool parse_addon_response(const std::string& body, json& out, std::string& err) {
	out = json();
	err.clear();
	constexpr size_t max_json_bytes = 16u * 1024 * 1024;
	if (body.size() > max_json_bytes) {
		err = "addon response exceeds 16 MiB";
		return false;
	}
	bool too_deep = false;
	auto depth_check = [&too_deep](int depth, json::parse_event_t, json&) {
		if (depth > 64) too_deep = true;
		return depth <= 64;
	};
	json parsed = json::parse(body, depth_check, false);
	if (too_deep || parsed.is_discarded() || !parsed.is_object()) {
		err = too_deep ? "addon JSON exceeds nesting limit" : "invalid addon JSON object";
		return false;
	}
	const auto& error = jobj(parsed, "error");
	// Some aggregators return partial content plus an error for one upstream.
	// Keep the usable resource; do not turn a provider-local problem into a
	// failure of every stream in that response.
	bool has_resource = jobj(parsed, "streams").is_array() || jobj(parsed, "metas").is_array() ||
	                    jobj(parsed, "metasDetailed").is_array() || jobj(parsed, "meta").is_object() ||
	                    jobj(parsed, "subtitles").is_array() || jobj(parsed, "addons").is_array();
	if (!error.is_null() && error != false && !has_resource) {
		// Provider text can contain a configured URL or token; don't put it in
		// generic logs. The response itself is never echoed in the error.
		err = "addon reported an error";
		uint64_t code;
		if (uint_value(jobj(error, "code"), code)) err += " (code " + std::to_string(code) + ")";
		return false;
	}
	out = std::move(parsed);
	return true;
}

bool fetch_json(const std::string& url, json& out, std::string& err, long timeout_s,
                const std::atomic<bool>* cancel) {
	err.clear();
	out = json();
	HttpResponse r = http_get(url, timeout_s, cancel, {}, 16u * 1024 * 1024);
	if (!r.ok()) {
		err = r.describe();
		return false;
	}
	return parse_addon_response(r.body, out, err);
}

bool fetch_addon(const std::string& url, Addon& out, std::string& err, const std::atomic<bool>* cancel) {
	json m;
	const std::string normalized = normalize_addon_url(url);
	if (normalized.empty()) {
		err = "invalid or unsupported addon URL; use HTTP(S) manifest URL";
		return false;
	}
	if (!fetch_json(normalized, m, err, 15, cancel)) return false;
	if (!parse_addon(m, normalized, out)) {
		err = "not an addon manifest";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Metadata

Meta parse_meta(const json& j) {
	Meta m;
	if (!j.is_object()) return m;
	m.raw = j;
	m.id = jstr(j, "id", jstr(j, "imdb_id"));
	m.type = jstr(j, "type");
	m.name = jstr(j, "name");
	m.poster = jstr(j, "poster");
	m.background = jstr(j, "background");
	m.logo = jstr(j, "logo");
	m.poster_shape = jstr(j, "posterShape", "poster");
	if (m.poster_shape != "poster" && m.poster_shape != "square" && m.poster_shape != "landscape")
		m.poster_shape = "poster";
	m.description = jstr(j, "description");
	m.release_info = jstr(j, "releaseInfo", jstr(j, "year"));
	m.year = m.release_info;
	m.runtime = jstr(j, "runtime");
	m.imdb_rating = jstr(j, "imdbRating");
	m.released = jstr(j, "released");
	m.language = jstr(j, "language");
	m.country = jstr(j, "country");
	m.awards = jstr(j, "awards");
	m.website = jstr(j, "website");
	m.ids = jobj(j, "ids");
	m.behavior_hints = jobj(j, "behaviorHints");
	m.default_video_id = jstr(m.behavior_hints, "defaultVideoId");
	m.is_live = m.type == "tv" || jbool(m.behavior_hints, "isLive");
	m.has_scheduled_videos = jbool(m.behavior_hints, "hasScheduledVideos");
	m.genres = str_array(jobj(j, "genres"));
	if (m.genres.empty()) m.genres = str_array(jobj(j, "genre"));
	m.cast = str_array(jobj(j, "cast"));
	m.directors = str_array(jobj(j, "director"));
	// Newer addons use "links" with categories instead.
	const json& links = jobj(j, "links");
	if (links.is_array()) {
		bool want_genres = m.genres.empty(), want_cast = m.cast.empty(), want_dir = m.directors.empty();
		for (const auto& l : links) {
			std::string cat = lower(jstr(l, "category")), name = jstr(l, "name");
			if (name.empty()) continue;
			m.links.push_back({name, jstr(l, "category"), jstr(l, "url")});
			if (want_genres && (cat == "genre" || cat == "genres")) unique_append(m.genres, name);
			else if (want_cast && (cat == "actor" || cat == "actors" || cat == "cast")) unique_append(m.cast, name);
			else if (want_dir && (cat == "directors" || cat == "director")) unique_append(m.directors, name);
		}
	}
	const json& vids = jobj(j, "videos");
	if (vids.is_array()) {
		for (const auto& v : vids) {
			Video vid;
			vid.id = string_value(v, "id");
			if (vid.id.empty()) continue;
			vid.raw = v;
			vid.title = jstr(v, "title", jstr(v, "name"));
			vid.thumbnail = jstr(v, "thumbnail");
			vid.overview = jstr(v, "overview", jstr(v, "description"));
			vid.released = jstr(v, "released", jstr(v, "firstAired"));
			vid.season = bounded_int(v, "season");
			vid.episode = bounded_int(v, "episode", bounded_int(v, "number"));
			vid.available = jbool(v, "available");
			vid.start_time = jstr(v, "startTime");
			vid.end_time = jstr(v, "endTime");
			vid.has_inline_streams = jobj(v, "streams").is_array();
			if (vid.has_inline_streams) vid.streams = parse_streams(v, "", true);
			m.videos.push_back(std::move(vid));
		}
		// Episode order is meaningful for series, but not for channel uploads
		// or an EPG. Keep those providers' chronological order unchanged.
		if (m.type == "series" && !m.has_scheduled_videos && !m.is_live) {
			std::stable_sort(m.videos.begin(), m.videos.end(), [](const Video& a, const Video& b) {
				if (a.season != b.season) return a.season < b.season;
				return a.episode < b.episode;
			});
		}
	}
	const auto& trailers = jobj(j, "trailers");
	if (trailers.is_array()) {
		json streams = json::array();
		for (const auto& trailer : trailers) {
			if (!trailer.is_object()) continue;
			json source = trailer;
			if (!string_value(trailer, "source").empty() && string_value(trailer, "ytId").empty())
				source["ytId"] = string_value(trailer, "source");
			streams.push_back(std::move(source));
		}
		m.trailers = parse_streams(streams, "Trailer", true);
	}
	return m;
}

static std::string normalize_info_hash(const std::string& value) {
	if (value.size() == 40 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return hex_digit(c) >= 0; }))
		return lower(value);
	// Magnet btih permits base32 as well as the conventional forty hex digits.
	if (value.size() != 32) return "";
	uint32_t buffer = 0;
	int bits = 0;
	std::string result;
	static const char* hex = "0123456789abcdef";
	for (unsigned char c : value) {
		if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
		int digit = c >= 'A' && c <= 'Z' ? c - 'A' : c >= '2' && c <= '7' ? c - '2' + 26 : -1;
		if (digit < 0) return "";
		buffer = (buffer << 5) | static_cast<uint32_t>(digit);
		bits += 5;
		if (bits >= 8) {
			bits -= 8;
			unsigned char byte = static_cast<unsigned char>((buffer >> bits) & 0xff);
			result += hex[byte >> 4];
			result += hex[byte & 15];
		}
	}
	return result.size() == 40 ? result : "";
}

static bool parse_magnet(const std::string& magnet, Stream& stream) {
	auto question = magnet.find('?');
	if (question == std::string::npos) return false;
	auto fragment = magnet.find('#', question);
	for (const auto& pair : split(magnet.substr(question + 1, fragment == std::string::npos ? fragment : fragment - question - 1), '&')) {
		auto eq = pair.find('=');
		if (eq == std::string::npos) continue;
		std::string key, value;
		if (!url_decode(pair.substr(0, eq), key) || !url_decode(pair.substr(eq + 1), value)) return false;
		key = lower(key);
		if (key == "xt" && starts_with(lower(value), "urn:btih:") && stream.info_hash.empty())
			stream.info_hash = normalize_info_hash(value.substr(9));
		else if (key == "tr") {
			if (starts_with(lower(value), "udp://") || http_valid_url(value)) unique_append(stream.sources, "tracker:" + value);
		} else if (key == "dn" && stream.filename.empty()) stream.filename = value;
	}
	return !stream.info_hash.empty();
}

static bool parse_headers(const json& object, std::vector<std::string>& output) {
	if (object.is_null()) return true;
	if (!object.is_object()) return false;
	for (auto it = object.begin(); it != object.end(); ++it) {
		if (!it.value().is_string()) return false;
		const auto& name = it.key();
		const auto& value = it.value().get_ref<const std::string&>();
		if (name.empty() || name.size() > 256 || value.size() > 64u * 1024) return false;
		for (unsigned char c : name) {
			const bool token = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			                   std::string("!#$%&'*+-.^_`|~").find(c) != std::string::npos;
			if (!token) return false;
		}
		for (unsigned char c : value)
			if ((c < 32 && c != '\t') || c == 127) return false;
		output.push_back(name + ": " + value);
	}
	return true;
}

std::vector<Stream> parse_streams(const json& j, const std::string& addon_name, bool include_unsupported) {
	std::vector<Stream> out;
	const json& arr = j.is_array() ? j : jobj(j, "streams");
	if (!arr.is_array()) return out;
	for (const auto& s : arr) {
		if (!s.is_object()) continue;
		Stream st;
		st.raw = s;
		st.addon = addon_name;
		st.name = jstr(s, "name", addon_name);
		if (st.name.empty()) st.name = addon_name;
		st.description = jstr(s, "description", jstr(s, "title"));
		if (st.description.empty()) st.description = jstr(s, "title");
		st.url = string_value(s, "url");
		st.info_hash = normalize_info_hash(string_value(s, "infoHash"));
		const auto& idx = jobj(s, "fileIdx");
		uint64_t file_idx;
		bool index_valid = true;
		if (!idx.is_null()) {
			if (uint_value(idx, file_idx) && file_idx <= static_cast<uint64_t>(std::numeric_limits<int>::max()))
				st.file_idx = static_cast<int>(file_idx);
			else if (idx.is_number_integer() && !idx.is_number_unsigned() && idx.get<int64_t>() == -1)
				st.file_idx = -1; // conventional automatic-file sentinel
			else index_valid = false;
		}
		st.sources = str_array(jobj(s, "sources"));
		for (const auto& source : str_array(jobj(s, "announce"))) unique_append(st.sources, source);
		const json& bh = jobj(s, "behaviorHints");
		st.behavior_hints = bh.is_object() ? bh : json::object();
		st.filename = string_value(bh, "filename");
		st.binge_group = string_value(bh, "bingeGroup");
		st.not_web_ready = jbool(bh, "notWebReady");
		st.video_hash = string_value(bh, "videoHash");
		st.has_video_size = uint_value(jobj(bh, "videoSize"), st.video_size);
		st.country_whitelist = str_array(jobj(bh, "countryWhitelist"));
		st.subtitles = parse_subtitles(s, addon_name);
		const json& headers = jobj(bh, "proxyHeaders");
		bool headers_valid = parse_headers(jobj(headers, "request"), st.request_headers);
		headers_valid = parse_headers(jobj(headers, "response"), st.response_headers) && headers_valid;
		st.yt_id = string_value(s, "ytId");
		st.external_url = string_value(s, "externalUrl");
		st.player_frame_url = string_value(s, "playerFrameUrl");
		if (!st.url.empty()) {
			if (starts_with(lower(st.url), "magnet:")) {
				// The magnet itself is the source; don't silently substitute an
				// unrelated descriptive infoHash supplied alongside it.
				st.info_hash.clear();
				if (parse_magnet(st.url, st) && index_valid) st.kind = StreamKind::Torrent;
				else st.unsupported_reason = "Invalid torrent magnet or file index";
				st.url.clear();
			} else if (http_valid_url(st.url)) st.kind = StreamKind::Direct;
			else st.unsupported_reason = "This stream URL uses an unsupported or invalid protocol";
		} else if (!string_value(s, "infoHash").empty()) {
			if (!st.info_hash.empty() && index_valid) st.kind = StreamKind::Torrent;
			else st.unsupported_reason = "Invalid torrent hash or file index";
		} else if (!st.yt_id.empty()) {
			st.kind = StreamKind::YouTube;
			st.unsupported_reason = "YouTube needs a compatible resolver or streaming service";
		} else if (!st.external_url.empty() || s.contains("androidTvUrl") || s.contains("tizenUrl") || s.contains("webosUrl")) {
			st.kind = StreamKind::External;
			st.unsupported_reason = "This source opens an external website or another platform's app";
		} else if (!st.player_frame_url.empty()) {
			st.kind = StreamKind::PlayerFrame;
			st.unsupported_reason = "This source requires an embedded web player";
		} else {
			struct Extended { const char* key; StreamKind kind; };
			static const Extended extended[] = {{"nzbUrl", StreamKind::Nzb}, {"rarUrls", StreamKind::Rar},
			    {"zipUrls", StreamKind::Zip}, {"7zipUrls", StreamKind::SevenZip}, {"tarUrls", StreamKind::Tar},
			    {"tgzUrls", StreamKind::Tgz}};
			for (const auto& source : extended)
				if (!jobj(s, source.key).is_null()) { st.kind = source.kind; break; }
			st.unsupported_reason = st.kind == StreamKind::Unsupported
			    ? "The addon did not provide a supported stream source"
			    : "Archive and Usenet sources require a compatible streaming service";
		}
		if (st.kind == StreamKind::Direct && !headers_valid)
			st.unsupported_reason = "The addon supplied invalid stream headers";
		if (st.playable() || include_unsupported) out.push_back(std::move(st));
	}
	return out;
}

std::vector<SubtitleTrack> parse_subtitles(const json& j, const std::string& addon_name) {
	std::vector<SubtitleTrack> out;
	const json& arr = j.is_array() ? j : jobj(j, "subtitles");
	if (!arr.is_array()) return out;
	for (const auto& s : arr) {
		if (!s.is_object()) continue;
		SubtitleTrack t;
		t.raw = s;
		t.id = jstr(s, "id");
		t.url = jstr(s, "url");
		t.lang = jstr(s, "lang");
		t.addon = addon_name;
		if (!http_valid_url(t.url)) continue;
		if (t.id.empty()) t.id = "subtitle-" + std::to_string(fnv1a(t.url + "\n" + t.lang));
		t.label = jstr(s, "label");
		if (t.label.empty()) t.label = language_name(t.lang);
		out.push_back(std::move(t));
	}
	return out;
}

AddonExtras stream_subtitle_extras(const Stream& stream) {
	AddonExtras extra;
	if (!stream.video_hash.empty()) extra.emplace_back("videoHash", stream.video_hash);
	if (stream.has_video_size) extra.emplace_back("videoSize", std::to_string(stream.video_size));
	if (!stream.filename.empty()) extra.emplace_back("filename", stream.filename);
	return extra;
}

std::string stream_proxy_url(const Stream& stream, const std::string& server_url) {
	if (!http_valid_url(server_url) || !http_valid_url(stream.url)) return "";
	std::string base = server_url;
	// The configured service base may carry a reverse-proxy prefix, but must
	// not carry a query/fragment which would swallow the new resource path.
	if (base.find_first_of("?#", base.find("://") + 3) != std::string::npos) return "";
	while (!base.empty() && base.back() == '/') base.pop_back();
	auto path_start = stream.url.find_first_of("/?#", stream.url.find("://") + 3);
	std::string origin = stream.url.substr(0, path_start);
	std::string path = path_start == std::string::npos ? "/" : stream.url.substr(path_start);
	if (path.front() != '/') path.insert(path.begin(), '/');
	auto fragment = path.find('#');
	if (fragment != std::string::npos) path.erase(fragment);
	AddonExtras options = {{"d", origin}};
	auto add_headers = [&](const std::vector<std::string>& headers, const char* key) {
		for (const auto& header : headers) {
			auto colon = header.find(':');
			if (colon == std::string::npos) continue;
			options.emplace_back(key, header.substr(0, colon) + ":" + trim(header.substr(colon + 1)));
		}
	};
	add_headers(stream.request_headers, "h");
	add_headers(stream.response_headers, "r");
	return base + "/proxy/" + encode_addon_extras(options) + path;
}

bool parse_local_torrent_subtitle_url(const std::string& url, std::string& info_hash, int& file_idx) {
	info_hash.clear();
	file_idx = -1;
	if (!http_valid_url(url)) return false;
	auto authority_start = url.find("://") + 3;
	auto path = url.find('/', authority_start);
	if (path == std::string::npos) return false;
	const auto authority = lower(url.substr(authority_start, path - authority_start));
	if (authority != "localhost:11470" && authority != "127.0.0.1:11470" && authority != "[::1]:11470") return false;
	auto end = url.find_first_of("?#", path);
	const auto parts = split(url.substr(path + 1, end == std::string::npos ? end : end - path - 1), '/');
	if (parts.size() < 2) return false;
	std::string hash_text, index_text;
	if (!url_decode(parts[0], hash_text) || !url_decode(parts[1], index_text)) return false;
	const auto hash = normalize_info_hash(hash_text);
	uint64_t index;
	if (hash.empty() || !uint_value(json(index_text), index) || index > static_cast<uint64_t>(std::numeric_limits<int>::max()))
		return false;
	info_hash = hash;
	file_idx = static_cast<int>(index);
	return true;
}

// ---------------------------------------------------------------------------
// Account API

ApiResult api_call(const std::string& method, json body) {
	ApiResult r;
	HttpResponse h = http_post_json(kApiBase + method, body.dump(), 25);
	if (!h.error.empty()) {
		r.error = h.error;
		return r;
	}
	json j = json::parse(h.body, nullptr, false);
	if (j.is_discarded()) {
		r.error = h.status ? "HTTP " + std::to_string(h.status) : "invalid response";
		return r;
	}
	const json& err = jobj(j, "error");
	if (!err.is_null()) {
		r.error = err.is_object() ? jstr(err, "message", "error") : (err.is_string() ? err.get<std::string>() : "error");
		return r;
	}
	if (!j.contains("result")) {
		r.error = "HTTP " + std::to_string(h.status);
		return r;
	}
	r.ok = true;
	r.result = j["result"];
	return r;
}

ApiResult api_login(const std::string& email, const std::string& password) {
	return api_call("login", json{{"type", "Login"}, {"email", email}, {"password", password}, {"facebook", false}});
}

ApiResult api_get_user(const std::string& auth_key) {
	return api_call("getUser", json{{"type", "GetUser"}, {"authKey", auth_key}});
}

ApiResult api_addon_collection(const std::string& auth_key) {
	return api_call("addonCollectionGet", json{{"type", "AddonCollectionGet"}, {"authKey", auth_key}, {"update", true}});
}

ApiResult api_library_get(const std::string& auth_key) {
	return api_call("datastoreGet",
	                json{{"authKey", auth_key}, {"collection", "libraryItem"}, {"ids", json::array()}, {"all", true}});
}

ApiResult api_library_put(const std::string& auth_key, const json& items) {
	return api_call("datastorePut", json{{"authKey", auth_key}, {"collection", "libraryItem"}, {"changes", items}});
}

ApiResult api_logout(const std::string& auth_key) {
	return api_call("logout", json{{"type", "Logout"}, {"authKey", auth_key}});
}

LinkCode link_create() {
	LinkCode lc;
	HttpResponse h = http_get(std::string(kLinkBase) + "create?type=Create", 20);
	if (!h.ok()) {
		lc.error = h.describe();
		return lc;
	}
	json j = json::parse(h.body, nullptr, false);
	const json& res = jobj(j, "result");
	lc.code = jstr(res, "code");
	lc.link = jstr(res, "link", "https://link.stremio.com");
	if (lc.code.empty()) {
		lc.error = jstr(jobj(j, "error"), "message", "no code in response");
		return lc;
	}
	lc.ok = true;
	return lc;
}

ApiResult link_read(const std::string& code) {
	ApiResult r;
	HttpResponse h = http_get(std::string(kLinkBase) + "read?type=Read&code=" + url_encode(code), 20);
	if (!h.error.empty()) {
		r.error = h.error;
		return r;
	}
	json j = json::parse(h.body, nullptr, false);
	const json& res = jobj(j, "result");
	std::string key = jstr(res, "authKey");
	if (!key.empty()) {
		r.ok = true;
		r.result = res;
		return r;
	}
	// Not approved yet. The server answers "Invalid or expired token" (code
	// 101) until then, so that isn't an expiry: the caller times out itself.
	r.error = jstr(jobj(j, "error"), "message", "pending");
	return r;
}

// ---------------------------------------------------------------------------
// Languages

struct Lang {
	const char* code2;
	const char* code3;
	const char* alt3;
	const char* name;
};

static const Lang kLangs[] = {
	{"en", "eng", "", "English"},     {"el", "gre", "ell", "Greek"},     {"es", "spa", "", "Spanish"},
	{"fr", "fre", "fra", "French"},   {"de", "ger", "deu", "German"},    {"it", "ita", "", "Italian"},
	{"pt", "por", "", "Portuguese"},  {"ro", "rum", "ron", "Romanian"},  {"nl", "dut", "nld", "Dutch"},
	{"pl", "pol", "", "Polish"},      {"ru", "rus", "", "Russian"},      {"tr", "tur", "", "Turkish"},
	{"ar", "ara", "", "Arabic"},      {"he", "heb", "", "Hebrew"},       {"bg", "bul", "", "Bulgarian"},
	{"hr", "hrv", "", "Croatian"},    {"sr", "srp", "scc", "Serbian"},   {"cs", "cze", "ces", "Czech"},
	{"sk", "slo", "slk", "Slovak"},   {"sl", "slv", "", "Slovenian"},    {"hu", "hun", "", "Hungarian"},
	{"sv", "swe", "", "Swedish"},     {"no", "nor", "nob", "Norwegian"}, {"da", "dan", "", "Danish"},
	{"fi", "fin", "", "Finnish"},     {"et", "est", "", "Estonian"},     {"lv", "lav", "", "Latvian"},
	{"lt", "lit", "", "Lithuanian"},  {"uk", "ukr", "", "Ukrainian"},    {"zh", "chi", "zho", "Chinese"},
	{"ja", "jpn", "", "Japanese"},    {"ko", "kor", "", "Korean"},       {"hi", "hin", "", "Hindi"},
	{"th", "tha", "", "Thai"},        {"vi", "vie", "", "Vietnamese"},   {"id", "ind", "", "Indonesian"},
	{"ms", "may", "msa", "Malay"},    {"fa", "per", "fas", "Persian"},   {"sq", "alb", "sqi", "Albanian"},
	{"mk", "mac", "mkd", "Macedonian"}, {"bs", "bos", "", "Bosnian"},    {"ca", "cat", "", "Catalan"},
	{"pb", "pob", "", "Portuguese (BR)"},
};

static const Lang* find_lang(const std::string& code) {
	std::string c = lower(trim(code));
	if (c.empty()) return nullptr;
	c = replace_all(c, "_", "-");
	if (c == "pt-br" || c == "por-br" || c == "ptb" || c == "brazilian portuguese") c = "pob";
	// Addons commonly supply BCP 47 language tags (it-IT, en-US, zh-Hans)
	// alongside ISO 639-1/2. Preserve the regional Brazilian choice above.
	const auto separator = c.find('-');
	if (separator != std::string::npos) c.resize(separator);
	if (c == "nb") c = "no";
	for (auto& l : kLangs) {
		if (c == l.code2 || c == l.code3 || (l.alt3[0] && c == l.alt3) || c == lower(l.name)) return &l;
	}
	return nullptr;
}

std::string language_name(const std::string& code) {
	const Lang* l = find_lang(code);
	if (l) return l->name;
	return code.empty() ? "Unknown" : code;
}

std::string language_to_iso639_2(const std::string& code) {
	const Lang* l = find_lang(code);
	return l ? l->code3 : lower(trim(code));
}
