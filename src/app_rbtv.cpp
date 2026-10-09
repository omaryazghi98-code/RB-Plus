// Native RBTV+ catalogue, match details, and direct-stream handoff.
#include "app.h"

#include <algorithm>
#include <ctime>

#include "http.h"

namespace {
constexpr uint64_t kRbtvSports[] = {1, 2, 3, 4, 6, 11, 90};

uint64_t selected_sport(int index) {
    const int clamped = std::clamp(index, 0, int(sizeof(kRbtvSports) / sizeof(kRbtvSports[0])) - 1);
    return kRbtvSports[clamped];
}

std::string match_title(const rbtv::Match& match) {
    if (match.has_home && match.has_away && !match.home.name.empty() && !match.away.name.empty())
        return match.home.name + " vs " + match.away.name;
    if (!match.name.empty()) return match.name;
    return "Match " + std::to_string(match.id);
}

std::string kickoff_label(uint64_t raw) {
    if (!raw) return "Time not supplied";
    uint64_t seconds = raw;
    if (seconds > 100000000000ULL) seconds /= 1000;
    // Only format plausible Unix timestamps. Unknown provider units remain
    // visible as a raw value instead of silently displaying a false date.
    if (seconds < 946684800ULL || seconds > 4102444800ULL)
        return "Time code " + std::to_string(raw);
    const std::time_t stamp = static_cast<std::time_t>(seconds);
    std::tm calendar{};
    if (!gmtime_r(&stamp, &calendar)) return "Time code " + std::to_string(raw);
    char buffer[64]{};
    if (!std::strftime(buffer, sizeof(buffer), "%a %d %b · %H:%M UTC", &calendar))
        return "Time code " + std::to_string(raw);
    return buffer;
}
}

rbtv::ApiConfig App::rbtv_api_config() const {
    rbtv::ApiConfig config;
    config.data_api = settings_.rbtv_data_api;
    config.web_origin = settings_.rbtv_web_origin;
    config.digit = settings_.rbtv_digit.empty() ? "snd" : settings_.rbtv_digit;
    config.user_agent = "RBTV-PS5-Native/0.4";
    config.timeout_seconds = 12;
    config.max_response_bytes = 20u * 1024u * 1024u;
    return config;
}

void App::rbtv_cancel_requests() {
    for (auto* request : {&rbtv_cancel_, &rbtv_detail_cancel_, &rbtv_stream_cancel_})
        if (*request) (*request)->store(true);
    ++rbtv_generation_;
    ++rbtv_detail_generation_;
    ++rbtv_stream_generation_;
    rbtv_loading_ = rbtv_detail_loading_ = rbtv_stream_resolving_ = false;
}

void App::rbtv_load_matches() {
    if (!settings_.rbtv_connection_approved) {
        rbtv_status = "Service access is not approved. Review the endpoint settings before connecting.";
        show_toast("RBTV+ network access has not been approved.", 5);
        dirty_all();
        return;
    }
    if (settings_.rbtv_data_api.empty() || settings_.rbtv_web_origin.empty()) {
        rbtv_status = "Set both HTTPS endpoints in Settings before loading the catalogue.";
        dirty_all();
        return;
    }
    if (rbtv_loading_) {
        rbtv_status = "The catalogue request is already running.";
        return;
    }
    if (rbtv_cancel_) rbtv_cancel_->store(true);
    const auto cancel = std::make_shared<std::atomic<bool>>(false);
    rbtv_cancel_ = cancel;
    const int generation = ++rbtv_generation_;
    const auto config = rbtv_api_config();
    const uint64_t sport = selected_sport(rbtv_sport_sel);
    const uint64_t language = 0; // APK's default language code; configurable later.
    rbtv_loading_ = true;
    rbtv_status = "Requesting the live catalogue from RBTV+…";
    dirty_all();

    struct Result {
        bool ok = false;
        std::string error;
        rbtv::LiveResult live;
    };
    bg<Result>([config, sport, language, cancel]() {
        Result result;
        result.ok = rbtv::get_live_matches(config, sport, language, result.live, result.error, cancel.get());
        return result;
    }, [this, generation](Result& result) {
        if (generation != rbtv_generation_) return;
        rbtv_loading_ = false;
        if (!result.ok) {
            rbtv_status = "Catalogue request failed: " + result.error;
            show_toast("Could not load the RBTV+ catalogue.", 5);
            dirty_all();
            return;
        }
        rbtv_match_data_ = std::move(result.live.matches);
        rbtv_live_stream_data_ = std::move(result.live.streams);
        rbtv_matches.clear();
        rbtv_matches.reserve(rbtv_match_data_.size());
        for (const auto& match : rbtv_match_data_) {
            UiRbtvMatch item;
            item.id = match.id;
            item.sport_type = match.sport_type ? match.sport_type : selected_sport(rbtv_sport_sel);
            item.title = match_title(match);
            item.league = match.has_league && !match.league.name.empty() ? match.league.name : "League";
            item.home = match.has_home && !match.home.name.empty() ? match.home.name : item.title;
            item.away = match.has_away ? match.away.name : "";
            item.score = (match.has_home || match.has_away)
                ? std::to_string(match.home_score) + " : " + std::to_string(match.away_score) : "—";
            item.kickoff = kickoff_label(match.date);
            item.status = "Status code " + std::to_string(match.status);
            item.hot = match.hot;
            rbtv_matches.push_back(std::move(item));
        }
        rbtv_match_sel = std::clamp(rbtv_match_sel, 0, std::max(0, int(rbtv_matches.size()) - 1));
        rbtv_status = "Loaded " + std::to_string(rbtv_matches.size()) +
                      " real events from the service. No sample fixtures were added.";
        if (rbtv_matches.empty()) rbtv_status += " The live catalogue is empty for this sport/language.";
        dirty_all();
    });
}

