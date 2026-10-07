// Real queue and artwork workers, with the video transport held at a boundary.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "download_manager.h"
#include "download_transfer.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

void dlog(const char*, ...) {}
namespace {
std::atomic<int> transfers{0};
std::atomic<unsigned> artwork_copies{0};
std::atomic<bool> hold_art{false}, art_blocked{false};
int checks = 0;
void require(bool okay, const char* message) {
    ++checks;
    if (!okay) throw std::runtime_error(message);
}
template<class Predicate> void until(Predicate predicate, const char* message) {
    const auto end = std::chrono::steady_clock::now() + 5s;
    do {
        if (predicate()) return;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < end);
    throw std::runtime_error(message);
}
std::string bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
void artwork(const fs::path& path, char color) {
    std::ofstream file(path, std::ios::binary);
    const uint32_t size[2] = {182, 277};
    const std::string pixels(182 * 277 * 4, color);
    file.write("RGBA", 4); file.write(reinterpret_cast<const char*>(size), sizeof(size));
    file.write(pixels.data(), pixels.size());
    require(file.good(), "fixture poster is written");
}
DownloadRequest request(const std::string& name) {
    DownloadRequest result;
    result.media_id = result.video_id = "fixture:" + name;
    result.type = "movie"; result.title = name;
    result.stream.kind = StreamKind::Direct;
    result.stream.url = "https://media.invalid/" + name + ".mp4";
    result.poster_url = "https://art.invalid/" + name + ".jpg";
    return result;
}
bool persisted(DownloadManager& manager, const std::string& id) {
    const auto row = manager.find(id);
    return row && fs::path(row->poster_path).filename() == "poster.rgba" && fs::exists(row->poster_path);
}
}

// Hold the actual file copy between selection and publication to make the
// concurrent remove edge deterministic. All actual writes still use POSIX.
extern "C" ssize_t __real_write(int fd, const void* data, size_t size);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size) {
	if (size == 12 && std::memcmp(data, "RGBA", 4) == 0) ++artwork_copies;
    if (hold_art.load() && size == 12 && std::memcmp(data, "RGBA", 4) == 0) {
        art_blocked = true;
        while (hold_art.load()) std::this_thread::sleep_for(2ms);
    }
    return __real_write(fd, data, size);
}

