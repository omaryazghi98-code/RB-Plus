#include "download_manager.h"
#include "download_transfer.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

void dlog(const char*, ...) {}

namespace {
struct Fake {
	int starts = 0, progress_calls = 0;
	bool release = false, release_cancel = true, cancel_seen = false, returned = false;
	bool failure = false, malformed = false, throws = false;
	bool hold_progress = false, progress_release = false;
	bool periodic_progress = false;
	int64_t offset = 0, progress_done = 4096;
	int64_t cancelled_prefix = -1, cancelled_file_size = -1;
	DownloadTransferRequest last;
};
std::mutex fake_mutex;
std::condition_variable fake_changed;
std::map<std::string, std::shared_ptr<Fake>> fakes;
int checks = 0, failures = 0;
std::atomic<int> fail_lstat{0}, fail_opendir{0}, fail_chmod{0}, fail_fchmod{0}, fail_write{0}, fail_sync{0};
std::string denied_path;
std::atomic<bool> hold_manifest_sync{false}, manifest_sync_entered{false}, release_manifest_sync{true};
std::atomic<unsigned> manifest_sync_count{0};
thread_local bool ui_polling = false;
std::atomic<unsigned> ui_storage_calls{0};

void check(bool ok, const char* explanation) {
	++checks;
	if (!ok) { ++failures; std::cerr << "FAIL: " << explanation << '\n'; }
}

template<class Test> bool until(Test test, int milliseconds = 3000) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	do { if (test()) return true; std::this_thread::sleep_for(5ms); } while (std::chrono::steady_clock::now() < deadline);
	return test();
}

std::shared_ptr<Fake> plan(const std::string& name, bool released = false) {
	std::lock_guard lock(fake_mutex);
	auto fake = std::make_shared<Fake>(); fake->release = released; fakes[name] = fake; return fake;
}

template<class Action> void change(const std::shared_ptr<Fake>& fake, Action action) {
	std::lock_guard lock(fake_mutex); action(*fake); fake_changed.notify_all();
}

template<class Test> auto inspect(const std::shared_ptr<Fake>& fake, Test test) {
	std::lock_guard lock(fake_mutex); return test(*fake);
}

DownloadRequest request(const std::string& name, bool torrent = false) {
	DownloadRequest result;
	result.media_id = "tt1234567"; result.type = "movie"; result.video_id = "tt1234567";
	result.title = "Offline film"; result.subtitle = "Provider · 1080p";
	result.stream.filename = name; result.stream.name = "Selected source";
	result.stream.kind = torrent ? StreamKind::Torrent : StreamKind::Direct;
	if (torrent) result.stream.info_hash = std::string(40, name.empty() ? 'a' : 'b');
	else result.stream.url = "https://provider.invalid/private-token/" + name + ".mp4";
	result.stream.request_headers = {"Authorization: Bearer PRIVATE-DOWNLOAD-TOKEN"};
	return result;
}

std::string bytes(const fs::path& path) {
	std::ifstream input(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(input), {});
}

void write(const fs::path& path, const std::string& contents) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc); output.write(contents.data(), contents.size());
}

json manifest(const fs::path& path) { return json::parse(bytes(path)); }

void artwork(const fs::path& path) {
	std::ofstream output(path, std::ios::binary);
	const uint32_t width = 2, height = 2;
	output.write("RGBA", 4); output.write(reinterpret_cast<const char*>(&width), 4);
	output.write(reinterpret_cast<const char*>(&height), 4); output.write("0123456789abcdef", 16);
}

bool state(DownloadManager& manager, const std::string& id, DownloadState expected) {
	const auto value = manager.find(id); return value && value->state == expected;
}

void storage_permissions_and_retry(const fs::path& data) {
	fs::create_directories(data);
	DownloadManager manager; std::string error;
	const auto selected = plan("permission-retry", true);
	struct stat st{};
	check(::stat(data.c_str(), &st) == 0 && S_ISDIR(st.st_mode), "pre-grant fixture permits stat like the native loader");
	denied_path = data.string(); fail_lstat = EPERM;
	check(!manager.init(data.string(), &error) && !manager.storage_available(), "pre-grant lstat EPERM cannot pass the storage safety check");
	check(error.find("lstat") != std::string::npos && error.find(data.string()) != std::string::npos &&
		error.find("errno " + std::to_string(EPERM)) != std::string::npos,
		"startup diagnostics identify lstat, the denied local path and EPERM");
	check(manager.storage_error() == error, "startup storage failure remains available to application diagnostics");
	manager.set_enabled(true);
	check(manager.enqueue(request("permission-retry"), error).empty() &&
		error == "Download storage is not available." && manager.snapshot().empty(),
		"retry while filesystem permission is denied keeps a localized UI error and creates no job");
	check(inspect(selected, [](const Fake& f) { return f.starts == 0; }), "failed storage initialization never starts a network transfer");
	fail_lstat = 0;
	const auto id = manager.enqueue(request("permission-retry"), error);
	check(!id.empty() && error.empty() && manager.storage_available() && manager.storage_error().empty(),
		"next explicit download recovers after the filesystem grant without restarting the app");
	check(until([&] { return state(manager, id, DownloadState::Complete); }), "storage recovery preserves the enabled state and completes the selected download");
	check(manager.enqueue(request("permission-retry"), error) == id && manager.snapshot().size() == 1 &&
		inspect(selected, [](const Fake& f) { return f.starts == 1; }),
		"recovery starts one worker and repeated enqueue cannot duplicate the selected transfer");
	check(!fs::exists(data / "downloads" / ".storage-check") && !fs::exists(data / "downloads" / ".storage-check.tmp"),
		"successful storage verification leaves no probe files");
	manager.shutdown();
	check(manager.enqueue(request("permission-retry"), error).empty() && !manager.storage_available(),
		"explicit shutdown cannot be undone by a later enqueue");

	denied_path = (data / "downloads").string(); fail_chmod = EPERM;
	check(!manager.init(data.string(), &error) && error.find("chmod") != std::string::npos &&
		error.find(denied_path) != std::string::npos, "private directory chmod denial is diagnosed without weakening permissions");
	fail_chmod = 0; fail_fchmod = EPERM;
	check(!manager.init(data.string(), &error) && error.find("fchmod") != std::string::npos,
		"write probe detects file permission denial before reporting storage ready");
	fail_fchmod = 0; fail_write = ENOSPC;
	check(!manager.init(data.string(), &error) && error.find("write") != std::string::npos &&
		error.find("errno " + std::to_string(ENOSPC)) != std::string::npos,
		"write probe detects a full disk even when mkdir and stat succeed");
	fail_write = 0; fail_sync = EIO;
	check(!manager.init(data.string(), &error) && error.find("fsync") != std::string::npos,
		"write probe requires durable storage instead of accepting a failed fsync");
	fail_sync = 0;
	check(!fs::exists(data / "downloads" / ".storage-check.tmp"), "failed storage probes clean up their temporary file");
	check(manager.init(data.string(), &error) && error.empty() && state(manager, id, DownloadState::Complete),
		"storage reinitialization preserves an existing completed offline video");
	manager.shutdown();
	fail_opendir = EPERM;
	const bool denied = !manager.init(data.string(), &error) && !manager.storage_available() &&
		error.find("opendir") != std::string::npos && error.find(denied_path) != std::string::npos &&
		error.find("errno " + std::to_string(EPERM)) != std::string::npos;
	fail_opendir = 0;
	const auto recovered_id = manager.enqueue(request("permission-retry"), error);
	check(denied && recovered_id == id && error.empty() && manager.storage_available() &&
		manager.snapshot().size() == 1 && state(manager, id, DownloadState::Complete),
		"unlistable inventory rejects readiness with precise errno and retry preserves the completed entry");
	manager.shutdown();
}

