// Independent relocation fault tests against real DownloadManager and files.
// No media network requests; the transfer boundary creates a bounded fixture.
#include "download_manager.h"
#include "download_transfer.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <fcntl.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
void dlog(const char*, ...) {}
namespace {
int checks = 0;
std::atomic<bool> fail_destination_media_write{false};
std::atomic<bool> fail_source_unlink{false};
std::atomic<unsigned> fault_hits{0};
int crash_phase = 0;
std::string fault_destination, fault_source;
constexpr size_t media_size = 65537;
const std::string media_bytes(media_size, 'V');
void expect(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class Test> bool until(Test test, int milliseconds = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do { if (test()) return true; std::this_thread::sleep_for(5ms); } while (std::chrono::steady_clock::now() < deadline);
    return test();
}
std::string contents(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string descriptor_path(int fd) {
    char path[4096];
    const auto size = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), path, sizeof(path));
    return size < 0 ? "" : std::string(path, size_t(size));
}
bool below(const std::string& path, const std::string& directory) {
    return !directory.empty() && path.starts_with(directory + '/');
}
DownloadRequest request() {
    DownloadRequest r; r.media_id = "move:test"; r.video_id = "move:test:1"; r.type = "movie";
    r.title = "Migration fixture"; r.stream.kind = StreamKind::Direct;
    r.stream.url = "https://fixture.invalid/private/selected.mkv";
    return r;
}
struct Fixture {
    fs::path data, registry, old_root, target;
    std::string id;
    Fixture(const fs::path& base, const fs::path& destination)
        : data(base), registry(base / "appdata"), old_root(base / "downloads"), target(destination) {
        fs::create_directories(registry); fs::create_directories(target);
        DownloadManager seed; std::string error;
        expect(seed.init(data.string(), &error), "fixture initializes legacy storage");
        id = seed.enqueue(request(), error);
        expect(!id.empty(), "fixture enqueues one owned job");
        seed.set_enabled(true);
        expect(until([&] { auto e = seed.find(id); return e && e->state == DownloadState::Complete; }), "fixture completes bounded media");
        seed.shutdown();
        expect(contents(old_root / id / "media.mkv") == media_bytes, "fixture contains exact media bytes");
    }
    void initialize(DownloadManager& manager) const {
        std::string error;
        expect(manager.init(data.string(), registry.string(), old_root.string(), &error), "manager opens registered original root");
        expect(manager.snapshot().size() == 1, "manager discovers one original job");
    }
    void verify_one_at_target(DownloadManager& manager) const {
        const auto entries = manager.snapshot();
        expect(entries.size() == 1 && entries[0].state == DownloadState::Complete, "relocation exposes exactly one complete job");
        expect(below(entries[0].local_path, target.string()) && contents(entries[0].local_path) == media_bytes,
               "relocated job is byte-exact at chosen destination");
        expect(!fs::exists(old_root / id), "successful relocation removes original job directory");
        expect(manager.download_directory() == target.string(), "chosen directory commits with inventory");
    }
};
void same_filesystem(const fs::path& base) {
    Fixture f(base / "same-data", base / "same-target"); DownloadManager manager; f.initialize(manager);
    std::string error;
    expect(manager.set_download_directory(f.target.string(), error), "same-filesystem relocation succeeds");
    f.verify_one_at_target(manager); manager.shutdown();
    expect(manager.init(f.data.string(), f.registry.string(), "", &error), "same-filesystem relocation reloads");
    f.verify_one_at_target(manager); manager.shutdown();
}
void full_destination(const fs::path& base, const fs::path& external) {
    Fixture f(base / "full-data", external / "full-target"); DownloadManager manager; f.initialize(manager);
    struct stat from{}, to{};
    expect(::stat(f.old_root.c_str(), &from) == 0 && ::stat(f.target.c_str(), &to) == 0 && from.st_dev != to.st_dev,
           "cross-filesystem test uses truly distinct devices");
    fault_destination = f.target.string(); fault_hits = 0; fail_destination_media_write = true;
    std::string error; const bool moved = manager.set_download_directory(f.target.string(), error);
    fail_destination_media_write = false;
    expect(fault_hits > 0, "cross-device copy reaches injected media ENOSPC");
    expect(!moved && !error.empty(), "ENOSPC cannot be acknowledged as successful relocation");
    expect(contents(f.old_root / f.id / "media.mkv") == media_bytes, "ENOSPC preserves complete original bytes");
    manager.shutdown();
    (void)manager.init(f.data.string(), f.registry.string(), "", &error);
    expect(manager.storage_readable(), "ENOSPC recovery retains readable storage");
    expect(manager.snapshot().size() == 1 && contents(manager.snapshot()[0].local_path) == media_bytes,
           "ENOSPC restart exposes one intact video");
    expect(manager.set_download_directory(f.target.string(), error), "retry after ENOSPC completes relocation");
    f.verify_one_at_target(manager); manager.shutdown();
}
void active_helper_lock(const fs::path& base) {
    Fixture f(base / "lock-data", base / "lock-target"); DownloadManager manager; f.initialize(manager);
    const int lock = ::open((f.old_root / f.id).c_str(), O_RDONLY | O_DIRECTORY);
    expect(lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) == 0, "fixture holds the same directory lock as native helper");
    auto move = std::async(std::launch::async, [&] { std::string error; return manager.set_download_directory(f.target.string(), error); });
    const bool returned = move.wait_for(150ms) == std::future_status::ready;
    const bool original_intact = contents(f.old_root / f.id / "media.mkv") == media_bytes;
    bool result = false;
    if (returned) result = move.get();
    ::flock(lock, LOCK_UN); ::close(lock);
    if (!returned) { expect(move.wait_for(5s) == std::future_status::ready, "helper-lock wait is bounded after release"); result = move.get(); }
    expect(original_intact && (!returned || !result), "active helper lock prevents source mutation or success before release");
    if (!result) { std::string error; expect(manager.set_download_directory(f.target.string(), error), "relocation retries once writer lock is released"); }
    f.verify_one_at_target(manager); manager.shutdown();
}
void failed_source_retirement(const fs::path& base, const fs::path& external) {
    Fixture f(base / "retire-data", external / "retire-target"); DownloadManager manager; f.initialize(manager);
    fault_source = f.old_root.string(); fault_hits = 0; fail_source_unlink = true;
    std::string error; const bool moved = manager.set_download_directory(f.target.string(), error);
    fail_source_unlink = false;
    expect(fault_hits > 0 && !moved && !error.empty(), "failed source retirement cannot claim a completed move");
    expect(contents(f.old_root / f.id / "media.mkv") == media_bytes, "failed source unlink preserves original media");
    manager.shutdown(); (void)manager.init(f.data.string(), f.registry.string(), "", &error);
    const auto recovered = manager.snapshot();
    expect(recovered.size() == 1 && contents(recovered[0].local_path) == media_bytes,
           "published destination plus unretired source exposes one usable inventory entry");
    expect(manager.set_download_directory(f.target.string(), error), "same-target retry completes pending source retirement");
    f.verify_one_at_target(manager);
    expect(!fs::exists(f.registry / "download-relocation.json"), "successful cleanup retires relocation journal");
    manager.shutdown();
}
void crash_recovery(const fs::path& base, const fs::path& external, int phase) {
    const auto label = "crash-" + std::to_string(phase);
    Fixture f(base / (label + "-data"), external / (label + "-target"));
    const pid_t child = ::fork();
    expect(child >= 0, "relocation crash fixture forks before manager workers exist");
    if (child == 0) {
        DownloadManager manager; f.initialize(manager);
        fault_source = f.old_root.string(); fault_destination = f.target.string(); crash_phase = phase;
        std::string error; (void)manager.set_download_directory(f.target.string(), error);
        ::_exit(2); // A configured crash boundary must actually have been reached.
    }
    int status = 0;
    expect(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 90 + phase,
           "process termination reaches the selected real filesystem boundary");
    DownloadManager manager; std::string error;
    (void)manager.init(f.data.string(), f.registry.string(), "", &error);
    const auto entries = manager.snapshot();
    expect(entries.size() == 1 && entries[0].state == DownloadState::Complete && contents(entries[0].local_path) == media_bytes,
           "metadata-only startup recovery keeps exactly one intact playable entry");
    expect(manager.set_download_directory(f.target.string(), error), "explicit same-target retry finishes interrupted move");
    f.verify_one_at_target(manager);
    expect(!fs::exists(f.registry / "download-relocation.json"), "completed crash recovery clears durable journal");
    for (const auto& entry : fs::directory_iterator(f.target))
        expect(!entry.path().filename().string().starts_with(".stremio-move-"), "completed crash recovery leaves no staging duplicate");
    manager.shutdown();
}
void colliding_ids_and_registry_backup(const fs::path& base) {
    Fixture f(base / "collision-data", base / "collision-target");
    const auto other = base / "collision-other"; fs::create_directories(other);
    fs::copy(f.old_root / f.id, other / f.id, fs::copy_options::recursive);
    auto different = json::parse(contents(other / f.id / "manifest.json"));
    different["media_id"] = "other:title"; different["video_id"] = "other:video";
    different["title"] = "Different collision title"; different["identity"] = "other-source-identity";
    different["stream"]["url"] = "https://fixture.invalid/other.mkv";
    { std::ofstream out(other / f.id / "manifest.json"); out << different.dump(); }
    const std::string other_bytes(media_size, 'X');
    { std::ofstream out(other / f.id / "media.mkv", std::ios::binary); out << other_bytes; }
    { std::ofstream out(f.registry / "download-directories.json");
      out << json{{"version", 1}, {"current", f.old_root.string()},
                  {"roots", {f.old_root.string(), other.string()}}}.dump(); }
    DownloadManager manager; std::string error;
    expect(manager.init(f.data.string(), f.registry.string(), "", &error), "colliding source roots initialize");
    const auto before = manager.snapshot();
    expect(before.size() == 2 && before[0].id != before[1].id, "same raw folder ID receives distinct public identities");
    std::map<std::string, std::string> identities;
    for (const auto& item : before) identities[item.media_id] = item.id;
    expect(manager.set_download_directory(f.target.string(), error), "colliding owned folders relocate without overwriting");
    auto verify = [&] {
        const auto entries = manager.snapshot();
        expect(entries.size() == 2, "collision relocation retains both videos exactly once");
        for (const auto& item : entries) {
            expect(item.id == identities[item.media_id] && below(item.local_path, f.target.string()),
                   "public ID and destination survive raw folder remapping");
            expect(contents(item.local_path) == (item.media_id == "other:title" ? other_bytes : media_bytes),
                   "raw ID collision cannot overwrite either media payload");
            const auto disk = json::parse(contents(fs::path(item.local_path).parent_path() / "manifest.json"));
            expect(disk.at("id") == fs::path(item.local_path).parent_path().filename().string() &&
                   disk.value("public_id", disk.at("id").get<std::string>()) == item.id,
                   "moved manifest preserves raw folder identity and optional stable public alias");
        }
        expect(!fs::exists(f.old_root / f.id) && !fs::exists(other / f.id), "collision move leaves neither original source job");
    };
    verify(); manager.shutdown();
    { std::ofstream out(f.registry / "download-directories.json"); out << "damaged primary registry"; }
    expect(manager.init(f.data.string(), f.registry.string(), "", &error), "final registry backup recovers damaged primary");
    expect(manager.download_directory() == f.target.string(), "successful move backup preserves final chosen root");
    verify(); manager.shutdown();
}
void inaccessible_source(const fs::path& base) {
    Fixture f(base / "missing-data", base / "missing-target"); DownloadManager manager; f.initialize(manager);
    manager.shutdown();
    const auto hidden = f.old_root.string() + "-disconnected";
    fs::rename(f.old_root, hidden);
    std::string error; (void)manager.init(f.data.string(), f.registry.string(), "", &error);
    expect(!manager.set_download_directory(f.target.string(), error), "unavailable known source cannot be treated as an empty successful move");
    expect(contents(fs::path(hidden) / f.id / "media.mkv") == media_bytes, "unmounted source content remains unchanged");
    manager.shutdown(); fs::rename(hidden, f.old_root);
}
}
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __wrap_write(int fd, const void* bytes, size_t count) {
    const auto path = descriptor_path(fd);
    if (fail_destination_media_write && below(path, fault_destination) && fs::path(path).filename().string().starts_with("media.")) {
        ++fault_hits; errno = ENOSPC; return -1;
    }
    const auto written = __real_write(fd, bytes, count);
    if (written > 0 && crash_phase == 3 && below(path, fault_destination) &&
        fs::path(path).filename().string().starts_with("media.")) ::_exit(93);
    return written;
}
extern "C" int __real_unlink(const char*);
extern "C" int __wrap_unlink(const char* path) {
    if (fail_source_unlink && below(path, fault_source)) { ++fault_hits; errno = EACCES; return -1; }
    return __real_unlink(path);
}
extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* from, const char* to) {
    const int result = __real_rename(from, to);
    const fs::path target(to);
    const auto name = target.filename().string();
    if (result == 0 && crash_phase == 1 && target.parent_path() == fs::path(fault_destination) &&
        name.size() == 33 && name.front() == 'd') ::_exit(91);
    return result;
}
extern "C" int __real_rmdir(const char*);
extern "C" int __wrap_rmdir(const char* path) {
    const int result = __real_rmdir(path);
    if (result == 0 && crash_phase == 2 && fs::path(path).parent_path() == fs::path(fault_source)) ::_exit(92);
    return result;
}
DownloadTransferResult download_transfer(const DownloadTransferRequest& r, const DownloadProgressCallback& progress,
                                        const std::atomic<bool>& cancel) {
    if (cancel) return {DownloadTransferStatus::Cancelled, "", ".mkv", 0, -1};
    std::ofstream output(r.partial_path, std::ios::binary);
    output.write(media_bytes.data(), media_bytes.size()); output.close();
    if (!output) return {DownloadTransferStatus::Error, "Fixture failed"};
    progress({int64_t(media_size), int64_t(media_size), 0});
    return {DownloadTransferStatus::Complete, "", ".mkv", int64_t(media_size), int64_t(media_size)};
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        const fs::path base(argv[1]), external(argv[2]); fs::create_directories(base); fs::create_directories(external);
        same_filesystem(base); full_destination(base, external); active_helper_lock(base);
        failed_source_retirement(base, external); colliding_ids_and_registry_backup(base);
        crash_recovery(base, external, 1); crash_recovery(base, external, 2); crash_recovery(base, external, 3);
        inaccessible_source(base);
        std::cout << "Independent download relocation faults: " << checks << " checks passed\n";
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