void App::rbtv_open_match(const UiRbtvMatch& item) {
    auto found = std::find_if(rbtv_match_data_.begin(), rbtv_match_data_.end(), [&](const rbtv::Match& match) {
        return match.id == item.id && (match.sport_type == item.sport_type || match.sport_type == 0);
    });
    if (found == rbtv_match_data_.end()) {
        show_toast("That event is no longer in the loaded catalogue. Refresh the list.", 5);
        return;
    }
    if (rbtv_detail_cancel_) rbtv_detail_cancel_->store(true);
    if (rbtv_stream_cancel_) rbtv_stream_cancel_->store(true);
    const auto cancel = std::make_shared<std::atomic<bool>>(false);
    rbtv_detail_cancel_ = cancel;
    const int generation = ++rbtv_detail_generation_;
    ++rbtv_stream_generation_;
    rbtv_stream_resolving_ = false;
    rbtv_selected_match_ = *found;
    rbtv_detail_stream_data_.clear();
    rbtv_streams.clear();
    rbtv_stream_sel = 0;
    rbtv_detail_title = item.title;
    rbtv_detail_league = item.league;
    rbtv_detail_teams = item.home + (item.away.empty() ? "" : " vs " + item.away);
    rbtv_detail_score = item.score;
    rbtv_detail_kickoff = item.kickoff;
    rbtv_detail_status = item.status;
    rbtv_detail_message = "Loading stream choices from RBTV+…";
    rbtv_detail_loading_ = true;
    set_view("rbtv-detail");
    dirty_all();

    struct Result {
        bool ok = false, has_match = false;
        std::string error;
        rbtv::Match match;
        std::vector<rbtv::Stream> streams;
    };
    const auto config = rbtv_api_config();
    const uint64_t match_id = item.id;
    const uint64_t sport = item.sport_type;
    bg<Result>([config, match_id, sport, cancel]() {
        Result result;
        result.ok = rbtv::get_match_detail(config, match_id, sport, 0, result.match,
                                           result.has_match, result.streams, result.error, cancel.get());
        return result;
    }, [this, generation](Result& result) {
        if (generation != rbtv_detail_generation_ || view != "rbtv-detail") return;
        rbtv_detail_loading_ = false;
        if (!result.ok) {
            rbtv_detail_message = "Could not load streams: " + result.error;
            dirty_all();
            return;
        }
        if (result.has_match) rbtv_selected_match_ = result.match;
        const auto& match = rbtv_selected_match_;
        rbtv_detail_title = match_title(match);
        rbtv_detail_league = match.has_league ? match.league.name : "";
        rbtv_detail_teams = match.has_home && match.has_away
            ? match.home.name + " vs " + match.away.name : match.name;
        rbtv_detail_score = (match.has_home || match.has_away)
            ? std::to_string(match.home_score) + " : " + std::to_string(match.away_score) : "—";
        rbtv_detail_kickoff = kickoff_label(match.date);
        rbtv_detail_status = "Status code " + std::to_string(match.status);
        rbtv_detail_stream_data_ = std::move(result.streams);
        rbtv_streams.clear();
        rbtv_streams.reserve(rbtv_detail_stream_data_.size());
        for (const auto& stream : rbtv_detail_stream_data_) {
            UiRbtvStream ui;
            ui.id = stream.id;
            ui.sport_type = stream.sport_type ? stream.sport_type : match.sport_type;
            ui.site_type = stream.site_type;
            ui.name = !stream.full_name.empty() ? stream.full_name :
                      !stream.name.empty() ? stream.name : "Stream " + std::to_string(stream.id);
            ui.description = "Status code " + std::to_string(stream.status) +
                             " · site type " + std::to_string(stream.site_type);
            ui.recommended = stream.recommended;
            rbtv_streams.push_back(std::move(ui));
        }
        rbtv_stream_sel = 0;
        rbtv_detail_message = rbtv_streams.empty()
            ? "The service returned no stream choices for this event."
            : "Select a source and press Cross. Only sources returned by RBTV+ are shown.";
        dirty_all();
    });
}

