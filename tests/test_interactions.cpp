// Real App/Tasks interaction regressions; only network/art boundaries are replaced.
#include "app.h"
#include <curl/curl.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
int checks = 0;
std::atomic<int> unexpected_http{0}, get_calls{0}, write_calls{0};
void expect(bool condition, const char* name) {
    ++checks;
    if (!condition) throw std::runtime_error(name);
}
template<class Predicate> void until(Predicate condition, const char* description, bool drain = true) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        if (drain) g_tasks.drain();
        if (condition()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(std::string("Timed out: ") + description);
}
struct Reply {
    explicit Reply(ApiResult result) : value(std::move(result)) {}
    ApiResult value;
    std::atomic<bool> entered{false};
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
    std::string key;
    json payload;
    void release() { std::lock_guard<std::mutex> lock(mutex); released = true; cv.notify_all(); }
    ApiResult call() {
        entered = true;
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(8), [&] { return released; }))
            throw std::runtime_error("Unreleased boundary reply");
        return value;
    }
};
std::mutex boundary_mutex;
std::deque<std::shared_ptr<Reply>> writes, reads;
std::map<std::string, std::deque<std::shared_ptr<Reply>>> resources;
std::vector<std::shared_ptr<Reply>> all_replies;
std::vector<std::string> fetched_resources;
std::map<std::string, std::map<std::string, json>> remote;
std::shared_ptr<Reply> reply(ApiResult result, bool immediate = false) {
    auto value = std::make_shared<Reply>(std::move(result));
    all_replies.push_back(value);
    if (immediate) value->release();
    return value;
}
std::shared_ptr<Reply> write_reply(bool ok = true, bool immediate = false) {
    auto value = reply({ok, json::object(), ok ? "" : "fixture connection failure"}, immediate);
    std::lock_guard<std::mutex> lock(boundary_mutex); writes.push_back(value); return value;
}
std::shared_ptr<Reply> resource_reply(const std::string& prefix, json body, bool immediate = false) {
    auto value = reply({true, std::move(body), {}}, immediate);
    std::lock_guard<std::mutex> lock(boundary_mutex); resources[prefix].push_back(value); return value;
}
std::shared_ptr<Reply> resource_error(const std::string& prefix, const std::string& error, bool immediate = true) {
    auto value = reply({false, {}, error}, immediate);
    std::lock_guard<std::mutex> lock(boundary_mutex); resources[prefix].push_back(value); return value;
}
Item media(const std::string& id, const std::string& type = "movie") {
    Item value;
    value.id = id; value.type = type; value.name = "Title " + id;
    value.poster = "https://art.test/" + id + ".jpg";
    value.background = "https://art.test/" + id + "-hero.jpg";
    value.logo = "https://art.test/" + id + "-logo.png";
    return value;
}
json library_item(const Item& item, int64_t offset = 0) {
    return {{"_id", item.id}, {"name", item.name}, {"type", item.type}, {"poster", item.poster},
            {"removed", false}, {"temp", false}, {"_mtime", iso8601_now()},
            {"state", {{"timeOffset", offset}, {"duration", 900000}, {"video_id", "episode:2"},
                       {"timeWatched", 40000}, {"overallTimeWatched", 50000}, {"timesWatched", 2},
                       {"lastWatched", iso8601_now()}}}};
}
std::unique_ptr<Addon> addon(const std::string& route, const std::string& name,
                             json resources = json::array({"stream"})) {
    auto value = std::make_unique<Addon>();
    const json manifest = {{"id", "interaction." + route}, {"name", name},
        {"resources", std::move(resources)}, {"types", json::array({"movie", "series", "anime"})}};
    if (!parse_addon(manifest, "https://sources.test/" + route + "/manifest.json?token=" + route, *value))
        throw std::runtime_error("Invalid fixture addon");
    return value;
}
json stream(const std::string& title, const std::string& marker) {
    return {{"name", title}, {"url", "https://media.test/same.mkv"},
            {"description", "🇮🇹 🇬🇧 👤 142 · 4K · 12.5 GB"},
            {"behaviorHints", {{"proxyHeaders", {{"request", {{"X-Installation", marker}}}}}}}};
}
} // namespace

