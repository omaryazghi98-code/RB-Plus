// Downloads of the highlighted stream, with independent offline playback.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "download_text.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
std::string download_bytes(int64_t bytes) {
    if (bytes < 0) return {};
    char value[64];
    if (bytes >= (1ll << 30)) std::snprintf(value, sizeof(value), "%.2f GiB", double(bytes) / (1ll << 30));
    else std::snprintf(value, sizeof(value), "%.1f MiB", double(bytes) / (1ll << 20));
    return value;
}
}

void App::download_selected_stream(const Stream& stream) {
    const bool it = settings_.ui_language == "it";
    if (!stream.playable()) {
        show_toast(it ? "Questa sorgente non può essere scaricata." : "This source cannot be downloaded.");
        return;
    }
    DownloadRequest request;
    request.media_id = d_item_.id;
    request.type = d_item_.type;
    request.video_id = d_video_id_;
    request.title = d_meta_loaded_ && !d_meta_.name.empty() ? d_meta_.name : d_item_.name;
    request.stream = stream; // Exactly one selected result, including this installation's headers.
    request.live = d_meta_loaded_ && (d_meta_.is_live || d_meta_.has_scheduled_videos);
    const auto poster = d_meta_loaded_ && !d_meta_.poster.empty() ? d_meta_.poster : d_item_.poster;
    const auto background = d_meta_loaded_ && !d_meta_.background.empty() ? d_meta_.background : d_item_.background;
    const auto logo = d_meta_loaded_ && !d_meta_.logo.empty() ? d_meta_.logo : d_item_.logo;
    request.poster_url = poster;
    request.poster_path = g_art.peek_cached(poster, ArtKind::PosterLarge);
    if (request.poster_path.empty()) request.poster_path = g_art.peek_cached(poster, ArtKind::Poster);
    request.background_path = g_art.peek_cached(background, ArtKind::Backdrop);
    if (request.background_path.empty()) request.background_path = g_art.peek_cached(background, ArtKind::Background);
    request.logo_path = g_art.peek_cached(logo, ArtKind::LaunchLogo);
    if (request.logo_path.empty()) request.logo_path = g_art.peek_cached(logo, ArtKind::LogoBox);
    if (d_series) for (const auto& video : d_meta_.videos) if (video.id == d_video_id_) {
        request.season = video.season; request.episode = video.episode;
        request.subtitle = "S" + std::to_string(video.season) + " · E" + std::to_string(video.episode);
        if (!video.title.empty()) request.subtitle += " · " + video.title;
        break;
    }
    if (request.subtitle.empty()) request.subtitle = d_year;
    if (request.live) {
        show_toast(it ? "Le dirette non possono essere scaricate per la visione offline." :
                      "Live streams cannot be downloaded for offline viewing.");
        return;
    }
    // Response overrides need the same configured proxy as ordinary playback.
    if (!stream.url.empty() && !stream.response_headers.empty() && !server().empty()) {
        request.stream.url = stream_proxy_url(stream, server());
        request.stream.request_headers.clear();
        request.stream.response_headers.clear();
        if (request.stream.url.empty()) {
            show_toast(it ? "Impossibile preparare questa sorgente." : "Could not prepare this source.");
            return;
        }
    }
    const auto account = account_generation_;
    const bool submitted = downloads_.enqueue_async(std::move(request),
        [this, account](std::string id, std::string error) {
            g_tasks.post([this, account, id = std::move(id), error = std::move(error)] {
                if (account != account_generation_) return;
                const bool it = settings_.ui_language == "it";
                if (id.empty()) {
                    dlog("Selected stream could not be queued: %s", error.c_str());
                    const auto storage_error = downloads_.storage_error();
                    if (!storage_error.empty()) dlog("Download storage failure: %s", storage_error.c_str());
                    show_toast((it ? "Download non disponibile: " : "Download unavailable: ") + download_error_text(error, it), 6);
                    return;
                }
                refresh_download_artwork();
                downloads_refresh();
                show_toast(it ? "Video aggiunto a Download" : "Video added to Downloads", 2.5);
            });
        });
    if (submitted) show_toast(it ? "Aggiunta a Download…" : "Adding to Downloads…", 2);
    else show_toast(it ? "Impossibile aggiungere altri download in questo momento." : "Could not add another download right now.", 4);
}