void App::rbtv_play_stream(int index) {
    if (!settings_.rbtv_connection_approved) {
        rbtv_detail_message = "Service access is not approved. Review Settings before resolving a stream.";
        dirty_all();
        return;
    }
    if (index < 0 || index >= int(rbtv_detail_stream_data_.size())) return;
    if (rbtv_stream_resolving_) {
        rbtv_detail_message = "Stream resolution is already running.";
        return;
    }
    if (rbtv_stream_cancel_) rbtv_stream_cancel_->store(true);
    const auto cancel = std::make_shared<std::atomic<bool>>(false);
    rbtv_stream_cancel_ = cancel;
    const int generation = ++rbtv_stream_generation_;
    const auto config = rbtv_api_config();
    const rbtv::Stream source = rbtv_detail_stream_data_[size_t(index)];
    const rbtv::Match match = rbtv_selected_match_;
    rbtv_stream_resolving_ = true;
    rbtv_detail_message = "Resolving the selected RBTV+ source…";
    dirty_all();

    struct Result {
        bool ok = false;
        std::string error;
        rbtv::Stream stream;
    };
    bg<Result>([config, source, match, cancel]() {
        Result result;
        result.ok = rbtv::resolve_stream(config, match.id,
            source.sport_type ? source.sport_type : match.sport_type,
            source.id, source.site_type, rbtv::UserInfo{}, result.stream, result.error, cancel.get());
        return result;
    }, [this, generation, match](Result& result) {
        if (generation != rbtv_stream_generation_ || view != "rbtv-detail") return;
        rbtv_stream_resolving_ = false;
        if (!result.ok) {
            rbtv_detail_message = "Stream resolution failed: " + result.error;
            show_toast("RBTV+ could not resolve this stream.", 5);
            dirty_all();
            return;
        }
        if (result.stream.url.empty()) {
            rbtv_detail_message = "This source did not return a direct URL. The Android P2P/CSL middleware is not ported yet.";
            show_toast("This RBTV+ source needs middleware that is not implemented yet.", 8);
            dirty_all();
            return;
        }
        if (!http_valid_url(result.stream.url)) {
            rbtv_detail_message = "The service returned an invalid media URL; playback was not started.";
            dirty_all();
            return;
        }

        Stream playback;
        playback.name = result.stream.full_name.empty() ? result.stream.name : result.stream.full_name;
        if (playback.name.empty()) playback.name = "RBTV+ stream";
        playback.addon = "RBTV+";
        playback.url = result.stream.url;
        playback.kind = StreamKind::Direct;
        for (const auto& [key, value] : result.stream.headers)
            playback.request_headers.push_back(key + ": " + value);

        d_item_ = Item{};
        d_item_.id = "rbtv:" + std::to_string(match.id);
        d_item_.type = "movie";
        d_item_.name = match_title(match);
        d_item_.description = match.has_league ? match.league.name : "";
        d_video_id_ = std::to_string(match.id);
        d_meta_ = Meta{};
        d_meta_loaded_ = false;
        d_meta_pending_ = false;
        d_series = false;
        d_year.clear();
        play_stream(playback, false);
    });
}

