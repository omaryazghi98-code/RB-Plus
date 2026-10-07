// Exercises production App methods and Tasks callbacks against a loopback addon.
// No App::init(), account, artwork downloads, decoder or torrent network starts.
#include "app.h"
#include "http.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
int assertions = 0;
void expect(bool condition, const std::string& message) {
	++assertions;
	if (!condition) throw std::runtime_error(message);
}
template <typename Predicate>
void until(Predicate predicate, const std::string& message, bool drain = true) {
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
	while (std::chrono::steady_clock::now() < end) {
		if (drain) g_tasks.drain();
		if (predicate()) return;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	throw std::runtime_error("Timed out: " + message);
}
json traffic(const std::string& base) {
	auto response = http_get(base + "/app/requests", 3);
	if (!response.ok()) throw std::runtime_error("Fixture traffic endpoint failed");
	return json::parse(response.body);
}
void reset_traffic(const std::string& base) {
	if (!http_get(base + "/app/reset", 3).ok()) throw std::runtime_error("Fixture reset failed");
}
bool contains_path(const json& paths, const std::string& needle) {
	for (const auto& path : paths)
		if (path.is_string() && path.get<std::string>().find(needle) != std::string::npos) return true;
	return false;
}
size_t count_paths(const json& paths, const std::string& needle) {
	size_t count = 0;
	for (const auto& path : paths)
		if (path.is_string() && path.get<std::string>().find(needle) != std::string::npos) ++count;
	return count;
}
std::unique_ptr<Addon> fixture_addon(const std::string& base, const std::string& route,
                                    const std::string& name, json resources, json catalogs = json::array()) {
	json manifest = {{"id", "app.fixture." + name}, {"name", name}, {"version", "1.0.0"},
	                 {"types", {"movie", "series", "anime", "tv", "other"}},
	                 {"resources", std::move(resources)}, {"catalogs", std::move(catalogs)}};
	auto addon = std::make_unique<Addon>();
	if (!parse_addon(manifest, base + "/app/" + route + "/manifest.json?opaque=A%2BB", *addon))
		throw std::runtime_error("Fixture manifest failed validation");
	addon->from_account = true;
	return addon;
}
Item item(const std::string& id, const std::string& type = "anime") {
	Item value;
	value.id = id;
	value.type = type;
	value.name = "Preview " + id;
	return value;
}
}  // namespace

struct AppProtocolTest {
	static void idle(App& app) { until([&] { return app.busy_count_ == 0; }, "all App worker callbacks"); }
	static void install_stream_fixtures(App& app, const std::string& base, bool metadata = true) {
		idle(app);
		app.addons_.clear();
		if (metadata) app.addons_.push_back(fixture_addon(base, "meta", "Metadata", {"meta"}));
		app.addons_.push_back(fixture_addon(base, "slow", "Slow first", {"stream"}));
		app.addons_.push_back(fixture_addon(base, "fast", "Fast second", {"stream"}));
		app.addons_loading_ = false;
	}