void basic_queue(const fs::path& data) {
	fs::create_directories(data);
	const auto image = data / "cached.rgba"; artwork(image);
	auto selected = plan("one"), alternative = plan("two");
	DownloadManager manager;
	std::string error;
	check(manager.init(data.string(), &error) && error.empty(), "initialization creates private download storage");
	auto first = request("one"); first.poster_path = first.background_path = first.logo_path = image.string();
	const auto revision = manager.revision();
	const auto id = manager.enqueue(first, error);
	check(!id.empty() && error.empty() && manager.snapshot().size() == 1, "only the explicitly selected stream is queued");
	check(manager.revision() > revision, "inventory revisions notify the UI of a queued stream");
	check(until([&] { auto e = manager.find(id); return e && !e->logo_path.empty(); }), "cached poster, hero and logo are copied on the worker");
	check(inspect(selected, [](const Fake& f) { return f.starts == 0; }), "init and enqueue do not start networking while disabled");
	check(manager.enqueue(first, error) == id && manager.snapshot().size() == 1, "same selected stream is deduplicated");
	const auto second_id = manager.enqueue(request("two"), error);
	check(!second_id.empty() && second_id != id && manager.snapshot().size() == 2, "different stream quality/source remains a separate download");
	manager.set_enabled(true);
	check(until([&] { return inspect(selected, [](const Fake& f) { return f.starts == 1; }); }), "enabled worker starts the first selected transfer");
	check(inspect(alternative, [](const Fake& f) { return f.starts == 0; }), "download queue has one active transfer");
	check(manager.pause(id), "active transfer can be paused");
	check(until([&] { return state(manager, id, DownloadState::Paused) && inspect(selected, [](const Fake& f) { return f.returned; }); }), "pause propagates cancellation to the transport");
	check(manager.find(id)->local_path.empty(), "paused partial media is never exposed as an offline video");
	check(manager.find(id)->done == 4096, "pause retains actual partial byte progress");
	check(manager.remove(second_id, error), "another queued or active transfer can be removed");
	check(until([&] { return !fs::exists(data / "downloads" / second_id); }), "removal waits for transport cancellation then deletes its files");
	check(manager.resume(id), "paused selected stream can resume");
	check(until([&] { return inspect(selected, [](const Fake& f) { return f.starts == 2; }); }), "resume invokes the real transfer boundary again");
	check(inspect(selected, [](const Fake& f) { return f.offset == 4096; }), "safe partial file remains available for transport resume");
	change(selected, [](Fake& f) { f.release = true; });
	check(until([&] { return state(manager, id, DownloadState::Complete); }), "successful transfer is published as complete");
	const auto completed = *manager.find(id);
	check(completed.done == 8192 && completed.total == 8192 && completed.progress == 1, "complete inventory records exact final bytes");
	check(fs::is_regular_file(completed.local_path) && !fs::exists(data / "downloads" / id / "media.part"), "only atomic part promotion creates the offline media path");
	check(bytes(completed.poster_path) == bytes(image) && bytes(completed.background_path) == bytes(image), "offline artwork matches the cached RGBA bytes");
	struct stat st{};
	::stat((data / "downloads" / id / "manifest.json").c_str(), &st);
	check((st.st_mode & 0777) == 0600, "provider credentials are persisted in a private 0600 manifest");
	::stat((data / "downloads").c_str(), &st);
	check((st.st_mode & 0777) == 0700, "download directory is private 0700");
	check(bytes(data / "downloads" / id / "manifest.json").find("PRIVATE-DOWNLOAD-TOKEN") != std::string::npos, "exact selected stream headers survive persistence");
	fs::remove(image);
	manager.shutdown();
	check(manager.init(data.string()), "inventory can restart without a server or account request");
	check(state(manager, id, DownloadState::Complete), "completed local file survives restart");
	check(manager.find(id)->local_path == completed.local_path && fs::exists(manager.find(id)->logo_path), "restart retains offline playback and artwork independently of art cache");
	manager.shutdown();
}