ApiResult wrapped_put(const std::string&, const json&) asm(WRAP_LIBRARY_PUT);
ApiResult wrapped_put(const std::string& key, const json& payload) {
    std::shared_ptr<Reply> planned;
    {
        std::lock_guard<std::mutex> lock(boundary_mutex);
        if (writes.empty()) return {false, {}, "unplanned fixture write"};
        planned = writes.front(); writes.pop_front(); ++write_calls;
        planned->key = key; planned->payload = payload;
    }
    ApiResult result = planned->call();
    if (result.ok) {
        std::lock_guard<std::mutex> lock(boundary_mutex);
        for (const auto& item : payload) remote[key][jstr(item, "_id")] = item;
    }
    return result;
}
ApiResult wrapped_get(const std::string&) asm(WRAP_LIBRARY_GET);
ApiResult wrapped_get(const std::string& key) {
    ++get_calls;
    std::shared_ptr<Reply> planned;
    {
        std::lock_guard<std::mutex> lock(boundary_mutex);
        if (!reads.empty()) { planned = reads.front(); reads.pop_front(); }
        else {
            json items = json::array();
            for (const auto& item : remote[key]) items.push_back(item.second);
            return {true, items, {}};
        }
    }
    return planned->call();
}
bool wrapped_fetch(const std::string&, json&, std::string&, long, const std::atomic<bool>*) asm(WRAP_FETCH_JSON);
bool wrapped_fetch(const std::string& url, json& body, std::string& error, long, const std::atomic<bool>* cancel) {
    std::shared_ptr<Reply> planned;
    {
        std::lock_guard<std::mutex> lock(boundary_mutex);
        fetched_resources.push_back(url);
        for (auto& prefix : resources) {
            if (url.rfind(prefix.first, 0) != 0 || prefix.second.empty()) continue;
            planned = prefix.second.front(); prefix.second.pop_front(); planned->key = url; break;
        }
    }
    if (!planned) { error = "unplanned fixture resource"; return false; }
    ApiResult result = planned->call();
    if (cancel && cancel->load()) { error = "cancelled"; return false; }
    body = result.result; error = result.error; return result.ok;
}
std::string wrapped_art_get(ArtCache*, const std::string&, ArtKind, std::function<void(const std::string&)>, ArtPriority, const void*) asm(WRAP_ART_GET);
std::string wrapped_art_get(ArtCache*, const std::string& url, ArtKind, std::function<void(const std::string&)>, ArtPriority, const void*) {
    return url.empty() ? "" : "fixture:" + url;
}
std::string wrapped_art_peek(ArtCache*, const std::string&, ArtKind) asm(WRAP_ART_PEEK);
std::string wrapped_art_peek(ArtCache*, const std::string& url, ArtKind) { return url.empty() ? "" : "fixture:" + url; }
extern "C" CURLcode __wrap_curl_easy_perform(CURL*) { ++unexpected_http; return CURLE_COULDNT_CONNECT; }