void App::refresh_download_artwork() {
    if (download_art_account_ != account_generation_) {
        if (download_art_cancel_) download_art_cancel_->store(true);
        download_art_cancel_ = std::make_shared<std::atomic<bool>>(false);
        download_art_account_ = account_generation_;
        download_meta_pending_.clear(); download_meta_retry_.clear();
    }
    const auto entries = downloads_.snapshot();
    const double now = now_seconds();
    const int account = account_generation_;
    for (auto it = download_meta_retry_.begin(); it != download_meta_retry_.end();) {
        if (std::none_of(entries.begin(), entries.end(), [&](const DownloadEntry& entry) { return entry.id == it->first; }))
            it = download_meta_retry_.erase(it);
        else ++it;
    }
    std::set<std::string> requested_urls;
    for (const auto& entry : entries) {
        // The worker validates and publishes private artwork paths. Polling
        // every row with stat() here can stall navigation behind media writes.
        if (!entry.poster_path.empty()) continue;
        std::string poster = entry.poster_url;
        if (poster.empty()) {
            // Older manifests had only a temporary cache path. Reuse already
            // loaded catalog metadata before making a provider request.
            const auto take = [&](const Item& item) {
                if (poster.empty() && item.id == entry.media_id && item.type == entry.type) poster = item.poster;
            };
            take(d_item_);
            for (const auto& row : board_) for (const auto& item : row.items) take(item);
            for (const auto& item : disc_items_) take(item);
            for (const auto& item : lib_items_) take(item);
            if (!poster.empty()) downloads_.update_poster_source(entry.id, poster);
        }
        if (poster.empty()) {
            if (offline_ || !signed_in() || download_meta_pending_.size() >= 2 ||
                download_meta_pending_.count(entry.id) || now < download_meta_retry_[entry.id]) continue;
            std::vector<std::string> providers;
            for (const auto& addon : addons_) if (addon->supports("meta", entry.type, entry.media_id)) {
                providers.push_back(addon->resource_url("meta", entry.type, entry.media_id));
                if (providers.size() == 3) break;
            }
            if (providers.empty()) continue;
            const auto cancel = download_art_cancel_;
            const auto id = entry.id;
            download_meta_pending_.insert(id); download_meta_retry_[id] = now + 60;
            g_tasks.run<std::string>([providers, cancel] {
                for (const auto& url : providers) {
                    if (cancel->load()) break;
                    json document; std::string error;
                    if (!fetch_json(url, document, error, 6, cancel.get())) continue;
                    const auto poster = jstr(jobj(document, "meta"), "poster");
                    if (!poster.empty() && poster.size() <= 16384) return poster;
                }
                return std::string{};
            }, [this, id, cancel, account](std::string& url) {
                if (account != account_generation_ || cancel != download_art_cancel_ || cancel->load()) return;
                download_meta_pending_.erase(id);
                if (!url.empty() && downloads_.update_poster_source(id, url)) download_art_refresh_at_ = 0;
            });
            continue;
        }
        if (!requested_urls.insert(poster).second) continue;
        // A shared URL may belong to multiple queued episodes: one callback
        // updates all matching entries and cannot be lost to observer dedupe.
        const auto accept = [this, poster, account, cancel = download_art_cancel_](const std::string& path) {
            if (path.empty() || account != account_generation_ || cancel != download_art_cancel_ || cancel->load()) return;
            for (const auto& item : downloads_.snapshot())
                if (item.poster_url == poster) downloads_.update_poster(item.id, path);
            images_dirty_ = true;
            if (view == "downloads") downloads_refresh();
        };
        auto cached = g_art.peek_cached(poster, ArtKind::PosterLarge);
        if (cached.empty()) cached = g_art.peek_cached(poster, ArtKind::Poster);
        if (cached.empty()) cached = g_art.get_async(poster, ArtKind::Poster, accept, ArtPriority::Visible, &downloads_);
        if (!cached.empty()) accept(cached);
    }
}