void crash_and_validation(const fs::path& data) {
	fs::create_directories(data);
	auto running = plan("crash");
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "crash fixture storage initializes");
	const auto id = manager.enqueue(request("crash"), error);
	manager.set_enabled(true);
	check(until([&] { return inspect(running, [](const Fake& f) { return f.starts == 1; }); }), "crash fixture reaches active transfer");
	manager.shutdown();
	const auto path = data / "downloads" / id;
	auto value = manifest(path / "manifest.json");
	value["state"] = static_cast<int>(DownloadState::Downloading); value["done"] = 8192; value["total"] = 8192;
	write(path / "media.part", std::string(8192, 'x')); write(path / "manifest.json", value.dump());
	check(manager.init(data.string()), "interrupted manifest is accepted on restart");
	check(state(manager, id, DownloadState::Paused) && manager.find(id)->local_path.empty(), "even a full-size .part after crash is restored Paused, never Complete");
	check(inspect(running, [](const Fake& f) { return f.starts == 1; }), "restart never silently starts downloads");
	manager.shutdown();
	value["state"] = static_cast<int>(DownloadState::Complete); value["media_name"] = "media.part";
	value["removing"] = "wrong-type"; value["artwork_pending"] = json::array();
	write(path / "manifest.json", value.dump());
	check(manager.init(data.string()) && state(manager, id, DownloadState::Failed), "malformed booleans cannot crash load and .part cannot masquerade as a complete file");
	manager.shutdown();
	const auto outside = data / "outside.mp4"; write(outside, std::string(8192, 'o'));
	value["media_name"] = "../../outside.mp4"; write(path / "manifest.json", value.dump());
	check(manager.init(data.string()) && manager.find(id)->local_path.empty(), "manifest cannot publish media outside its own job directory");
	check(!manager.remove("../../outside.mp4", error) && bytes(outside).size() == 8192, "delete rejects path traversal identifiers");
	manager.shutdown();
	const auto moved = data / "saved-job"; fs::rename(path, moved); fs::create_directory_symlink(moved, path);
	check(manager.init(data.string()) && manager.snapshot().empty(), "inventory refuses symlinked job directories");
	manager.shutdown(); fs::remove(path); fs::rename(moved, path);
	value["media_name"] = "media.mp4"; value["state"] = static_cast<int>(DownloadState::Complete);
	fs::create_symlink(outside, path / "media.mp4"); write(path / "manifest.json", value.dump());
	check(manager.init(data.string()) && state(manager, id, DownloadState::Failed), "completed media symlink is rejected");
	check(manager.remove(id, error), "removal may unlink an owned symlink without following it");
	check(until([&] { return !fs::exists(path); }) && bytes(outside).size() == 8192, "symlink target outside download storage is preserved");
	manager.shutdown();
}

void progress_persistence(const fs::path& data) {
	fs::create_directories(data);
	const auto running = plan("progress-persistence");
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "progress persistence fixture initializes");
	const auto selected = request("progress-persistence", true);
	const auto id = manager.enqueue(selected, error);
	const auto job = data / "downloads" / id;
	check(until([&] { return !manifest(job / "manifest.json").value("artwork_pending", true); }),
		"initial artwork state settles before counting progress writes");
	manager.set_enabled(true);
	check(until([&] { return inspect(running, [](const Fake& f) { return f.progress_calls > 0; }); }),
		"first transfer progress finishes metadata persistence");
	const auto first_manifest = bytes(job / "manifest.json");
	const auto initial = json::parse(first_manifest);
	check(initial.at("total") == 8192 && initial.at("done") == 4096 &&
		initial.at("state") == static_cast<int>(DownloadState::Downloading),
		"resolved total is persisted as soon as it becomes known");
	check(initial.at("stream").at("info_hash") == selected.stream.info_hash &&
		initial.at("stream").at("request_headers") == selected.stream.request_headers,
		"metadata persistence retains the exact selected source identity and headers");
	const auto saves = manifest_sync_count.load();
	const auto revision = manager.revision();
	change(running, [](Fake& f) { f.periodic_progress = true; f.progress_done = 6144; });
	const auto started = std::chrono::steady_clock::now();
	check(until([&] {
		return std::chrono::steady_clock::now() - started >= 5500ms &&
			inspect(running, [](const Fake& f) { return f.progress_calls >= 21; });
	}, 6500), "progress continues across the former five-second persistence interval");
	change(running, [](Fake& f) { f.periodic_progress = false; });
	const auto updated = manager.find(id);
	check(updated && updated->done == 6144 && updated->total == 8192 && updated->progress == 0.75 &&
		manager.revision() > revision, "progress-only changes still publish the current UI inventory");
	check(manifest_sync_count.load() == saves && bytes(job / "manifest.json") == first_manifest,
		"periodic progress performs no additional manifest fsync or rewrite");
	std::cout << "Progress persistence: " << inspect(running, [](const Fake& f) { return f.progress_calls; })
		<< " updates across 5.5 seconds, " << manifest_sync_count.load() - saves << " extra manifest flushes\n";

	const auto recovery = data / "interrupted";
	const auto saved_job = recovery / "downloads" / id;
	fs::create_directories(saved_job);
	write(saved_job / "manifest.json", first_manifest);
	write(saved_job / "media.part", std::string(6144, 'a'));
	write(saved_job / "transfer.json", json{{"version", 1}, {"kind", "http"}, {"source", "different-source"},
		{"bytes", 7000000}, {"total", 9000000}}.dump());
	DownloadManager restored;
	check(restored.init(recovery.string()), "an interrupted inventory snapshot restores without a live transfer");
	const auto recovered = restored.find(id);
	check(recovered && recovered->state == DownloadState::Paused && recovered->done == 6144 &&
		recovered->total == 8192 && recovered->progress == 0.75 && recovered->local_path.empty(),
		"restart recovers current partial bytes and the persisted total without trusting foreign checkpoint metadata");
	restored.shutdown();
	check(manifest(saved_job / "manifest.json").at("state") == static_cast<int>(DownloadState::Paused),
		"restored state remains durable on shutdown");

	const auto before_pause = manifest_sync_count.load();
	check(manager.pause(id) && until([&] { return inspect(running, [](const Fake& f) { return f.returned; }); }),
		"pause stops the transfer after progress-only updates");
	manager.set_enabled(false);
	const auto paused = manifest(job / "manifest.json");
	check(manifest_sync_count.load() > before_pause && paused.at("state") == static_cast<int>(DownloadState::Paused) &&
		paused.at("done") == 6144 && paused.at("total") == 8192,
		"pause still flushes the latest state and bytes to the manifest");
	const auto before_resume = manifest_sync_count.load();
	check(manager.resume(id) && manifest_sync_count.load() > before_resume &&
		manifest(job / "manifest.json").at("state") == static_cast<int>(DownloadState::Queued),
		"resume durably queues the same selected source before starting it");
	change(running, [](Fake& f) { f.release = true; });
	manager.set_enabled(true);
	check(until([&] { return state(manager, id, DownloadState::Complete); }),
		"a transfer with progress-only snapshots can still complete normally");
	const auto complete = manifest(job / "manifest.json");
	check(complete.at("state") == static_cast<int>(DownloadState::Complete) && complete.at("done") == 8192 &&
		complete.at("total") == 8192 && complete.at("media_name") == "media.mp4",
		"completion durably records the final file and exact byte count");
	manager.shutdown();
}