	static void details(App& app, const std::string& base) {
		install_stream_fixtures(app, base);
		reset_traffic(base);
		app.detail_open(item("kitsu:inline"));
		expect(app.d_stream_list_.empty(), "stream addons wait until metadata can declare inline exclusivity");
		idle(app);
		expect(app.d_meta_loaded_ && app.d_series && app.d_zone == "episodes", "series defaultVideoId preselects the direct episode carousel");
		app.detail_button(Btn::Cross);
		expect(app.d_video_id_ == "opaque/video:+%?1" && app.d_stream_list_.size() == 1,
		       "opaque inline video ID survives metadata, selection and stream list");
		expect(app.d_stream_list_[0].addon == "Metadata", "inline streams retain the metadata provider identity");
		expect(count_paths(traffic(base)["requests"], "/stream/") == 0, "inline streams exclude every installed stream addon");

		reset_traffic(base);
		app.detail_open(item("kitsu:empty"));
		idle(app);
		app.detail_button(Btn::Cross);
		expect(app.d_meta_loaded_ && app.d_stream_list_.empty(), "explicit empty inline stream array stays empty");
		expect(count_paths(traffic(base)["requests"], "/stream/") == 0,
		       "explicit empty inline stream array does not trigger provider fallback");
		expect(app.d_streams_status.find("metadata addon") != std::string::npos, "empty inline response has a useful status");

		reset_traffic(base);
		app.detail_open(item("kitsu:absent"));
		idle(app);
		app.detail_button(Btn::Cross);
		until([&] { return app.d_meta_loaded_ && app.d_video_id_ == "opaque/absent:+?" && app.d_answered_ == 1; },
		      "fast provider result before the first provider");
		expect(app.d_stream_list_.size() == 2 && app.d_stream_list_[0].addon == "Fast second",
		       "fast provider is usable while an earlier provider is loading");
		app.d_stream_sel = 1;
		const std::string focused_name = app.d_streams[1].name;
		idle(app);
		expect(app.d_stream_list_.size() == 4 && app.d_stream_list_[0].addon == "Slow first" &&
		       app.d_stream_list_[2].addon == "Fast second", "final results follow installed provider order despite reverse completion");
		expect(!app.d_pick_res && app.d_view_ == std::vector<int>({0, 1, 2, 3}),
		       "initial source view preserves provider ranking rather than regrouping by quality");
		expect(app.d_stream_sel == 3 && app.d_streams[app.d_stream_sel].name == focused_name,
		       "selected stream retains focus when earlier provider inserts results above it");
		const auto requests = traffic(base)["requests"];
		expect(contains_path(requests, "/stream/anime/opaque%2Fabsent%3A%2B%3F.json?opaque=A%2BB"),
		       "actual worker URL preserves custom type, encoded video ID and configured query");

		app.detail_load_streams("provider-failure");
		idle(app);
		expect(app.d_stream_list_.size() == 2 && app.d_stream_list_[0].addon == "Fast second",
		       "one failed stream provider does not discard another provider's results");
		expect(app.d_streams_status.find("HTTP 503") != std::string::npos &&
		       app.d_streams_status.find("opaque=") == std::string::npos, "provider failure status is useful and does not expose configured URL");

		reset_traffic(base);
		app.detail_open(item("channel:one", "tv"));
		idle(app);
		expect(!app.d_series && app.d_video_id_ == "channel:one", "live channel uses channel ID, not programme or default programme ID");
		expect(contains_path(traffic(base)["requests"], "/stream/tv/channel%3Aone.json"), "live playback queries the channel's type and identity");

		install_stream_fixtures(app, base, false);
		reset_traffic(base);
		app.detail_open(item("custom:no-metadata", "other"));
		idle(app);
		expect(app.d_video_id_ == "custom:no-metadata" && app.d_stream_list_.size() == 4,
		       "missing metadata falls back to a single video with the exact meta ID");
		expect(contains_path(traffic(base)["requests"], "/stream/other/custom%3Ano-metadata.json"),
		       "fallback never invents an IMDb or episode ID");

		app.addons_.clear();
		app.addons_.push_back(fixture_addon(base, "external", "External provider", {"stream"}));
		app.detail_open(item("external:one", "movie"));
		idle(app);
		expect(app.d_stream_list_.size() == 1 && !app.d_stream_list_[0].playable() &&
		       app.d_sources.empty() && app.d_streams.empty() && app.d_streams_status.find("No compatible streams") != std::string::npos,
		       "external descriptor is retained with an unavailable reason but no unusable source tab");
		app.play_stream(app.d_stream_list_[0], false);
		expect(!app.launch_visible && !app.watching_ && !app.toast.empty(), "unsupported source is stopped before native playback");
	}