void App::downloads_refresh() {
    const bool it = settings_.ui_language == "it";
    const auto selected = download_sel >= 0 && download_sel < int(download_rows.size()) ? download_rows[download_sel].id : "";
    // Capture before the snapshot so a concurrent completion is seen on the next poll.
    download_revision_ = downloads_.revision();
    const auto entries = downloads_.snapshot();
    download_rows.clear();
    size_t complete = 0, pending = 0;
    for (const auto& entry : entries) {
        UiDownload row;
        row.id = entry.id; row.title = entry.title; row.subtitle = entry.subtitle;
        row.image = entry.poster_path.empty() ? entry.background_path : entry.poster_path;
        row.progress = std::isfinite(entry.progress) ? float(entry.progress) : -1;
        row.complete = entry.state == DownloadState::Complete;
        row.active = entry.state == DownloadState::Downloading;
        row.failed = entry.state == DownloadState::Failed;
        row.paused = entry.state == DownloadState::Paused;
        row.playable_while_downloading = entry.playable_while_downloading;
        switch (entry.state) {
        case DownloadState::Complete: row.status = it ? "Disponibile offline" : "Available offline"; ++complete; break;
        case DownloadState::Downloading: row.status = it ? "Download in corso" : "Downloading"; ++pending; break;
        case DownloadState::Queued: row.status = it ? "In coda" : "Queued"; ++pending; break;
        case DownloadState::Paused: row.status = it ? "In pausa" : "Paused"; break;
        case DownloadState::Waiting: row.status = it ? "In attesa del motore torrent" : "Waiting for the torrent engine"; ++pending; break;
        case DownloadState::Failed:
            row.status = (it ? "Download interrotto" : "Download interrupted") +
                (entry.error.empty() ? std::string{} : " · " + download_error_text(entry.error, it));
            break;
        }
        row.size = download_bytes(entry.done);
        if (entry.total > 0 && !row.complete) row.size += " / " + download_bytes(entry.total);
        if (row.active && entry.bytes_per_second > 0 && std::isfinite(entry.bytes_per_second))
            row.speed = download_bytes(int64_t(std::min(entry.bytes_per_second, double(INT64_MAX / 2)))) + "/s";
        if (row.active) {
            row.remaining = download_remaining_text(entry.remaining_seconds, it);
            row.connections = download_connections_text(entry.connected_peers, entry.connected_seeders, it);
        }
        download_rows.push_back(std::move(row));
    }
    download_sel = std::clamp(download_sel, 0, std::max(0, int(download_rows.size()) - 1));
    for (size_t i = 0; i < download_rows.size(); ++i) if (download_rows[i].id == selected) { download_sel = int(i); break; }
    if (entries.empty()) download_status = it ? "I video che scarichi appariranno qui." : "Downloaded videos will appear here.";
    else download_status = std::to_string(complete) + (it ? " disponibili offline" : " available offline") +
        (pending ? " · " + std::to_string(pending) + (it ? " in coda o in download" : " queued or downloading") : "");
    dirty_all();
}

void App::downloads_button(Btn button) {
    const bool it = settings_.ui_language == "it";
    if (button == Btn::Up && download_sel > 0) --download_sel;
    else if (button == Btn::Down && download_sel + 1 < int(download_rows.size())) ++download_sel;
    else if (button == Btn::Circle) { set_view("home"); return; }
    else if ((button == Btn::Cross || button == Btn::Square || button == Btn::Options) &&
             download_sel >= 0 && download_sel < int(download_rows.size())) {
        const auto id = download_rows[download_sel].id;
        const auto entry = downloads_.find(id);
        if (!entry) { downloads_refresh(); return; }
        if (button == Btn::Options) {
            play_download(id, entry->state != DownloadState::Complete);
            return;
        } else if (button == Btn::Cross) {
            if (entry->state == DownloadState::Complete) { play_download(id); return; }
            if (entry->state == DownloadState::Paused || entry->state == DownloadState::Failed) downloads_.resume(id);
            else downloads_.pause(id);
            downloads_refresh();
        } else if (button == Btn::Square) {
            const auto title = entry->title + (entry->subtitle.empty() ? "" : " · " + entry->subtitle);
            open_dropdown((it ? "Eliminare " : "Delete ") + title + "?",
                {it ? "Annulla" : "Cancel", it ? "Elimina download e file" : "Delete download and files"}, 0,
                [this, id](int selected) {
                    if (selected != 1) return;
                    std::string error;
                    if (!downloads_.remove(id, error)) {
                        dlog("Download removal failed: %s", error.c_str());
                        show_toast(settings_.ui_language == "it" ? "Impossibile eliminare il download." : "Could not delete the download.");
                    }
                    downloads_refresh();
                });
        }
    }
    dirty_all();
}