void progress_metadata_failure(const fs::path& data) {
	fs::create_directories(data);
	const auto running = plan("metadata-failure");
	change(running, [](Fake& f) { f.hold_progress = true; });
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "metadata failure fixture initializes");
	const auto id = manager.enqueue(request("metadata-failure"), error);
	const auto job = data / "downloads" / id;
	check(until([&] { return !manifest(job / "manifest.json").value("artwork_pending", true); }),
		"artwork persistence completes before injecting a metadata flush failure");
	manager.set_enabled(true);
	check(until([&] { return inspect(running, [](const Fake& f) { return f.starts == 1; }); }),
		"metadata failure fixture reaches the first progress callback");
	fail_sync = EIO;
	change(running, [](Fake& f) { f.progress_release = true; });
	const bool failed = until([&] { return state(manager, id, DownloadState::Failed); });
	fail_sync = 0;
	const auto entry = manager.find(id);
	check(failed && entry && entry->error == "The download metadata could not be saved." &&
		entry->local_path.empty() && inspect(running, [](const Fake& f) { return f.cancel_seen; }),
		"failure to persist a newly resolved total cancels the writer and reports a storage error");
	change(running, [](Fake& f) { f.release = true; });
	check(manager.resume(id) && until([&] { return state(manager, id, DownloadState::Complete); }),
		"metadata storage failure can be retried after durable writes recover");
	manager.shutdown();
}

void progressive_leases(const fs::path& data) {
	fs::create_directories(data);
	const auto running = plan("progressive-live");
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "progressive manager storage initializes");
	const auto id = manager.enqueue(request("progressive-live", true), error);
	check(!manager.open_progressive(id, error) && !error.empty(), "queued download cannot open a progressive lease");
	manager.set_enabled(true);
	check(until([&] { const auto e = manager.find(id); return e && e->playable_while_downloading; }),
		"real manager advertises progressive readiness only after transport reports its prefix");
	auto playback = manager.open_progressive(id, error);
	check(playback && error.empty() && playback->descriptor >= 0 && playback->total == 8192,
		"progressive lease opens the one selected part with its known total");
	if (!playback) { manager.shutdown(); return; }
	check(playback->torrent_hash == request("progressive-live", true).stream.info_hash && playback->torrent_file_idx == 4,
		"lease receives the resolved torrent file index, not an unresolved addon guess");
	check(state(manager, id, DownloadState::Downloading) && inspect(running, [](const Fake& f) { return f.starts == 1 && !f.cancel_seen; }),
		"opening progressive playback neither duplicates nor preempts the background torrent");
	check(manager.pause(id) && until([&] { return inspect(running, [](const Fake& f) { return f.returned; }); }),
		"pause waits for the existing transfer to release its writer");
	check(playback->state->snapshot().phase == GrowingFilePhase::Paused && !manager.open_progressive(id, error),
		"pause reaches active playback and prevents starting another partial reader");
	change(running, [](Fake& f) { f.hold_progress = true; f.progress_release = false; });
	const auto old_generation = playback->generation;
	check(manager.resume(id) && until([&] { return inspect(running, [](const Fake& f) { return f.starts == 2; }); }),
		"resuming starts reconciliation of the same saved job");
	check(!manager.find(id)->playable_while_downloading && !manager.open_progressive(id, error),
		"stale on-disk bytes cannot qualify before resumed transfer publishes reconciled progress");
	check(playback->state->snapshot().generation != old_generation,
		"restarted transfer invalidates the old decoder generation before possible truncation");
	change(running, [](Fake& f) { f.progress_release = true; });
	check(until([&] { const auto e = manager.find(id); return e && e->playable_while_downloading; }),
		"resumed download becomes playable again after fresh transport progress");
	auto resumed = manager.open_progressive(id, error);
	check(resumed && resumed->generation != old_generation && manager.snapshot().size() == 1,
		"reopened playback uses the new generation without a second download");
	change(running, [](Fake& f) { f.release = true; });
	check(until([&] { return state(manager, id, DownloadState::Complete); }), "download completes normally while a progressive descriptor is held");
	struct stat st{};
	check(resumed && ::fstat(resumed->descriptor, &st) == 0 && st.st_size == 8192 &&
		resumed->state->snapshot().phase == GrowingFilePhase::Complete && !fs::exists(resumed->path),
		"completion publishes exact final size and held inode survives part rename");
	check(manager.remove(id, error) && until([&] { return !fs::exists(data / "downloads" / id); }),
		"completed download with a held descriptor can be deleted safely");
	check(resumed && resumed->state->snapshot().phase == GrowingFilePhase::Removed,
		"deletion is published to the independent playback lease before job destruction");
	manager.shutdown();
}