	static void stale_results(App& app, const std::string& base) {
		install_stream_fixtures(app, base);
		app.detail_open(item("kitsu:absent"));
		idle(app);
		reset_traffic(base);
		app.detail_load_streams("old:queued");
		until([&] { return contains_path(traffic(base)["completed"], "/fast/stream/anime/old%3Aqueued.json"); },
		      "old result queued for UI", false);
		app.detail_load_streams("new:queued");
		idle(app);
		bool all_current = app.d_stream_list_.size() == 4;
		for (const auto& stream : app.d_stream_list_) all_current = all_current && jstr(stream.raw, "fixtureId") == "new:queued";
		expect(all_current && app.d_answered_ == 2, "queued callbacks from an old video cannot corrupt current provider buckets/counts");

		reset_traffic(base);
		app.detail_load_streams("close:queued");
		until([&] { return contains_path(traffic(base)["completed"], "/fast/stream/anime/close%3Aqueued.json"); },
		      "result queued before leaving detail", false);
		app.detail_close();
		idle(app);
		expect(app.view == "home" && app.d_stream_list_.empty(), "late stream callbacks cannot repopulate a closed detail page");

		reset_traffic(base);
		app.detail_open(item("meta:slow"));
		until([&] { return contains_path(traffic(base)["requests"], "/meta/meta/anime/meta%3Aslow.json"); },
		      "old metadata request in flight", false);
		app.detail_open(item("meta:new"));
		idle(app);
		expect(app.d_item_.id == "meta:new" && app.d_meta_.id == "meta:new" && app.d_name == "Full meta:new",
		       "old metadata callback cannot replace a subsequently opened item");
	}

	static void identical_provider_focus(App& app, const std::string& base) {
		idle(app);
		app.addons_.clear();
		app.addons_.push_back(fixture_addon(base, "slow", "Same display name", {"stream"}));
		app.addons_.push_back(fixture_addon(base, "fast", "Same display name", {"stream"}));
		app.detail_open(item("ambiguous:identity"));
		until([&] { return app.d_video_id_ == "ambiguous:identity" && app.d_answered_ == 1; },
		      "fast configured installation ready");
		expect(app.d_stream_list_.size() == 2 && app.d_stream_list_[1].request_headers ==
		       std::vector<std::string>{"X-Fixture-Provider: fast"}, "fast installation supplies its distinct media headers");
		app.d_stream_sel = 1;
		idle(app);
		expect(app.d_stream_sel == 3 && app.d_stream_list_[app.d_view_[app.d_stream_sel]].request_headers ==
		       std::vector<std::string>{"X-Fixture-Provider: fast"},
		       "focus stays on the same installation even when another has identical display name, URLs and stream titles");
	}