void App::rbtv_button(Btn button) {
    if (button == Btn::Options) { set_view("settings"); return; }
    if (button == Btn::Circle) { set_view("home"); return; }
    if (button == Btn::Square) { rbtv_load_matches(); return; }

    if (zone == "rbtv-sports") {
        if (button == Btn::Left || button == Btn::Right) {
            const int delta = button == Btn::Left ? -1 : 1;
            const int count = int(rbtv_sports.size());
            if (count > 0) rbtv_sport_sel = (rbtv_sport_sel + delta + count) % count;
            rbtv_matches.clear();
            rbtv_match_data_.clear();
            rbtv_live_stream_data_.clear();
            rbtv_match_sel = 0;
            rbtv_status = "Sport selected. Press Cross to request its live catalogue.";
            dirty_all();
        } else if (button == Btn::Down) {
            zone = "content";
            rbtv_match_sel = std::clamp(rbtv_match_sel, 0, std::max(0, int(rbtv_matches.size()) - 1));
            dirty_all();
        } else if (button == Btn::Cross) {
            rbtv_load_matches();
        }
        return;
    }

    const int count = int(rbtv_matches.size());
    if (button == Btn::Up) {
        if (rbtv_match_sel >= 2) rbtv_match_sel -= 2;
        else zone = "rbtv-sports";
    } else if (button == Btn::Down) {
        if (rbtv_match_sel + 2 < count) rbtv_match_sel += 2;
    } else if (button == Btn::Left) {
        if (rbtv_match_sel > 0) --rbtv_match_sel;
    } else if (button == Btn::Right) {
        if (rbtv_match_sel + 1 < count) ++rbtv_match_sel;
    } else if (button == Btn::Cross) {
        if (rbtv_matches.empty()) rbtv_load_matches();
        else rbtv_open_match(rbtv_matches[size_t(std::clamp(rbtv_match_sel, 0, count - 1))]);
    }
    dirty_all();
}

void App::rbtv_detail_button(Btn button) {
    if (button == Btn::Circle) { set_view("rbtv"); return; }
    if (button == Btn::Options) { set_view("settings"); return; }
    if (button == Btn::Square) {
        UiRbtvMatch item;
        item.id = rbtv_selected_match_.id;
        item.sport_type = rbtv_selected_match_.sport_type;
        item.title = rbtv_detail_title;
        item.league = rbtv_detail_league;
        item.home = rbtv_selected_match_.has_home ? rbtv_selected_match_.home.name : rbtv_detail_title;
        item.away = rbtv_selected_match_.has_away ? rbtv_selected_match_.away.name : "";
        item.score = rbtv_detail_score;
        item.kickoff = rbtv_detail_kickoff;
        item.status = rbtv_detail_status;
        item.hot = rbtv_selected_match_.hot;
        rbtv_open_match(item);
        return;
    }
    if (rbtv_detail_loading_ || rbtv_stream_resolving_) return;
    const int count = int(rbtv_streams.size());
    if (button == Btn::Up || button == Btn::Left) {
        if (rbtv_stream_sel > 0) --rbtv_stream_sel;
    } else if (button == Btn::Down || button == Btn::Right) {
        if (rbtv_stream_sel + 1 < count) ++rbtv_stream_sel;
    } else if (button == Btn::Cross) {
        if (count == 0) {
            UiRbtvMatch item;
            item.id = rbtv_selected_match_.id;
            item.sport_type = rbtv_selected_match_.sport_type;
            item.title = rbtv_detail_title;
            item.league = rbtv_detail_league;
            item.home = rbtv_selected_match_.has_home ? rbtv_selected_match_.home.name : rbtv_detail_title;
            item.away = rbtv_selected_match_.has_away ? rbtv_selected_match_.away.name : "";
            item.score = rbtv_detail_score;
            item.kickoff = rbtv_detail_kickoff;
            item.status = rbtv_detail_status;
            rbtv_open_match(item);
        } else rbtv_play_stream(rbtv_stream_sel);
    }
    dirty_all();
}