void cancelled_unacknowledged_tail(const fs::path& data) {
	fs::create_directories(data);
	const auto running = plan("cancelled-tail");
	change(running, [](Fake& f) { f.cancelled_prefix = 4096; f.cancelled_file_size = 8192; });
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "cancelled tail fixture initializes");
	const auto id = manager.enqueue(request("cancelled-tail", true), error);
	manager.set_enabled(true);
	check(until([&] { const auto entry = manager.find(id); return entry && entry->playable_while_downloading; }),
		"cancelled tail fixture publishes its acknowledged prefix");
	const auto playback = manager.open_progressive(id, error);
	check(bool(playback), "cancelled tail fixture holds a progressive playback lease");
	check(manager.pause(id), "cancelled tail fixture requests cancellation");
	std::atomic<bool> stop{false};
	check(manager.wait_for_torrent_idle(stop), "cancelled writer releases ownership before the final prefix is inspected");
	const auto entry = manager.find(id);
	check(fs::file_size(data / "downloads" / id / "media.part") == 8192,
		"cancelled writer fixture retains the larger unacknowledged on-disk tail");
	check(entry && entry->state == DownloadState::Paused && entry->done == 4096 &&
		entry->total == 8192 && entry->progress == 0.5f && entry->local_path.empty(),
		"cancelled torrent inventory uses acknowledged bytes and never promotes the full-length tail");
	check(playback && playback->state->snapshot().available == 4096 &&
		playback->state->snapshot().phase == GrowingFilePhase::Paused,
		"progressive playback cannot read the unacknowledged cancelled tail");
	check(manifest(data / "downloads" / id / "manifest.json")["done"] == 4096,
		"cancelled torrent manifest persists the acknowledged prefix");
	manager.shutdown();
}

void snapshot_during_slow_storage(const fs::path& data) {
	fs::create_directories(data);
	const auto cover = data / "cached.rgba"; artwork(cover);
	const auto running = plan("slow-manifest");
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "slow storage fixture initializes");
	const auto id = manager.enqueue(request("slow-manifest", true), error);
	const auto job_path = data / "downloads" / id;
	check(until([&] { return !manifest(job_path / "manifest.json").value("artwork_pending", true); }),
		"initial artwork state is persisted before the slow-storage fixture starts");
	manager.set_enabled(true);
	check(until([&] { auto e = manager.find(id); return e && e->playable_while_downloading &&
		inspect(running, [](const Fake& f) { return f.progress_calls > 0; }); }),
		"slow storage fixture reaches an active prefix beyond five percent");
	change(running, [](Fake& f) { f.periodic_progress = true; f.progress_done = 6144; });
	const auto waiting = plan("queued-during-slow-commit");
	auto queued = request("queued-during-slow-commit"); queued.poster_path = cover.string();
	std::promise<std::string> committed;
	auto completion = committed.get_future();
	manifest_sync_entered = false; release_manifest_sync = false; hold_manifest_sync = true;
	ui_polling = true;
	const auto submitted_at = std::chrono::steady_clock::now();
	const bool submitted = manager.enqueue_async(queued, [&](std::string new_id, std::string error) {
		committed.set_value(error.empty() ? std::move(new_id) : std::string{});
	});
	ui_polling = false;
	check(submitted && std::chrono::steady_clock::now() - submitted_at < 100ms,
		"selected enqueue submission returns within 100 ms without waiting for its durable commit");
	const bool entered = until([] { return manifest_sync_entered.load(); });
	check(entered, "dedicated enqueue worker reaches the held manifest fsync");
	check(completion.wait_for(0ms) == std::future_status::timeout && manager.snapshot().size() == 1,
		"uncommitted request is neither acknowledged nor published as a queued download");
	const auto progress_before = inspect(running, [](const Fake& f) { return f.progress_calls; });
	const bool transfer_live = until([&] {
		return inspect(running, [&](const Fake& f) { return f.progress_calls >= progress_before + 3; });
	}, 1200);
	check(transfer_live && manager.find(id)->done == 6144,
		"active transfer publishes three further progress callbacks while another job's fsync is held");
	check(inspect(waiting, [](const Fake& f) { return f.starts == 0; }),
		"waiting job cannot start another network transfer during its commit");
	auto polling = std::async(std::launch::async, [&] {
		ui_polling = true;
		for (int n = 0; n < 100; ++n) {
			const auto list = manager.snapshot(); const auto selected = manager.find(id);
			if (list.size() != 1 || !selected || selected->id != id || selected->done != 6144 ||
				selected->remaining_seconds != 10 || selected->connected_peers != 3 || selected->connected_seeders != 2 ||
				!manager.update_poster(id, cover.string()) || !manager.update_poster_source(id, "https://art.invalid/refreshed.jpg"))
				return false;
		}
		return true;
	});
	const bool responsive = polling.wait_for(100ms) == std::future_status::ready;
	// Release even on regression so a failed deadline cannot hang this suite.
	release_manifest_sync = true; hold_manifest_sync = false;
	check(responsive && polling.get(), "100 UI inventory polls and poster callbacks finish within 100 ms while real worker fsync is blocked");
	check(ui_storage_calls == 0, "UI inventory and poster callbacks perform no file stat, write or fsync");
	check(completion.wait_for(3s) == std::future_status::ready,
		"enqueue result arrives after the durable write is released");
	const auto waiting_id = completion.get();
	check(!waiting_id.empty() && manager.find(waiting_id) && manager.find(waiting_id)->state == DownloadState::Queued,
		"successful completion identifies the one committed selected request");
	check(manifest(data / "downloads" / waiting_id / "manifest.json").at("stream").at("url") == queued.stream.url,
		"accepted queue entry preserves its exact selected stream on disk");
	check(manager.find(waiting_id)->poster_path == cover.string() &&
		!fs::exists(data / "downloads" / waiting_id / "poster.rgba"),
		"waiting cover remains visible without a competing RGBA copy or artwork fsync");
	const auto stable_saves = manifest_sync_count.load();
	const auto callbacks_before = inspect(running, [](const Fake& f) { return f.progress_calls; });
	check(until([&] { return inspect(running, [&](const Fake& f) { return f.progress_calls >= callbacks_before + 3; }); }, 1200) &&
		manifest_sync_count.load() == stable_saves,
		"waiting entry and late poster updates add no recurring manifest writes during active progress");
	manager.set_enabled(false);
	check(manager.pause(id), "download remains pausable after slow storage resumes");
	const auto paused = manager.find(id);
	check(paused && paused->remaining_seconds == -1 && paused->connected_peers == -1 && paused->connected_seeders == -1,
		"paused snapshots do not retain stale live ETA or peer counts");
	check(until([&] { return manager.find(waiting_id)->poster_path == (data / "downloads" / waiting_id / "poster.rgba").string(); }),
		"private artwork persistence resumes when the media writer becomes idle");
	manager.shutdown();
	check(manager.init(data.string()) && state(manager, waiting_id, DownloadState::Paused) &&
		!manager.find(waiting_id)->poster_path.empty(),
		"acknowledged queued request and its artwork survive restart without becoming complete");
	manager.shutdown();
}