	static json catalog_manifest() {
		return json::array({
		    json{{"type", "other"}, {"id", "my/list"}, {"name", "Required region"},
		         {"extra", json::array({json{{"name", "region"}, {"isRequired", true}, {"options", {"IT", "US"}}},
		                                  json{{"name", "genre"}, {"options", {"A & B", "Comedy"}}}, json{{"name", "skip"}}})}},
		    json{{"type", "other"}, {"id", "no-paging"}, {"name", "Single page"}},
		    json{{"type", "other"}, {"id", "search-list"}, {"name", "Search list"},
		         {"extra", json::array({json{{"name", "search"}, {"isRequired", true}},
		                                  json{{"name", "region"}, {"isRequired", true}, {"options", {"IT"}}}})}},
		    json{{"type", "other"}, {"id", "free-filter"}, {"name", "Custom filter"},
		         {"extra", json::array({json{{"name", "collection"}, {"isRequired", true}}})}}
		});
	}
	static void select_catalog(App& app, const std::string& id) {
		for (size_t index = 0; index < app.disc_catalogs_.size(); index++)
			if (app.disc_catalogs_[index].catalog->id == id) {
				app.disc_catalog_ = int(index);
				app.discover_build_chips();
				app.discover_load(false);
				return;
			}
		throw std::runtime_error("Catalog missing in UI: " + id);
	}
	static void catalogs(App& app, const std::string& base) {
		idle(app);
		app.addons_.clear();
		auto addon = fixture_addon(base, "catalog", "Catalogs", {"stream"}, catalog_manifest());
		// Real Torrentio-style mismatch: catalogs do not inherit stream filters.
		addon->types = {"movie"};
		addon->id_prefixes = {"tt"};
		app.addons_.push_back(std::move(addon));
		app.view = "home";
		reset_traffic(base);
		app.build_home();
		idle(app);
		expect(app.board_.size() == 3 && app.board_[1].catalog.id == "my/list",
		       "home includes required-enum catalogs, excludes search-only and missing free-text values");
		expect(contains_path(traffic(base)["requests"], "/catalog/other/my%2Flist/region=IT.json"),
		       "home requests required default option for catalog outside manifest stream types/prefixes");

		app.view = "discover";
		app.disc_catalog_ = 0;
		app.disc_type_.clear();
		app.disc_items_.clear();
		app.disc_filter_catalog_key_.clear();
		reset_traffic(base);
		app.enter_discover();
		idle(app);
		expect(app.disc_types_ == std::vector<std::string>{"other"} && app.disc_extra_keys_ == std::vector<std::string>({"region", "genre"}),
		       "discover exposes custom type and generic manifest filters");
		expect(app.disc_items_.size() == 2 && app.disc_skip_ == 4, "pagination offset counts raw records, including duplicates and invalid previews");
		app.discover_load(true);
		idle(app);
		expect(app.disc_items_.size() == 4 && app.disc_skip_ == 7, "pagination deduplicates by type plus ID, not ID alone");
		expect(contains_path(traffic(base)["requests"], "region=IT&skip=4.json"), "next request uses raw provider offset instead of visible card count");
		app.discover_load(true);
		idle(app);
		expect(app.disc_end_ && contains_path(traffic(base)["requests"], "skip=7.json"), "empty page ends pagination after the correct offset");
		const auto completed = traffic(base)["requests"].size();
		app.discover_load(true);
		expect(traffic(base)["requests"].size() == completed, "finished catalog does not keep issuing duplicate pages");

		app.zone = "filters";
		app.disc_chip = 3;  // genre
		app.discover_button(Btn::Cross);
		expect(app.dd_visible && app.dd_options.size() == 3, "generic enum filter opens declared choices and optional All");
		app.dropdown_button(Btn::Down);
		app.dropdown_button(Btn::Cross);
		idle(app);
		expect(app.disc_extra_values_["genre"] == "A & B" &&
		       contains_path(traffic(base)["requests"], "region=IT&genre=A%20%26%20B.json"),
		       "controller filter selection resets pagination and safely encodes reserved characters");

		select_catalog(app, "no-paging");
		idle(app);
		const auto no_paging_requests = traffic(base)["requests"].size();
		app.discover_load(true);
		expect(app.disc_end_ && traffic(base)["requests"].size() == no_paging_requests,
		       "catalog without a skip declaration is never paginated speculatively");
		const auto detailed_item = app.disc_items_.front();
		app.detail_open(detailed_item);
		expect(app.d_meta_loaded_ && app.d_stream_list_.size() == 1 && app.d_video_id_ == "single:video" &&
		       app.d_stream_list_[0].addon == "Catalogs", "metasDetailed catalog carries inline video and provider through Item into detail");
		expect(traffic(base)["requests"].size() == no_paging_requests, "complete catalog metadata and inline streams need no speculative addon requests");
		app.view = "discover";
		select_catalog(app, "free-filter");
		expect(app.disc_status.find("required collection") != std::string::npos && !app.disc_loading_,
		       "required free-text extra asks for a value instead of silently hiding the catalog or issuing invalid request");
		app.zone = "filters";
		app.disc_chip = 2;
		app.discover_button(Btn::Cross);
		expect(app.input_visible_, "required custom filter uses the existing text-entry flow");
		app.input_value = "My list & friends";
		app.input_finish(true);
		idle(app);
		expect(contains_path(traffic(base)["requests"], "collection=My%20list%20%26%20friends.json") && app.disc_items_.size() == 1,
		       "custom required filter reaches the addon and loads its catalog");

		reset_traffic(base);
		app.start_search("Lupin & Jigen+ 2");
		idle(app);
		expect(app.search_.size() == 1 && app.search_[0].items.size() == 1 &&
		       contains_path(traffic(base)["requests"], "region=IT&search=Lupin%20%26%20Jigen%2B%202.json"),
		       "global search includes other required enum defaults and preserves the complete search term");
		reset_traffic(base);
		app.start_search("old search");
		until([&] { return contains_path(traffic(base)["requests"], "search=old%20search"); }, "old search in flight", false);
		app.start_search("current search");
		idle(app);
		expect(app.search_query == "current search" && app.search_.size() == 1 && app.search_[0].items.size() == 1 &&
		       app.search_[0].items[0].name == "current search" && app.search_pending_ == 0,
		       "old search callback cannot replace current search results or decrement the current pending count");
	}