void App::play_download(const std::string& id, bool progressive) {
    const auto entry = downloads_.find(id);
    if (entry && entry->state == DownloadState::Complete) progressive = false;
    double resume = 0;
    if (entry && !progressive) {
        const auto saved = progress_.find(entry->media_id);
        if (saved != progress_.end() && saved->second.video_id == entry->video_id &&
            std::isfinite(saved->second.time) && saved->second.time > 5 &&
            (saved->second.duration <= 0 || saved->second.time < saved->second.duration))
            resume = saved->second.time;
    }
    if (resume > 0 && entry->state == DownloadState::Complete) {
        const bool it = settings_.ui_language == "it";
        const auto generation = account_generation_;
        open_dropdown(entry->title,
            {it ? "Riproduci dall'inizio" : "Play from beginning",
             (it ? "Riproduci da " : "Resume from ") + format_time(resume)}, 1,
            [this, id, resume, generation](int choice) {
                if (generation != account_generation_ || choice < 0 || choice > 1) return;
                start_download_playback(id, false, choice == 0 ? 0 : resume);
            });
        return;
    }
    // A byte percentage does not establish a resumable timestamp in a VBR
    // container. Partial files start at the beginning; later seeks use the
    // player's verified media boundary once their timestamps are known.
    start_download_playback(id, progressive, 0);
}

void App::start_download_playback(const std::string& id, bool progressive, double start) {
    const auto entry = downloads_.find(id);
    if (entry && entry->state == DownloadState::Complete) progressive = false;
    std::shared_ptr<GrowingFilePlayback> growing;
    std::string error;
    if (entry && progressive) growing = downloads_.open_progressive(id, error);
    if (progressive && !growing) {
        dlog("download playback: not ready (%s)", error.c_str());
        show_toast(settings_.ui_language == "it" ?
            "Puoi iniziare a guardare dal 5% di un download in corso." :
            "You can start watching from 5% of an active download.", 4);
        downloads_refresh();
        return;
    }
    if (!entry || (!progressive && (entry->state != DownloadState::Complete || entry->local_path.empty() || !file_exists(entry->local_path)))) {
        show_toast(settings_.ui_language == "it" ? "Il file scaricato non è disponibile." : "The downloaded file is unavailable.");
        downloads_refresh();
        return;
    }
    if (watching_) watch_stop(false);
    w_offline_ = true; w_download_id_ = id;
    w_growing_download_ = std::move(growing);
    w_download_poster_ = entry->poster_path;
    w_download_background_ = entry->background_path;
    w_download_logo_ = entry->logo_path;
    w_item_ = {};
    w_item_.id = entry->media_id; w_item_.type = entry->type; w_item_.name = entry->title;
    w_video_id_ = entry->video_id;
    const auto path = w_growing_download_ ? w_growing_download_->path : entry->local_path;
    w_stream_ = {}; w_stream_.kind = StreamKind::Direct; w_stream_.url = path;
    w_stream_.filename = entry->title;
    w_transcode_ = false;
    t_visible = false;
    autoplay_pending_ = false;
    launch_title = entry->title; launch_image = entry->background_path; launch_logo = entry->logo_path;
    watch_start(path, {}, start, entry->title, entry->subtitle, false);
}