struct AppProtocolTest {
    static void idle(App& app) { until([&] { return app.busy_count_ == 0; }, "App worker callbacks"); }
    static void prepare(App& app, const std::string& directory) {
        app.data_dir_ = directory; app.settings_.auth_key = "ACCOUNT_A"; app.settings_.ui_language = "it";
        app.library_loaded_ = true; app.view = "home"; app.zone = "content";
        BoardRow cw; cw.title = "Continue Watching"; cw.continue_watching = true; cw.loaded = true;
        BoardRow row; row.title = "Catalog"; row.loaded = true; row.items = {media("movie:one"), media("series:two", "series")};
        app.board_ = {cw, row}; app.refresh_home_cards();
    }
    static void cover_routes(App& app) {
        auto first = write_reply();
        app.on_button(Btn::Square);
        expect(app.in_library("movie:one") && app.home_rows[0].cards[0].in_library,
               "Home Square immediately adds the focused cover and bookmark badge");
        expect(app.view == "home" && app.toast == "Aggiunto alla libreria", "Home Square remains Home and shows a small add notification");
        until([&] { return first->entered.load(); }, "first favourite write");
        expect(first->payload[0]["removed"] == false && first->payload[0]["temp"] == false,
               "new favourite uses permanent Stremio library flags");
        expect(first->payload[0]["state"].contains("overallTimeWatched") && first->payload[0]["state"].contains("video_id"),
               "new favourite initializes the complete account progress schema");
        auto second = write_reply();
        app.on_button(Btn::Square); app.on_button(Btn::Square);
        expect(app.in_library("movie:one") && write_calls == 1 && app.library_pending_changes_.size() == 1,
               "rapid remove/add presses coalesce behind one in-flight write");
        first->release(); until([&] { return second->entered.load(); }, "coalesced favourite write");
        expect(second->payload.size() == 1 && second->payload[0]["removed"] == false,
               "serialized second request contains only the latest favourite intention");
        second->release(); idle(app);
        expect(app.library_pending_changes_.empty() && !app.library_write_inflight_, "successful favourite writer drains completely");

        app.view = "search"; app.search_ = {app.board_[1]}; app.search_row = app.search_col = 0; app.refresh_search_cards();
        auto remove = write_reply(true, true); app.on_button(Btn::Square); idle(app);
        expect(!app.in_library("movie:one") && !app.search_rows[0].cards[0].in_library,
               "Search Square removes an existing favourite and updates its badge");
        expect(app.toast == "Rimosso dalla libreria", "favourite removal has its own concise notification");

        app.view = "discover"; app.zone = "content"; app.disc_items_ = {media("movie:one")};
        app.disc_cards = {app.make_card(app.disc_items_[0], false)}; app.disc_sel = 0;
        write_reply(true, true); app.on_button(Btn::Square); idle(app);
        expect(app.in_library("movie:one") && app.disc_cards[0].in_library && !app.disc_loading_,
               "Discover Square toggles a favourite instead of reloading its catalog");

        app.view = "library"; app.zone = "content"; app.library_refresh(); app.lib_sel = 0;
        write_reply(true, true); app.on_button(Btn::Square); idle(app);
        expect(app.lib_cards.empty() && !app.in_library("movie:one"), "Library Square removes the selected saved item");
        expect(app.lib_sel == 0, "removing the last library cover leaves a valid empty selection");
    }
    static void initial_library_load(App& app) {
        app.library_loaded_ = false;
        auto initial = reply({true, json::array({library_item(media("movie:one"))}), {}});
        { std::lock_guard<std::mutex> lock(boundary_mutex); reads.push_back(initial); }
        app.on_button(Btn::Square);
        expect(write_calls == 0 && app.library_.empty(), "Square before initial account snapshot does not guess whether to add or remove");
        expect(app.toast == "Caricamento della libreria…", "unknown library membership reports loading clearly");
        until([&] { return initial->entered.load(); }, "initial account snapshot requested by Square");
        initial->release(); idle(app);
        expect(app.library_loaded_ && app.in_library("movie:one"), "initial account membership becomes available before later toggle actions");
        app.library_.clear(); app.refresh_library_membership();
    }
    static void progress_separation(App& app) {
        const Item item = media("series:two", "series");
        app.library_[item.id] = library_item(item, 230000);
        app.progress_[item.id] = {item.type, item.name, item.poster, "episode:2", 230, 900, now_epoch_ms()};
        app.view = "home"; app.zone = "content"; app.refresh_home_cards();
        expect(app.home_rows[0].continue_watching, "Continue Watching carries explicit row identity for controller hints");
        app.home_row = 1; app.home_col = 1;
        write_reply(true, true); app.on_button(Btn::Square); idle(app);
        expect(!app.in_library(item.id) && app.continue_watching().size() == 1,
               "removing a favourite preserves its independent Continue Watching entry");
        expect(jbool(app.library_[item.id], "temp") && jnum(app.library_[item.id]["state"], "timeOffset") == 230000,
               "ongoing episode and exact resume offset survive favourite removal");
        write_reply(true, true); app.on_button(Btn::Square); idle(app);
        expect(app.in_library(item.id) && !jbool(app.library_[item.id], "temp"), "adding a temporary progress record back promotes it to a favourite");
        app.home_row = 0; app.home_col = 0;
        write_reply(true, true); app.on_button(Btn::Square); idle(app);
        expect(app.in_library(item.id) && app.continue_watching().empty(), "Continue Watching Square removes progress without removing the favourite");
        expect(app.progress_[item.id].time == 0 && jnum(app.library_[item.id]["state"], "timeOffset") == 0,
               "Continue Watching removal clears local and account resume offsets");
        expect(jnum(app.library_[item.id]["state"], "overallTimeWatched") == 50000 &&
               jstr(app.library_[item.id]["state"], "video_id") == "episode:2",
               "Continue Watching removal preserves watch history and episode identity");
        expect(app.toast == "Rimosso da Continua a guardare", "Continue Watching notification is distinct from library removal");
    }
    static void failures_and_accounts(App& app) {
        const Item failed = media("failure:new");
        auto failure = write_reply(false); app.toggle_library(failed);
        until([&] { return failure->entered.load(); }, "failed add request");
        failure->release(); idle(app);
        expect(!app.in_library(failed.id) && !app.library_.count(failed.id), "failed new favourite write rolls back the optimistic record");
        expect(app.toast.find("Impossibile") != std::string::npos, "failed write replaces optimistic toast with a useful error");

        const Item existing = media("failure:existing"); app.library_[existing.id] = library_item(existing, 12000);
        auto failed_remove = write_reply(false); app.toggle_library(existing);
        auto latest_add = write_reply(true); app.toggle_library(existing);
        failed_remove->release(); until([&] { return latest_add->entered.load(); }, "newer edit after a failed request");
        expect(app.in_library(existing.id), "older write failure cannot roll back a newer local add intention");
        latest_add->release(); idle(app);
        expect(jnum(app.library_[existing.id]["state"], "timeOffset") == 12000, "successful newer edit retains original progress");
        const json confirmed = app.library_[existing.id];

        auto bad_one = write_reply(false); app.toggle_library(existing);
        auto bad_two = write_reply(false); app.toggle_library(existing);
        bad_one->release(); until([&] { return bad_two->entered.load(); }, "second failed mutation");
        bad_two->release(); idle(app);
        expect(app.in_library(existing.id) && app.library_[existing.id] == confirmed,
               "multiple failed coalesced edits restore the last confirmed state rather than a failed intermediate edit");

        auto old_account = write_reply(false); app.toggle_library(media("old:account"));
        until([&] { return old_account->entered.load(); }, "old account write in flight");
        ++app.account_generation_; app.settings_.auth_key = "ACCOUNT_B";
        app.library_.clear(); app.library_pending_changes_.clear(); app.library_original_changes_.clear();
        app.library_write_inflight_ = false; app.library_refresh_pending_ = false; ++app.library_revision_;
        auto new_account = write_reply(true); app.toggle_library(media("new:account"));
        until([&] { return new_account->entered.load(); }, "new account write in flight");
        old_account->release(); until([&] { return app.busy_count_ == 1; }, "late old-account response");
        expect(app.library_write_inflight_ && app.in_library("new:account") && !app.library_.count("old:account"),
               "old account callback cannot roll back or unlock the new account writer");
        new_account->release(); idle(app);
        expect(new_account->key == "ACCOUNT_B", "new write uses only the current account key");

        auto stale_read = reply({true, json::array(), {}});
        { std::lock_guard<std::mutex> lock(boundary_mutex); reads.push_back(stale_read); }
        app.load_library(); until([&] { return stale_read->entered.load(); }, "library refresh started");
        auto fresh_write = write_reply(true); app.toggle_library(media("fresh:edit"));
        stale_read->release(); until([&] { return app.library_refresh_pending_; }, "stale refresh deferred");
        expect(app.in_library("fresh:edit"), "stale account fetch cannot overwrite a newer favourite change");
        fresh_write->release(); idle(app);
        expect(app.in_library("fresh:edit") && !app.library_refresh_pending_, "deferred account refresh runs after the serialized writer drains");

        auto older = reply({true, json::array({library_item(media("older:snapshot"))}), {}});
        auto newer = reply({true, json::array({library_item(media("newer:snapshot"))}), {}});
        { std::lock_guard<std::mutex> lock(boundary_mutex); reads.push_back(older); reads.push_back(newer); }
        app.load_library(); until([&] { return older->entered.load(); }, "older overlapping account GET");
        app.load_library(); until([&] { return newer->entered.load(); }, "newer overlapping account GET");
        newer->release(); until([&] { return app.in_library("newer:snapshot"); }, "newer account snapshot applied");
        older->release(); idle(app);
        expect(app.in_library("newer:snapshot") && !app.library_.count("older:snapshot"),
               "overlapping GET responses keep the latest requested snapshot even when the old request finishes last");
    }
    static void series(App& app) {
        app.addons_.clear();
        Item item = media("opaque:series", "series");
        item.metadata_addon = "Series metadata"; item.metadata_addon_url = "https://meta.test/configured/manifest.json";
        item.inline_meta = {{"id", item.id}, {"type", "series"}, {"name", "Ordered show"},
            {"logo", "https://art.test/title-logo.png"},
            {"behaviorHints", {{"defaultVideoId", "opaque/season2:episode1"}, {"hasScheduledVideos", true}}},
            {"videos", json::array({
                {{"id", "opaque/season1:episode2"}, {"season", 1}, {"episode", 2}, {"title", "Second"}, {"thumbnail", "https://art.test/e2.jpg"}},
                {{"id", "opaque/season2:episode1"}, {"season", 2}, {"episode", 1}, {"title", "Season two"},
                 {"overview", "The selected episode synopsis."},
                 {"thumbnail", "https://art.test/s2e1.jpg"}, {"streams", json::array({stream("Inline 4K", "inline")})}},
                {{"id", "opaque/special"}, {"season", 0}, {"episode", 1}, {"title", "Special"}},
                {{"id", "opaque/season1:episode1"}, {"season", 1}, {"episode", 1}, {"title", "First"}},
                {{"id", "opaque/unknown"}, {"title", "Unnumbered"}}})}};
        app.detail_open(item);
        expect(app.d_series && app.d_zone == "episodes" && app.d_stream_list_.empty(),
               "series without a meta endpoint opens its inline episode carousel directly");
        expect(app.d_seasons_ == std::vector<int>({1, 2, 0, -1}), "regular seasons, explicit specials and unknown seasons remain distinct");
        expect(app.d_seasons.size() == 4 && app.d_seasons[2].value == "Speciali" && app.d_season_sel == 1,
               "public season tabs expose every season and the selected index");
        expect(app.d_season_idx_ == 1 && app.d_episodes[0].title == "Season two",
               "defaultVideoId preselects an episode without embedding its number in its title");
        expect(app.d_episodes[0].number == "1" && app.d_episodes[0].description == "The selected episode synopsis.",
               "episode number and synopsis are separately available to the carousel UI");
        expect(app.d_episodes[0].thumb.find("s2e1.jpg") != std::string::npos && app.d_logo.find("title-logo.png") != std::string::npos,
               "episode thumbnails and supplied movie logos reach the actual UI model");
        app.on_button(Btn::L1);
        expect(app.d_season_idx_ == 0 && app.d_episodes[0].title == "First",
               "L1 selects the previous season and episodes are ordered by episode number");
        app.on_button(Btn::Right);
        expect(app.d_zone == "episodes" && app.d_episode_sel == 1 && app.d_episodes[1].title == "Second",
               "Right browses the next horizontal episode without opening streams");
        app.on_button(Btn::Left);
        expect(app.d_episode_sel == 0, "Left browses the previous horizontal episode");
        app.on_button(Btn::R1); app.on_button(Btn::Cross);
        expect(app.d_zone == "streams" && app.d_video_id_ == "opaque/season2:episode1" && app.d_streams.size() == 1,
               "Cross on an episode opens only that exact opaque episode's streams");
        expect(app.d_stream_list_[0].addon_url == item.metadata_addon_url, "inline source retains configured metadata installation identity");
        expect(app.d_sources.size() == 1 && app.d_sources[0].value == "Series metadata",
               "one exclusive metadata source has one tab and no redundant All tab");
        expect(app.d_streams[0].seeders == 142 && app.d_streams[0].languages == std::vector<std::string>({"ita", "eng"}),
               "stream list exposes parsed language flags and seeders directly in row data");
        app.on_button(Btn::Circle);
        expect(app.d_zone == "episodes" && app.d_episode_sel == 0, "Circle returns from sources directly to the chosen episode");
        app.on_button(Btn::R1);
        expect(app.d_season_label == "Speciali", "R1 reaches specials from regular seasons");
        app.on_button(Btn::R1);
        expect(app.d_season_label == "Episodi", "missing season numbers are labelled Episodes, never invented as Specials");
        app.on_button(Btn::R1);
        expect(app.d_season_sel == 0, "season shoulders wrap back to the first regular season");
        app.on_button(Btn::Circle);
        expect(app.view == app.return_view_, "Circle from episodes directly returns to the previous page");
    }
    static void complete_series_metadata(App& app) {
        idle(app); app.addons_.clear();
        app.addons_.push_back(addon("full", "Full catalog", json::array({"meta"})));
        app.addons_.push_back(addon("streams", "Stream provider"));
        Item item = media("tt:ted-fixture", "series");
        item.video_id = "opaque/ted:3:1";
        item.metadata_addon = "Home catalog";
        item.metadata_addon_url = "https://catalog.test/manifest.json";
        item.inline_meta = {{"id", item.id}, {"type", "series"}, {"name", "Ted fixture"},
            {"description", "Useful catalog description"}, {"logo", "https://art.test/ted-title.png"},
            {"videos", json::array({
                {{"id", "opaque/ted:3:1"}, {"season", 3}, {"episode", 1}, {"title", "Third season"},
                 {"thumbnail", "https://art.test/ted-s3e1.jpg"}, {"overview", "Preserved episode overview"},
                 {"streams", json::array({stream("Catalog exclusive", "catalog-inline")})}},
                {{"id", "opaque/ted:4:1"}, {"season", 4}, {"episode", 1}, {"title", "Fourth season"}}
            })}};
        json full = {{"id", item.id}, {"type", "series"}, {"name", "Full series"},
            {"videos", json::array({
                {{"id", "opaque/ted:0:1"}, {"season", 0}, {"episode", 1}, {"title", "Special"}},
                {{"id", "opaque/ted:1:1"}, {"season", 1}, {"episode", 1}, {"title", "First season"}},
                {{"id", "opaque/ted:2:1"}, {"season", 2}, {"episode", 1}, {"title", "Second season"}},
                {{"id", "opaque/ted:3:1"}, {"season", 3}, {"episode", 1}, {"title", "Third season full"}},
                {{"id", "opaque/ted:4:1"}, {"season", 4}, {"episode", 1}, {"title", "Fourth season full"}, {"streams", json::array()}}
            })}};
        auto response = resource_reply("https://sources.test/full/meta/", {{"meta", full}});
        app.detail_open(item);
        until([&] { return response->entered.load(); }, "full meta resource requested despite catalog videos");
        expect(app.d_meta_pending_ && app.d_zone == "episodes" && app.d_seasons_ == std::vector<int>({3, 4}),
               "recent catalog episodes remain browsable while complete metadata is loading");
        app.on_button(Btn::R1);
        expect(app.d_season_sel == 1, "user can change season during full metadata request");
        app.on_button(Btn::Cross);
        expect(app.d_zone == "streams" && app.d_pending_video_id_ == "opaque/ted:4:1" && app.d_pending_ == 0,
               "Cross retains exact pending episode while waiting for metadata stream exclusivity");
        response->release(); idle(app);
        expect(app.d_seasons_ == std::vector<int>({1, 2, 3, 4, 0}) && app.d_seasons.size() == 5,
               "Home recent-episodes subset expands to all four seasons and specials from full meta");
        expect(!app.d_meta_pending_ && app.d_zone == "streams" && app.d_season_sel == 3 && app.d_video_id_ == "opaque/ted:4:1",
               "metadata completion preserves selected season and automatically opens requested episode streams");
        expect(app.d_stream_list_.empty() && app.d_sources.empty() && app.d_streams_status.find("metadata addon") != std::string::npos,
               "full metadata explicit empty streams stay exclusive with no fake source tab");
        expect(app.d_description == "Useful catalog description" && app.d_logo.find("ted-title.png") != std::string::npos,
               "full metadata keeps useful catalog description and logo when endpoint omits them");
        app.on_button(Btn::Circle); app.on_button(Btn::L1);
        expect(app.d_season_sel == 2 && app.d_episodes[0].description == "Preserved episode overview" &&
               app.d_episodes[0].thumb.find("ted-s3e1.jpg") != std::string::npos,
               "merged full-series episodes preserve missing catalog thumbnail and synopsis");
        app.on_button(Btn::Cross);
        expect(app.d_stream_list_.size() == 1 && app.d_stream_list_[0].addon_url == item.metadata_addon_url &&
               app.d_sources.size() == 1 && app.d_sources[0].value == "Home catalog",
               "catalog inline exclusivity and original installation survive full metadata from another provider");
        {
            std::lock_guard<std::mutex> lock(boundary_mutex);
            expect(std::none_of(fetched_resources.begin(), fetched_resources.end(), [](const auto& url) {
                return starts_with(url, "https://sources.test/streams/stream/");
            }), "full-meta and catalog exclusive arrays make zero stream-provider requests");
        }
        app.on_button(Btn::Circle); app.on_button(Btn::L1); app.on_button(Btn::L1);
        expect(app.d_season_sel == 0 && app.d_episodes[0].title == "First season", "Home detail can navigate back to season one");
        app.on_button(Btn::L1);
        expect(app.d_season_label == "Speciali", "Home detail can also navigate to specials");

        response = resource_reply("https://sources.test/full/meta/", {{"meta", full}});
        app.detail_open(item); app.on_button(Btn::R1); app.on_button(Btn::Cross); app.on_button(Btn::Circle);
        app.on_button(Btn::L1);
        response->release(); idle(app);
        expect(app.d_zone == "episodes" && app.d_pending_video_id_.empty() && app.d_season_sel == 2,
               "Circle and season navigation cancel pending stream intent without losing current episode focus");

        response = resource_reply("https://sources.test/full/meta/", {{"meta", full}});
        app.detail_open(item); app.on_button(Btn::Cross);
        expect(app.d_meta_pending_ && app.d_zone == "streams" && app.d_streams.size() == 1,
               "catalog-declared exclusive stream can be opened immediately while full series metadata loads");
        const std::string inline_identity = app.d_source_urls_[app.d_source_sel];
        response->release(); idle(app);
        expect(app.d_zone == "streams" && app.d_streams.size() == 1 && app.d_streams[0].name == "Catalog exclusive" &&
               app.d_source_urls_[app.d_source_sel] == inline_identity && app.d_video_id_ == "opaque/ted:3:1",
               "full metadata does not reset an already open inline stream list or its configured source identity");

        resource_error("https://sources.test/full/meta/", "HTTP 503");
        app.detail_open(item); idle(app);
        expect(app.d_meta_loaded_ && !app.d_meta_pending_ && app.d_seasons_ == std::vector<int>({3, 4}),
               "failed metadata endpoint retains usable inline catalog episodes instead of a single-video fallback");

        response = resource_reply("https://sources.test/full/meta/", {{"meta", full}});
        app.detail_open(item); until([&] { return response->entered.load(); }, "metadata in flight for old account");
        const int account = app.account_generation_;
        ++app.account_generation_; response->release(); idle(app);
        expect(app.d_seasons_ == std::vector<int>({3, 4}), "stale account metadata cannot replace current detail state");
        app.account_generation_ = account;
        app.detail_close();
    }
    static void provider_sources(App& app) {
        app.addons_.clear(); app.addons_.push_back(addon("first", "Same provider")); app.addons_.push_back(addon("second", "Same provider"));
        Item item = media("opaque:movie"); item.inline_meta = {{"id", item.id}, {"type", "movie"}, {"name", item.name}};
        auto slow = resource_reply("https://sources.test/first/", {{"streams", json::array({stream("Shared result", "first")})}});
        auto fast = resource_reply("https://sources.test/second/", {{"streams", json::array({stream("Shared result", "second"), stream("Next ranked 720p", "second")})}});
        app.detail_open(item);
        expect(app.d_sources.empty(), "loading provider manifests do not create empty selectable source tabs");
        fast->release(); until([&] { return app.d_answered_ == 1; }, "second provider replies first");
        expect(app.d_sources.size() == 1 && app.d_sources[0].value == "Same provider" && app.d_streams.size() == 2 &&
               app.d_view_ == std::vector<int>({0, 1}), "a single usable provider is displayed directly without All");
        app.on_button(Btn::R1);
        expect(app.d_source_sel == 0 && app.d_streams.size() == 2 && app.view == "detail", "R1 on one source stays within that provider");
        app.d_stream_sel = 1; slow->release(); idle(app);
        expect(app.d_sources.size() == 3 && app.d_sources[1].value != app.d_sources[2].value,
               "two usable installations expose All and distinct same-name provider tabs");
        expect(app.d_source_sel == 2 && app.d_stream_sel == 1 && app.d_view_ == std::vector<int>({1, 2}),
               "later arrival of an earlier provider preserves explicitly selected source and row identity");
        expect(app.d_stream_list_[app.d_view_[0]].request_headers == std::vector<std::string>({"X-Installation: second"}),
               "source filtering uses installation URL rather than equal titles or equal media URLs");
        app.on_button(Btn::L1);
        expect(app.d_source_sel == 1 && app.d_streams.size() == 1 && app.d_view_[0] == 0,
               "L1 selects the first provider with original stream-index mapping intact");
        app.on_button(Btn::L1);
        expect(app.d_source_sel == 0 && app.d_view_ == std::vector<int>({0, 1, 2}), "All sources preserves installed provider and provider result ordering");
        app.on_button(Btn::L1);
        expect(app.d_source_sel == 2, "source shoulders wrap across the source tabs");
        app.on_button(Btn::Options);
        expect(app.d_pick_res && !app.d_streams.empty(), "optional quality chooser is available via Options");
        app.on_button(Btn::Circle);
        expect(!app.d_pick_res && app.view == "detail", "Circle dismisses quality chooser without leaving selected video");
    }
    static void usable_sources_only(App& app) {
        idle(app); app.addons_.clear(); app.settings_.server_url.clear();
        auto local = std::make_unique<Addon>();
        const json local_manifest = {{"id", "org.stremio.local"}, {"name", "Local Files (without catalog support)"},
            {"resources", json::array({"stream"})}, {"types", json::array({"movie", "series"})}, {"idPrefixes", json::array({"tt"})}};
        expect(parse_addon(local_manifest, "http://127.0.0.1:11470/local-addon/manifest.json", *local), "official local addon fixture parses");
        app.addons_.push_back(std::move(local));
        app.addons_.push_back(addon("empty", "Empty catalog"));
        app.addons_.push_back(addon("unsupported", "External only"));
        app.addons_.push_back(addon("not-found", "No resource"));
        app.addons_.push_back(addon("torrentio", "Torrentio"));
        const json external = {{"name", "External link"}, {"externalUrl", "https://watch.test/title"}};
        resource_reply("https://sources.test/empty/", {{"streams", json::array()}}, true);
        resource_reply("https://sources.test/unsupported/", {{"streams", json::array({external})}}, true);
        resource_error("https://sources.test/not-found/", "HTTP 404");
        resource_reply("https://sources.test/torrentio/", {{"streams", json::array({stream("Ready 1080p", "ready")})}}, true);
        app.detail_open(media("tt-source-fixture")); idle(app);
        expect(app.d_pending_ == 4 && app.d_sources.size() == 1 && app.d_sources[0].value == "Torrentio" && app.d_streams.size() == 1,
               "only playable Torrentio creates a source tab among local, empty, unsupported and absent providers");
        expect(app.d_streams_status.empty() && app.d_stream_list_.size() == 2 && !app.d_stream_list_[0].playable(),
               "unsupported descriptors remain available for diagnostics without masquerading as errors or selectable streams");
        {
            std::lock_guard<std::mutex> lock(boundary_mutex);
            expect(std::none_of(fetched_resources.begin(), fetched_resources.end(), [](const auto& url) {
                return starts_with(url, "http://127.0.0.1:11470/local-addon/");
            }), "desktop Local Files endpoint is never contacted without a configured service");
        }
        app.addons_.erase(app.addons_.begin() + 1, app.addons_.end());
        app.settings_.server_url = "http://configured-service.test:11470/";
        auto mapped = resource_reply("http://configured-service.test:11470/local-addon/", {{"streams", json::array({stream("Real local stream", "local")})}}, true);
        app.detail_open(media("tt-source-fixture")); idle(app);
        expect(mapped->entered.load() && app.d_sources.size() == 1 && app.d_streams.size() == 1 &&
               mapped->key == "http://configured-service.test:11470/local-addon/stream/movie/tt-source-fixture.json",
               "explicitly configured service can supply Local Files through its real remote endpoint");
        app.settings_.server_url.clear();
        app.addons_.clear(); app.addons_.push_back(addon("failed", "Failed provider")); app.addons_.push_back(addon("torrentio", "Torrentio"));
        resource_error("https://sources.test/failed/", "HTTP 503");
        resource_reply("https://sources.test/torrentio/", {{"streams", json::array({stream("Ready", "ready")})}}, true);
        app.detail_open(media("tt-error-fixture")); idle(app);
        expect(app.d_sources.size() == 1 && app.d_sources[0].value == "Torrentio" &&
               app.d_streams_status.find("Failed provider: HTTP 503") != std::string::npos,
               "real provider outage remains diagnosable without adding a dead source tab");
        app.addons_.clear(); app.addons_.push_back(addon("unsupported", "External only"));
        resource_reply("https://sources.test/unsupported/", {{"streams", json::array({external})}}, true);
        app.detail_open(media("tt-external-fixture")); idle(app);
        expect(app.d_sources.empty() && app.d_streams.empty() && app.d_stream_list_.size() == 1 &&
               app.d_streams_status.find("No compatible streams") != std::string::npos &&
               app.d_streams_status.find("could not answer") == std::string::npos,
               "unsupported-only results explain compatibility separately from network/provider errors");
    }
    static void player(App& app) {
        app.watching_ = true; app.launch_visible = false; app.menu_visible = false; app.info_visible = false;
        app.on_button(Btn::Up);
        expect(app.info_visible && !app.menu_visible, "player Up reveals playback controls without opening audio/subtitle menu");
        app.on_button(Btn::Circle);
        expect(!app.info_visible && app.watching_ && app.w_info_until_ == 0,
               "first Circle hides playback controls and clears their timer without stopping playback");
        app.on_button(Btn::Down);
        expect(app.info_visible && !app.menu_visible && app.w_info_until_ > now_seconds(),
               "player Down reveals playback controls and restarts their timeout");
        app.on_button(Btn::Options);
        expect(app.menu_visible, "Options opens the audio/subtitle sheet");
        app.on_button(Btn::L1);
        expect(app.m_col == 0 && app.menu_visible, "L1 selects Audio within the track sheet");
        app.on_button(Btn::R1);
        expect(app.m_col == 1 && app.menu_visible, "R1 selects Subtitles within the track sheet");
        app.on_button(Btn::Left);
        expect(app.m_col == 0, "directional Audio selection remains available alongside shoulders");
        app.on_button(Btn::Circle);
        expect(!app.menu_visible && app.watching_ && app.info_visible, "closing track sheet leaves playback active with controls visible");

        app.settings_.seek_seconds = 10;
        app.settings_.shoulder_seek_seconds = 75;
        app.player_.seek(120);
        app.on_button(Btn::R1);
        expect(app.player_.position() == 195, "R1 uses the configured independent shoulder seek interval");
        app.on_button(Btn::L1);
        expect(app.player_.position() == 120, "L1 seeks backward by the configured shoulder interval");
        app.on_button(Btn::Right);
        expect(app.player_.position() == 130, "directional seek keeps its separate shorter interval");
        app.on_button(Btn::Left);
        expect(app.player_.position() == 120, "directional backward seek remains independent of shoulders");

        app.player_.set_paused(true); app.w_paused = true; app.w_buffering = true;
        app.on_button(Btn::Circle);
        expect(!app.info_visible && app.watching_ && app.player_.paused(),
               "Circle can hide controls while paused and buffering without resuming or stopping");
        app.watch_update();
        expect(!app.info_visible && app.watching_ && app.player_.paused(),
               "next player update keeps manually hidden paused controls hidden");
        app.on_button(Btn::Up); app.on_button(Btn::Circle); app.on_button(Btn::Circle);
        expect(!app.watching_ && !app.menu_visible, "Circle exits playback only after controls are already hidden");
    }
    static void run(const std::string& directory) {
        App app; prepare(app, directory);
        try {
            initial_library_load(app); cover_routes(app); progress_separation(app); failures_and_accounts(app);
            series(app); complete_series_metadata(app); provider_sources(app); usable_sources_only(app); player(app); idle(app);
            expect(unexpected_http == 0, "all interaction regressions performed zero real HTTP requests");
        } catch (...) {
            for (auto& value : all_replies) value->release();
            try { idle(app); } catch (...) {}
            throw;
        }
    }
};

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    g_tasks.start(4);
    int result = 0;
    try { AppProtocolTest::run(argv[1]); std::cout << "PASS: " << checks << " real App interaction checks; real HTTP=" << unexpected_http << "\n"; }
    catch (const std::exception& error) { std::cerr << "FAIL after " << checks << " checks: " << error.what() << "\n"; result = 1; }
    for (auto& value : all_replies) value->release();
    g_tasks.stop();
    return result;
}