void asynchronous_queue_lifecycle(const fs::path& data) {
	fs::create_directories(data);
	DownloadManager manager; std::string error;
	check(manager.init(data.string()), "asynchronous lifecycle fixture initializes");
	const auto first_plan = plan("logout-inflight"), second_plan = plan("logout-pending");
	std::promise<std::string> first_result, second_result;
	auto first = first_result.get_future(), second = second_result.get_future();
	manifest_sync_entered = false; release_manifest_sync = false; hold_manifest_sync = true;
	check(manager.enqueue_async(request("logout-inflight"), [&](std::string id, std::string error) {
		first_result.set_value(error.empty() ? std::move(id) : std::string{});
	}), "one selected request enters the dedicated persistence worker");
	check(until([] { return manifest_sync_entered.load(); }), "logout fixture holds an unpublished new-job commit");
	check(manager.enqueue_async(request("logout-pending"), [&](std::string id, std::string error) {
		second_result.set_value(error.empty() ? std::move(id) : std::string{});
	}), "a second selected request waits without starting another persistence thread");
	manager.set_enabled(false);
	manager.set_enabled(true);
	release_manifest_sync = true; hold_manifest_sync = false;
	check(first.wait_for(3s) == std::future_status::ready && second.wait_for(3s) == std::future_status::ready,
		"both pre-logout submissions finish after their durable commits");
	const auto first_id = first.get(), second_id = second.get();
	check(!first_id.empty() && !second_id.empty() && state(manager, first_id, DownloadState::Paused) &&
		state(manager, second_id, DownloadState::Paused),
		"submissions spanning disable and re-enable remain paused instead of auto-starting under a new account");
	check(manifest(data / "downloads" / first_id / "manifest.json").at("state") == static_cast<int>(DownloadState::Paused) &&
		manifest(data / "downloads" / second_id / "manifest.json").at("state") == static_cast<int>(DownloadState::Paused),
		"both in-flight and not-yet-started submissions persist the suspended state before acceptance");
	check(inspect(first_plan, [](const Fake& f) { return f.starts == 0; }) &&
		inspect(second_plan, [](const Fake& f) { return f.starts == 0; }),
		"account suspension cannot briefly start a hidden queued network transfer");
	manager.set_enabled(false);
	std::promise<std::string> duplicate_result;
	auto duplicate = duplicate_result.get_future();
	check(manager.enqueue_async(request("logout-inflight"), [&](std::string id, std::string error) {
		duplicate_result.set_value(error.empty() ? std::move(id) : std::string{});
	}) && duplicate.wait_for(3s) == std::future_status::ready && duplicate.get() == first_id && manager.snapshot().size() == 2,
		"asynchronous duplicate submission returns the existing stable id without another job");
	std::promise<bool> failed_result;
	auto failed = failed_result.get_future();
	fail_sync = EIO;
	check(manager.enqueue_async(request("async-storage-failure"), [&](std::string id, std::string error) {
		failed_result.set_value(id.empty() && !error.empty());
	}), "storage failure fixture reaches asynchronous admission");
	const bool failure_reported = failed.wait_for(3s) == std::future_status::ready && failed.get();
	fail_sync = 0;
	check(failure_reported && manager.snapshot().size() == 2,
		"failed durable enqueue reports failure and never publishes an accepted job");
	check(std::distance(fs::directory_iterator(data / "downloads"), fs::directory_iterator{}) == 2,
		"failed asynchronous enqueue removes its incomplete private directory");
	auto invalid = request("oversized-art-path"); invalid.poster_path.assign(16385, 'a');
	check(!manager.enqueue_async(invalid, {}), "oversized artwork paths cannot consume pending queue memory");

	std::promise<std::string> retained_result;
	auto retained = retained_result.get_future();
	manifest_sync_entered = false; release_manifest_sync = false; hold_manifest_sync = true;
	check(manager.enqueue_async(request("shutdown-inflight"), [&](std::string id, std::string error) {
		retained_result.set_value(error.empty() ? std::move(id) : std::string{});
	}), "shutdown fixture enters a durable enqueue");
	check(until([] { return manifest_sync_entered.load(); }), "shutdown fixture holds the in-flight commit");
	std::atomic<unsigned> cancelled{0}, unexpected{0};
	unsigned pending = 0;
	for (unsigned i = 0; i < 128; ++i) {
		auto large = request("pending-" + std::to_string(i));
		large.stream.description.assign(16384, 'd');
		large.stream.request_headers.assign(64, "X-Private: " + std::string(1000, 'h'));
		large.stream.raw = json{{"unused", std::string(1 << 20, 'r')}};
		if (!manager.enqueue_async(std::move(large), [&](std::string id, std::string error) {
			if (id.empty() && !error.empty()) ++cancelled; else ++unexpected;
		})) break;
		++pending;
	}
	check(pending > 0 && pending < 128, "aggregate request memory bounds admission before the per-job count limit");
	auto closed = std::async(std::launch::async, [&] { manager.shutdown(); });
	check(closed.wait_for(50ms) == std::future_status::timeout,
		"shutdown joins an in-flight durable commit instead of discarding accepted state");
	release_manifest_sync = true; hold_manifest_sync = false;
	check(closed.wait_for(3s) == std::future_status::ready, "shutdown finishes after the owned commit is released");
	closed.get();
	check(retained.wait_for(0ms) == std::future_status::ready, "in-flight committed request receives its completion before shutdown returns");
	const auto retained_id = retained.get();
	check(!retained_id.empty() && cancelled == pending && unexpected == 0,
		"unstarted submissions receive exactly one cancellation while the completed commit retains its id");
	check(!manager.enqueue_async(request("after-shutdown"), {}), "shutdown rejects later asynchronous submissions");
	check(manager.init(data.string()) && manager.snapshot().size() == 3 && state(manager, retained_id, DownloadState::Paused),
		"every acknowledged job survives shutdown and reload without resurrecting unstarted submissions");
	manager.shutdown();
}

