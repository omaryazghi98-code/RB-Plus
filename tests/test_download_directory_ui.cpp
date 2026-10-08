// Controller, modal ownership and async destination selection regressions.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app.h"
#include "download_directory.h"
#include "home_ui.h"
#include "tasks.h"
#include "core/input.hpp"
#include "ui/feedback.hpp"
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
int checks = 0;
std::atomic<int> unexpected_http{0};
void expect(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void wait_for(const std::function<bool()>& ready, bool drain = true) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        if (drain) g_tasks.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    expect(ready(), "worker completes within the bounded test deadline");
}
void worker_fence() {
    auto done = std::make_shared<std::atomic<bool>>(false);
    g_tasks.run([done]() { done->store(true); });
    wait_for([&]() { return done->load(); }, false);
}
int setting(const App& app, const char* id) {
    const auto found = std::find_if(app.s_rows.begin(), app.s_rows.end(),
        [&](const auto& row) { return row.id == id; });
    if (found == app.s_rows.end()) throw std::runtime_error(std::string("Missing setting: ") + id);
    return static_cast<int>(found - app.s_rows.begin());
}
}

extern "C" CURLcode __wrap_curl_easy_perform(CURL*) { ++unexpected_http; return CURLE_COULDNT_CONNECT; }

struct AppProtocolTest {
    static void settings(App& app) {
        app.view = "settings"; app.zone = "content"; app.nav_sel = 4;
        app.ui_language = app.settings_.ui_language = "en";
        app.settings_refresh();
        app.s_sel = setting(app, "download_directory");
    }
    static void browse(App& app, const std::string& path) { app.browse_download_directory(path); }
    static std::string queued_download(App& app) {
        DownloadRequest request;
        request.media_id = request.video_id = "tt1234567";
        request.type = "movie"; request.title = "Folder move fixture";
        request.stream.kind = StreamKind::Direct;
        request.stream.url = "https://example.invalid/fixture.mp4";
        std::string error;
        const auto id = app.downloads_.enqueue(request, error);
        expect(!id.empty() && error.empty(), "offline fixture creates one tracked queued download");
        return id;
    }
};

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    char temporary[] = "/tmp/stremio-directory-ui-XXXXXX";
    const char* made = ::mkdtemp(temporary);
    if (!made) return 2;
    const fs::path root(made);
    App app;
    bool initialized = false;
    g_tasks.start(1);
    try {
        fs::create_directory(root / "profile");
        fs::create_directory(root / "Movies");
        fs::create_directory(root / "Drive");
        for (int i = 0; i < 12; ++i) fs::create_directory(root / "Movies" / ("Folder" + std::to_string(i)));
        initialized = app.init(argv[1], (root / "profile").string(), true);
        expect(initialized && app.settings().download_directory.empty(), "fresh installation has no implicit destination");
        HomeUi ui;
        hui::ui::Feedback feedback;
        using A = hui::Action;
        using D = hui::Direction;
        auto send = [&](A action = A::count, D direction = D::none) {
            hui::InputFrame input;
            input.connected = true;
            input.nav = direction;
            if (action != A::count) input.pressed = hui::action_bit(action);
            ui.update(app, .016f); ui.handle(app, input, feedback); ui.update(app, .016f);
        };
        auto settle = [&]() { wait_for([&]() { return !app.directory_picker_loading && !app.directory_picker_committing; }); };

        app.view = "detail"; app.d_zone = "streams"; app.download_directory_notice = true;
        send(A::north); send(A::page_next); send(A::west);
        expect(app.download_directory_notice && app.view == "detail" && !app.directory_picker_visible && !app.input_visible_,
               "download notice blocks Search, tabs and background downloads");
        send(A::confirm);
        expect(!app.download_directory_notice && app.view == "detail" && !app.directory_picker_visible,
               "single OK acknowledges without navigating away");
        app.download_directory_notice = true; send(A::back);
        expect(!app.download_directory_notice && app.view == "detail", "Circle only closes the missing-folder notice");

        AppProtocolTest::settings(app);
        expect(app.s_rows[app.s_sel].value == "Not selected", "unset destination is explicit in Settings");
        send(A::confirm); settle();
        expect(app.directory_picker_visible && !app.input_visible_ && !app.dd_visible,
               "download setting opens the folder browser without a keyboard");
        AppProtocolTest::browse(app, (root / "Movies").string()); settle();
        expect(app.directory_picker_entries.size() == 13 && app.directory_picker_entries.front() == "..",
               "browser contains real directory rows and a parent row");
        send(A::page_next);
        expect(app.directory_picker_sel == 6 && app.view == "settings" && app.nav_sel == 4,
               "R1 pages directory focus without switching the main tab");
        send(A::page_prev);
        expect(app.directory_picker_sel == 0, "L1 pages back through directory rows");
        send(A::count, D::down);
        const auto opened = app.directory_picker_entries[1];
        send(A::confirm); settle();
        expect(app.directory_picker_path == (root / "Movies" / opened).string(), "Cross enters the focused real folder");
        send(A::count, D::left); settle();
        expect(app.directory_picker_path == (root / "Movies").string() && app.directory_picker_sel == 1,
               "Left returns to parent and restores the child focus");
        send(A::north);
        expect(app.directory_picker_committing, "Triangle schedules selection off the UI thread");
        worker_fence();
        send(A::back);
        expect(app.directory_picker_visible && app.directory_picker_committing,
               "Circle cannot hide a move or its pending completion");
        const auto pending = g_tasks.pending();
        send(A::confirm); send(A::north); send(A::page_next); send(A::menu);
        expect(g_tasks.pending() == pending && app.directory_picker_visible && app.nav_sel == 4,
               "moving picker consumes duplicate selection, refresh and page input");
        g_tasks.drain();
        expect(app.settings().download_directory == (root / "Movies").string(),
               "successful move completion updates the selected folder");
        expect(!app.directory_picker_visible && !app.download_relocation_active(),
               "successful move completion closes the picker and releases input");
        expect(!fs::exists(root / "Movies" / "downloads"), "selection uses the exact folder without appending downloads");
        const auto moved_id = AppProtocolTest::queued_download(app);
        expect(fs::is_regular_file(root / "Movies" / moved_id / "manifest.json"),
               "tracked download starts in the originally selected folder");

        send(A::confirm); settle();
        AppProtocolTest::browse(app, (root / "Drive").string()); settle();
        send(A::west); settle();
        expect(app.directory_picker_path == (root / "Drive" / download_directory::folder_name).string(),
               "Square creates and enters Stremio Plus Downloads");
        expect(app.settings().download_directory == (root / "Movies").string(),
               "creating a child does not silently choose it as the destination");
        std::ofstream(root / "Drive" / download_directory::folder_name / "keep.part") << "existing download";
        send(A::north); settle();
        expect(!app.directory_picker_visible && app.settings().download_directory ==
               (root / "Drive" / download_directory::folder_name).string(), "separate Triangle confirms the newly created child");
        const auto chosen = app.settings().download_directory;
        expect(!fs::exists(root / "Movies" / moved_id) &&
               fs::is_regular_file(fs::path(chosen) / moved_id / "manifest.json"),
               "Triangle moves the existing tracked download and removes its old directory");
        expect(std::any_of(app.download_rows.begin(), app.download_rows.end(), [&](const auto& row) {
                   return row.id == moved_id && row.title == "Folder move fixture";
               }), "moved download remains visible in the refreshed Downloads inventory");
        send(A::confirm); settle();
        send(A::north); settle();
        expect(app.settings().download_directory == chosen && !app.download_relocation_active(),
               "reselecting the same folder completes without scheduling duplicate work");

        send(A::confirm); settle();
        fs::create_directory(root / "Disconnected");
        AppProtocolTest::browse(app, (root / "Disconnected").string()); settle();
        fs::remove(root / "Disconnected");
        send(A::north); settle();
        expect(app.directory_picker_visible && app.directory_picker_error && !app.directory_picker_status.empty(),
               "removed drive reports a selection error within the picker");
        expect(app.settings().download_directory == chosen, "failed selection preserves the previous destination");
        send(A::count, D::left); settle();
        fs::create_directory(root / "Refreshed");
        send(A::menu); settle();
        expect(std::find(app.directory_picker_entries.begin(), app.directory_picker_entries.end(), "Refreshed") !=
               app.directory_picker_entries.end(), "Options refreshes newly available directories");

        AppProtocolTest::browse(app, (root / "Movies").string());
        worker_fence();
        send(A::back);
        const auto before = app.directory_picker_path;
        g_tasks.drain();
        expect(!app.directory_picker_visible && app.directory_picker_path == before,
               "late directory scan cannot reopen or overwrite a closed picker");
        expect(fs::file_size(root / "Drive" / download_directory::folder_name / "keep.part") == 17,
               "changing and browsing destinations preserves existing media");

        app.s_sel = setting(app, "next_episode_delay_seconds");
        send(A::confirm);
        expect(app.dd_visible && app.dd_options.size() == 6, "next episode countdown offers six timing choices");
        app.dd_sel = 5; send(A::confirm);
        expect(app.settings().next_episode_delay_seconds == 120, "countdown choice updates the persistent settings model");
        expect(unexpected_http == 0, "folder navigation and settings never contact an online service");
        app.shutdown(); initialized = false;
        g_tasks.stop(); g_tasks.drain();
        fs::remove_all(root);
        std::cout << checks << " download directory UI checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        if (initialized) app.shutdown();
        g_tasks.stop(); g_tasks.drain();
        fs::remove_all(root);
        return 1;
    }
}