	static void subtitles(App& app, const std::string& base) {
		idle(app);
		app.addons_.clear();
		app.addons_.push_back(fixture_addon(base, "subtitles", "Community fixture", {"subtitles"}));
		app.w_item_ = item("catalog:one");
		app.w_video_id_ = "opaque/episode:one";
		app.w_stream_ = Stream();
		app.w_stream_.video_hash = "0123456789abcdef";
		app.w_stream_.video_size = 50000000000ULL;
		app.w_stream_.has_video_size = true;
		app.w_stream_.filename = "Episode & 1.mkv";
		app.w_sub_cancel_ = std::make_shared<std::atomic<bool>>(false);
		app.w_sub_requests_.clear();
		app.w_subs_.clear();
		app.w_sub_pending_ = 0;
		app.w_menu_built_ = false;
		++app.w_gen_;
		reset_traffic(base);
		app.watch_load_addon_subtitles();
		idle(app);
		expect(app.w_subs_.size() == 2 && app.w_subs_[0].lang == "ita" &&
		       app.w_subs_[0].label.find("Italiano alternativo") != std::string::npos,
		       "subtitle provider labels and preferred regional language survive the real callback");
		expect(contains_path(traffic(base)["requests"],
		       "/subtitles/anime/opaque%2Fepisode%3Aone/videoHash=0123456789abcdef&videoSize=50000000000&filename=Episode%20%26%201.mkv.json"),
		       "actual subtitle request sends video ID in the path and file hash/64-bit size/filename in extras");
		app.watch_load_addon_subtitles();
		expect(app.w_sub_pending_ == 0 && traffic(base)["requests"].size() == 1,
		       "identical subtitle enrichment does not repeat the same provider request");
		app.w_stream_.video_hash = "fedcba9876543210";
		app.watch_load_addon_subtitles();
		idle(app);
		expect(traffic(base)["requests"].size() == 2 && app.w_subs_.size() == 2,
		       "new file matching parameters retry addons and merge alternatives without duplicate tracks");
		SubtitleTrack track;
		track.id = "inline-it"; track.url = base + "/inline.srt"; track.lang = "it";
		track.label = "Italiano embedded URL"; track.addon = "Inline";
		app.watch_add_subtitles({track, track});
		expect(app.w_subs_.size() == 3, "inline subtitle merge keeps a unique alternative rather than duplicating it");
	}

	static void run(const std::string& base) {
		App app;
		app.offline_ = true;
		app.settings_.auto_subtitles = false;
		try {
			details(app, base);
			stale_results(app, base);
			identical_provider_focus(app, base);
			catalogs(app, base);
			subtitles(app, base);
			idle(app);
		} catch (...) {
			for (auto* cancel : {&app.home_cancel_, &app.search_cancel_, &app.disc_cancel_,
			                     &app.d_stream_cancel_, &app.d_meta_cancel_, &app.w_sub_cancel_, &app.launch_cancel_})
				if (*cancel) (*cancel)->store(true);
			try { idle(app); } catch (...) {}
			throw;
		}
	}
};

int main(int argc, char** argv) {
	if (argc != 2) { std::cerr << "Loopback fixture URL required\n"; return 2; }
	http_init("");
	g_tasks.start(6);
	try {
		AppProtocolTest::run(argv[1]);
		g_tasks.stop();
		std::cout << "PASS: " << assertions << " real App addon integration assertions\n";
		return 0;
	} catch (const std::exception& error) {
		g_tasks.stop();
		std::cerr << "FAIL after " << assertions << " assertions: " << error.what() << '\n';
		return 1;
	}
}