void retry_and_remove(const fs::path& data) {
	fs::create_directories(data);
	DownloadManager manager; std::string error; manager.init(data.string());
	auto failed = plan("failure", true); change(failed, [](Fake& f) { f.failure = true; });
	const auto id = manager.enqueue(request("failure"), error); manager.set_enabled(true);
	check(until([&] { return state(manager, id, DownloadState::Failed); }), "transport failure is visible in download inventory");
	check(manager.find(id)->error.find("private-token") == std::string::npos, "provider URLs never escape through download errors");
	change(failed, [](Fake& f) { f.failure = false; });
	check(manager.resume(id), "failed selected stream can be retried");
	check(until([&] { return state(manager, id, DownloadState::Complete); }), "retry can complete and publish the video");
	auto malformed = plan("truncated", true); change(malformed, [](Fake& f) { f.malformed = true; });
	const auto truncated = manager.enqueue(request("truncated"), error);
	check(until([&] { return state(manager, truncated, DownloadState::Failed); }), "wrong final byte count never becomes a complete offline video");
	check(manager.find(truncated)->local_path.empty(), "truncated completion has no playable local path");
	auto held = plan("remove-running"); change(held, [](Fake& f) { f.release_cancel = false; });
	const auto removing = manager.enqueue(request("remove-running"), error);
	check(until([&] { return inspect(held, [](const Fake& f) { return f.starts == 1; }); }), "delete fixture reaches active file writer");
	const auto directory = data / "downloads" / removing;
	const auto outside = data / "keep.txt"; write(outside, "preserve"); fs::create_symlink(outside, directory / "link");
	check(manager.remove(removing, error) && !manager.find(removing), "delete hides selected item immediately without blocking the UI");
	check(until([&] { return inspect(held, [](const Fake& f) { return f.cancel_seen; }); }), "active delete requests transport cancellation");
	check(fs::exists(directory), "manager does not delete files while the transport still owns them");
	change(held, [](Fake& f) { f.release_cancel = true; });
	check(until([&] { return !fs::exists(directory); }) && bytes(outside) == "preserve", "delete cleans selected job only after writer returns");
	auto live = request("live"); live.live = true;
	check(manager.enqueue(live, error).empty() && !error.empty(), "live content cannot enter the finite offline queue");
	auto local = request("local"); local.stream.url = "file:///etc/passwd";
	check(manager.enqueue(local, error).empty(), "non-network descriptor cannot copy arbitrary local files");
	manager.shutdown();
}

void torrent_arbitration(const fs::path& data) {
	fs::create_directories(data);
	DownloadManager manager; std::string error; manager.init(data.string());
	auto bt = plan("bt"); change(bt, [](Fake& f) { f.release_cancel = false; });
	auto req = request("bt", true); req.type = "series"; req.video_id = "tt1234567:2:4"; req.season = 2; req.episode = 4;
	const auto bt_id = manager.enqueue(req, error); manager.set_enabled(true);
	check(until([&] { return inspect(bt, [](const Fake& f) { return f.starts == 1; }); }), "background torrent transfer starts when engine is free");
	check(inspect(bt, [](const Fake& f) { return f.last.season == 2 && f.last.episode == 4; }), "selected episode context reaches torrent file selection");
	manager.set_torrent_playback_active(true);
	auto resolver_lease = manager.acquire_torrent_resolution();
	check(until([&] { return inspect(bt, [](const Fake& f) { return f.cancel_seen; }); }), "foreground torrent requests background torrent cancellation");
	std::atomic<bool> cancel{false};
	auto ready = std::async(std::launch::async, [&] { return manager.wait_for_torrent_idle(cancel); });
	check(ready.wait_for(30ms) == std::future_status::timeout, "foreground cannot acquire engine before background transfer really returns");
	auto direct = plan("http-during-torrent"); const auto direct_id = manager.enqueue(request("http-during-torrent"), error);
	change(bt, [](Fake& f) { f.release_cancel = true; });
	check(ready.wait_for(2s) == std::future_status::ready && ready.get(), "foreground engine becomes available after background transport closes");
	check(until([&] { return state(manager, bt_id, DownloadState::Waiting); }), "preempted torrent remains waiting rather than failed");
	check(until([&] { return inspect(direct, [](const Fake& f) { return f.starts == 1; }); }), "HTTP download can proceed while foreground owns torrent engine");
	manager.set_torrent_playback_active(false);
	check(state(manager, bt_id, DownloadState::Waiting), "UI cancellation cannot release engine while an old foreground resolver still holds its lease");
	check(inspect(direct, [](const Fake& f) { return !f.cancel_seen; }), "torrent ownership changes do not cancel independent HTTP download");
	change(direct, [](Fake& f) { f.release = true; });
	check(until([&] { return state(manager, direct_id, DownloadState::Complete); }), "HTTP job completes normally alongside foreground torrent lifecycle");
	std::this_thread::sleep_for(150ms);
	check(inspect(bt, [](const Fake& f) { return f.starts == 1; }), "background torrent cannot restart in the gap between foreground wait and Engine.start");
	resolver_lease.reset();
	check(until([&] { return inspect(bt, [](const Fake& f) { return f.starts == 2; }); }), "waiting torrent resumes automatically after engine is released");
	change(bt, [](Fake& f) { f.release = true; });
	check(until([&] { return state(manager, bt_id, DownloadState::Complete); }), "resumed selected torrent becomes one offline episode");
	manager.shutdown();
	std::shared_ptr<void> discarded_task_lease;
	{
		DownloadManager temporary;
		discarded_task_lease = temporary.acquire_torrent_resolution();
	}
	discarded_task_lease.reset();
	check(true, "discarding a queued resolver after manager destruction releases its independent token safely");
}
} // namespace