DownloadTransferResult download_transfer(const DownloadTransferRequest&,
    const DownloadProgressCallback&, const std::atomic<bool>& cancel) {
    ++transfers;
    while (!cancel.load()) std::this_thread::sleep_for(2ms);
    return {DownloadTransferStatus::Cancelled, {}, ".mp4", 0, -1};
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        const fs::path root = fs::absolute(argv[1]);
        fs::create_directories(root);
        const auto cache = root / "cached.rgba";
        const auto alternate = root / "alternate.rgba";
        artwork(cache, 'A'); artwork(alternate, 'B');
        const auto expected = bytes(cache);
        DownloadManager manager; std::string error;
        struct ReleaseArtworkGate { ~ReleaseArtworkGate() { hold_art = false; } } release_gate;
        require(manager.init(root.string(), &error), "queue initializes");
        auto initial = request("active"); initial.poster_path = cache.string();
        const auto active = manager.enqueue(initial, error);
        require(!active.empty(), "active fixture enters queue");
        manager.set_enabled(true);
        until([&] { return transfers.load() == 1; }, "first video transport starts");
        const auto copies_before = artwork_copies.load();

        auto queued = request("queued"); queued.poster_path = cache.string();
        const auto id = manager.enqueue(queued, error);
        require(!id.empty() && !manager.find(id)->poster_path.empty(), "queued cover is exposed immediately from cache");
        require(manager.find(id)->poster_path == cache.string() && !persisted(manager, id),
                "queued cover uses the existing cache without writing another image during an active transfer");
        require(manager.find(id)->state == DownloadState::Queued && transfers == 1,
                "adding the queued cover does not start its video or stop the active transfer");
        require(bytes(manager.find(id)->poster_path) == expected, "queued cover matches all original pixels");

        auto late = request("late");
        const auto late_id = manager.enqueue(late, error);
        require(!late_id.empty() && manager.find(late_id)->poster_path.empty(), "uncached source retains its pending state");
        require(manager.find(late_id)->poster_url == late.poster_url, "uncached poster URL remains available to the artwork fetcher");
        require(manager.update_poster(late_id, cache.string()), "asynchronous artwork completion is accepted for the queued job");
        until([&] { return manager.find(late_id)->poster_path == cache.string(); }, "late cover is published from its existing cache");
        require(transfers == 1 && manager.find(late_id)->state == DownloadState::Queued,
                "late artwork also avoids the blocked video worker");
        const auto preserved = manager.find(late_id)->poster_path;
        require(manager.update_poster(late_id, (root / "missing.rgba").string()) &&
                manager.update_poster_source(late_id, "https://art.invalid/processed.jpg"),
                "callback queues image validation without touching the filesystem on the UI thread");
        until([&] { return manager.find(late_id)->poster_url == "https://art.invalid/processed.jpg"; }, "worker validates the coalesced artwork update");
        require(manager.find(late_id)->poster_path == preserved, "missing callback file cannot replace a valid cover");
        require(artwork_copies.load() == copies_before && !persisted(manager, id) && !persisted(manager, late_id),
                "waiting items and late cover callbacks perform no competing image copies while media is active");

        manager.set_enabled(false);
        until([&] { return persisted(manager, id) && persisted(manager, late_id); },
              "deferred covers are made private after the active media writer stops");
        require(transfers == 1 && manager.find(id)->state == DownloadState::Queued,
                "processing deferred images while disabled cannot start queued video transfers");
        require(bytes(manager.find(id)->poster_path) == expected, "deferred offline cover preserves every pixel");
        struct stat st{};
        require(::stat(manager.find(id)->poster_path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
                "persistent cover keeps private file permissions");
        const auto private_cover = manager.find(late_id)->poster_path;
        require(manager.update_poster(late_id, alternate.string()), "shared cover callback remains accepted after persistence");
        require(manager.update_poster_source(late_id, "https://art.invalid/persisted.jpg"), "private cover metadata may be refreshed");
        until([&] { return manager.find(late_id)->poster_url == "https://art.invalid/persisted.jpg"; },
              "private cover refresh is applied on the artwork worker");
        require(manager.find(late_id)->poster_path == private_cover, "later callbacks preserve an already private cover");

        const auto pending_id = manager.enqueue(request("retry_after_restart"), error);
        require(!pending_id.empty(), "uncached restart fixture is queued");
        manager.shutdown(); fs::remove(cache);
        require(manager.init(root.string(), &error), "queue reopens after artwork cache eviction");
        require(persisted(manager, id) && bytes(manager.find(id)->poster_path) == expected,
                "queued cover survives restart without its temporary cache file");
        require(persisted(manager, late_id), "late cover survives the same restart");
        require(manager.find(pending_id)->poster_url == request("retry_after_restart").poster_url,
                "not-yet-fetched artwork address survives restart for a retry");
        require(manager.update_poster_source(pending_id, "https://art.invalid/recovered.jpg"),
                "legacy queued metadata accepts a recovered poster source");
        until([&] { return manager.find(pending_id)->poster_url == "https://art.invalid/recovered.jpg"; },
              "legacy queued metadata publishes its restored poster source on the artwork worker");

        hold_art = true; art_blocked = false;
        auto removal = request("remove_during_copy"); removal.poster_path = alternate.string();
        const auto removal_id = manager.enqueue(removal, error);
        until([&] { return art_blocked.load(); }, "artwork copy reaches the controlled write boundary");
        require(manager.remove(removal_id, error) && !manager.find(removal_id), "remove hides the selected entry during an active copy");
        require(!manager.update_poster(removal_id, alternate.string()), "late callback cannot resurrect the removed entry");
        hold_art = false;
        until([&] { return !fs::exists(root / "downloads" / removal_id); }, "remove waits for the copy then deletes all files safely");
        require(!manager.find(removal_id), "copy completion never republishes the deleted entry");
        manager.shutdown();
        require(!manager.update_poster(id, alternate.string()), "callbacks after shutdown cannot recreate workers or entries");
        std::cout << "Download artwork: " << checks << " checks passed; queued copies wait for the media writer to become idle\n";
        return 0;
    } catch (const std::exception& error) {
        hold_art = false;
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