extern "C" int __real_lstat(const char* path, struct stat* info);
extern "C" int __wrap_lstat(const char* path, struct stat* info) {
	if (ui_polling) ++ui_storage_calls;
	const int code = fail_lstat.load();
	if (code && path == denied_path) { errno = code; return -1; }
	return __real_lstat(path, info);
}
extern "C" int __real_chmod(const char* path, mode_t mode);
extern "C" int __wrap_chmod(const char* path, mode_t mode) {
	const int code = fail_chmod.load();
	if (code && path == denied_path) { errno = code; return -1; }
	return __real_chmod(path, mode);
}
extern "C" DIR* __real_opendir(const char* path);
extern "C" DIR* __wrap_opendir(const char* path) {
	const int code = fail_opendir.load();
	if (code && path == denied_path) { errno = code; return nullptr; }
	return __real_opendir(path);
}
extern "C" int __real_fchmod(int fd, mode_t mode);
extern "C" int __wrap_fchmod(int fd, mode_t mode) {
	const int code = fail_fchmod.load();
	if (code) { errno = code; return -1; }
	return __real_fchmod(fd, mode);
}
extern "C" ssize_t __real_write(int fd, const void* data, size_t size);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size) {
	if (ui_polling) ++ui_storage_calls;
	const int code = fail_write.load();
	if (code) { errno = code; return -1; }
	return __real_write(fd, data, size);
}
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
	if (ui_polling) ++ui_storage_calls;
	char path[4096]{};
	const auto descriptor = "/proc/self/fd/" + std::to_string(fd);
	const ssize_t count = ::readlink(descriptor.c_str(), path, sizeof(path) - 1);
	const bool is_manifest = count > 0 && std::string(path, size_t(count)).ends_with("/manifest.json.tmp");
	if (is_manifest) ++manifest_sync_count;
	const int code = fail_sync.load();
	if (code) { errno = code; return -1; }
	if (hold_manifest_sync.load()) {
		if (is_manifest && hold_manifest_sync.exchange(false)) {
			manifest_sync_entered = true;
			while (!release_manifest_sync.load()) std::this_thread::sleep_for(1ms);
		}
	}
	return __real_fsync(fd);
}

DownloadTransferResult download_transfer(const DownloadTransferRequest& request,
	const DownloadProgressCallback& progress, const std::atomic<bool>& cancel) {
	std::shared_ptr<Fake> fake;
	{
		std::lock_guard lock(fake_mutex);
		const auto it = fakes.find(request.stream.filename);
		if (it == fakes.end()) throw std::runtime_error("missing test plan");
		fake = it->second; ++fake->starts; fake->cancel_seen = fake->returned = false; fake->last = request;
		fake->offset = fs::exists(request.partial_path) ? static_cast<int64_t>(fs::file_size(request.partial_path)) : 0;
	}
	if (!fs::exists(request.partial_path)) write(request.partial_path, std::string(4096, 'a'));
	{
		std::unique_lock lock(fake_mutex);
		while (fake->hold_progress && !fake->progress_release && !cancel.load()) fake_changed.wait_for(lock, 5ms);
	}
	progress({static_cast<int64_t>(fs::file_size(request.partial_path)), 8192, 128000,
		request.stream.info_hash.empty() ? -1 : 4, 10, 3, 2});
	{
		std::unique_lock lock(fake_mutex);
		++fake->progress_calls;
		while (!fake->release && !cancel.load()) {
			fake_changed.wait_for(lock, fake->periodic_progress ? 250ms : 5ms);
			if (fake->periodic_progress && !fake->release && !cancel.load()) {
				const auto done = fake->progress_done;
				lock.unlock();
				write(request.partial_path, std::string(size_t(done), 'a'));
				progress({done, 8192, 128000, 4, 10, 3, 2});
				lock.lock(); ++fake->progress_calls;
			}
		}
		if (cancel.load()) {
			fake->cancel_seen = true; fake_changed.notify_all();
			while (!fake->release_cancel) fake_changed.wait_for(lock, 5ms);
			if (fake->cancelled_file_size >= 0)
				write(request.partial_path, std::string(size_t(fake->cancelled_file_size), 'a'));
			const auto prefix = fake->cancelled_prefix >= 0 ? fake->cancelled_prefix :
				static_cast<int64_t>(fs::file_size(request.partial_path));
			fake->returned = true;
			return {DownloadTransferStatus::Cancelled, {}, ".mp4", prefix, 8192};
		}
		if (fake->throws) throw std::runtime_error("https://provider.invalid/private-token");
		if (fake->failure) { fake->returned = true; return {DownloadTransferStatus::Error, "https://provider.invalid/private-token failed", ".mp4", 4096, 8192}; }
	}
	write(request.partial_path, std::string(8192, 'a')); progress({8192, 8192, 128000});
	std::lock_guard lock(fake_mutex); fake->returned = true;
	return {DownloadTransferStatus::Complete, {}, ".mp4", 8192, fake->malformed ? 16384 : 8192};
}

int main(int argc, char** argv) {
	if (argc != 2) return 2;
	const fs::path root = fs::absolute(argv[1]);
	storage_permissions_and_retry(root / "storage");
	basic_queue(root / "queue");
	crash_and_validation(root / "crash");
	retry_and_remove(root / "remove");
	torrent_arbitration(root / "torrent");
	progress_persistence(root / "progress-persistence");
	progress_metadata_failure(root / "metadata-failure");
	progressive_leases(root / "progressive");
	cancelled_unacknowledged_tail(root / "cancelled-tail");
	snapshot_during_slow_storage(root / "slow-storage");
	asynchronous_queue_lifecycle(root / "async-lifecycle");
	std::cout << "Download manager: " << checks - failures << '/' << checks << " checks passed\n";
	return failures ? 1 : 0;
}
