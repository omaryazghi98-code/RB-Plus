// Real disk worker with deterministic I/O gates. No external swarm is used.
#include "../src/torrent/engine.cpp"

#include <chrono>
#include <filesystem>
#include <future>
#include <sys/stat.h>
#include <iostream>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;
int checks = 0, network_attempts = 0;
std::atomic<int> writes{0}, reads{0};
std::atomic<int> fail_write_fd{-1}, fail_read_fd{-1};
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    int fd = -1;
    bool entered = false, released = false;
    void arm(int target) { std::lock_guard<std::mutex> lock(mutex); fd = target; entered = released = false; }
    void block(int target) {
        std::unique_lock<std::mutex> lock(mutex);
        if (target != fd) return;
        entered = true; cv.notify_all();
        cv.wait(lock, [&] { return released; });
    }
    bool wait() {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, 5s, [&] { return entered; });
    }
    void release() { std::lock_guard<std::mutex> lock(mutex); released = true; fd = -1; cv.notify_all(); }
} write_gate, read_gate;

struct Fixture {
    std::shared_ptr<bt::Torrent> torrent = std::make_shared<bt::Torrent>();
    std::string data;
    int reader = 0;
    Fixture(const fs::path& folder, int slots = 2, int64_t length = 32768, int pieces = 8, bool memory_download = false) {
        auto& I = bt::impl();
        std::lock_guard<std::mutex> lock(I.mu);
        check(!I.disk_running && I.torrents.empty(), "worker starts cold between cases");
        fs::create_directories(folder);
        I.data_dir = folder.string(); I.cache_bytes = slots * length;
        I.speed_profile = bt::SpeedProfile::UltraFast;
        data.assign(size_t(length), 'P');
        unsigned char hash[20]; SHA1(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);
        std::string hashes;
        for (int n = 0; n < pieces; ++n) hashes.append(reinterpret_cast<const char*>(hash), 20);
        BValue metadata = BValue::dict();
        metadata.d["name"] = BValue(std::string("synthetic.bin"));
        metadata.d["length"] = BValue(int64_t(pieces) * length);
        metadata.d["piece length"] = BValue(length);
        metadata.d["pieces"] = BValue(hashes);
        torrent->hex = "0123456789abcdef0123456789abcdef01234567";
        check(bt::parse_info(torrent.get(), bencode(metadata)), "real metadata parser accepts fixture");
        // Deliberately small table, to force eviction in the pin test.
        torrent->nslots = slots; torrent->piece_in_slot.assign(size_t(slots), -1);
        torrent->slot_touch.assign(size_t(slots), 0); bt::ensure_slot_state(torrent.get());
        auto peer = std::make_unique<bt::Peer>();
        peer->state = bt::Peer::Active; peer->peer_choking = false;
        peer->key = 0x10000; peer->has.assign(size_t(pieces), 1); peer->has_count = pieces;
        torrent->peers.push_back(std::move(peer));
        reader = torrent->next_reader++;
        torrent->readers[reader] = 0; torrent->reader_files[reader] = 0;
        if (memory_download) { torrent->download_readers.insert(reader); torrent->used_download_memory = true; }
        I.torrents.push_back(torrent);
        bt::start_disk_worker_locked();
    }
    void deliver(int index, bool corrupt = false) {
        std::lock_guard<std::mutex> lock(bt::impl().mu);
        check(bt::make_payload_room_locked(torrent.get(), torrent->plen), "piece is admitted inside payload budget");
        bt::Piece piece;
        piece.memory = std::make_shared<bt::PayloadReservation>(torrent->plen);
        piece.data.resize(size_t(torrent->plen));
        piece.got.assign(size_t(torrent->blocks(index)), 0);
        piece.asked.assign(piece.got.size(), 0);
        piece.asked_at.assign(piece.got.size(), 0);
        torrent->active.emplace(index, std::move(piece));
        std::string block;
        for (int offset = 0; offset < torrent->plen; offset += bt::kBlock) {
            const int count = int(std::min<int64_t>(bt::kBlock, torrent->plen - offset));
            const char* bytes = data.data() + offset;
            if (corrupt && offset == 0) { block.assign(bytes, size_t(count)); block[0] ^= 1; bytes = block.data(); }
            bt::on_block(torrent.get(), torrent->peers[0].get(), uint32_t(index), uint32_t(offset), bytes, size_t(count), now_seconds());
        }
        check(torrent->disk_pending.count(index) && !torrent->have[size_t(index)], "completed network piece waits for verification and cache publication");
    }
    void idle() { bt::wait_for_disk_idle(); }
    ~Fixture() {
        write_gate.release(); read_gate.release();
        bt::Engine::get().shutdown();
        std::lock_guard<std::mutex> lock(bt::impl().mu);
        bt::impl().torrents.clear(); bt::impl().running = false;
    }
};

void responsive_stats(const std::string& hash) {
    auto result = std::async(std::launch::async, [&] { return bt::Engine::get().stats(hash); });
    const bool ready = result.wait_for(200ms) == std::future_status::ready;
    if (!ready) { write_gate.release(); read_gate.release(); }
    check(ready, "stats and network state remain responsive while disk I/O is held");
    result.get();
}
}

extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, size_t count, off_t offset) {
    ++writes; write_gate.block(fd);
    if (fail_write_fd == fd) { errno = ENOSPC; return -1; }
    return __real_pwrite(fd, data, count, offset);
}
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* data, size_t count, off_t offset) {
    ++reads;
    if (fail_read_fd == fd) { errno = EIO; return -1; }
    read_gate.block(fd);
    return __real_pread(fd, data, count, offset);
}
extern "C" int __wrap_socket(int, int, int) { ++network_attempts; errno = ENETUNREACH; return -1; }
extern "C" int __wrap_connect(int, const sockaddr*, socklen_t) { ++network_attempts; errno = ENETUNREACH; return -1; }
extern "C" int __wrap_getaddrinfo(const char*, const char*, const addrinfo*, addrinfo**) { ++network_attempts; return EAI_FAIL; }

int main(int argc, char** argv) {
    try {
        check(argc == 2, "temporary output directory supplied");
        const fs::path root(argv[1]);
        {
            Fixture f(root / "write");
            write_gate.arm(fileno(f.torrent->cache));
            f.deliver(0);
            check(write_gate.wait(), "worker reaches gated pwrite");
            responsive_stats(f.torrent->hex);
            {
                std::lock_guard<std::mutex> lock(bt::impl().mu);
                check(!f.torrent->have[0] && !f.torrent->ever[0], "unwritten piece is never visible to the player or progress");
            }
            f.deliver(1);
            check(f.torrent->unique_window == 2 * f.torrent->plen, "network accepts another piece while the disk is held");
            write_gate.release(); f.idle();
            std::vector<uint8_t> bytes(size_t(f.torrent->plen));
            const int before = reads.load();
            const int count = bt::Engine::get().read(f.torrent, f.reader, 0, f.torrent->plen, bytes.data(), int(bytes.size()), nullptr);
            check(count == int(bytes.size()) && memcmp(bytes.data(), f.data.data(), bytes.size()) == 0, "hot verified piece returns exact data");
            check(reads == before, "immediate download reader avoids rereading the just-written cache piece");
            check(f.torrent->disk_pending.empty() && f.torrent->disk_pending_bytes == 0, "worker releases all pending state");
        }
        check(bt::payload_memory_bytes == 0, "all shared payload reservations released after shutdown");
        {
            Fixture f(root / "download-memory", 32, 2ll << 20, 128, true);
            const int before_writes = writes.load(), before_reads = reads.load();
            for (int piece = 0; piece < 32; ++piece) f.deliver(piece);
            f.idle();
            check(writes == before_writes && bt::payload_memory_bytes == (64ll << 20),
                  "download-only pipeline verifies 64 MiB without duplicating any media write in the rolling cache");
            auto stats = bt::Engine::get().stats(f.torrent->hex);
            check(stats.found && stats.peers == 1 && stats.seeders == 1,
                  "seeder telemetry counts a connected peer which advertises the complete bitfield");
            {
                std::lock_guard<std::mutex> lock(bt::impl().mu);
                auto* peer = f.torrent->peers[0].get(); peer->max_reqs = peer->reqq = 8000;
                for (int sample = 0; sample < 20; ++sample) bt::pick(f.torrent.get(), peer, now_seconds());
                check(peer->reqs.empty() && f.torrent->active.empty() && f.torrent->have[0] && f.torrent->have[31],
                      "a stationary download writer applies backpressure instead of evicting and requesting its future prefix again");
                auto partial = std::make_unique<bt::Peer>(); partial->state = bt::Peer::Active; partial->has_count = 127;
                auto dead = std::make_unique<bt::Peer>(); dead->state = bt::Peer::Active; dead->has_count = 128; dead->dead = true;
                auto connecting = std::make_unique<bt::Peer>(); connecting->state = bt::Peer::Connecting; connecting->has_count = 128;
                f.torrent->peers.push_back(std::move(partial)); f.torrent->peers.push_back(std::move(dead));
                f.torrent->peers.push_back(std::move(connecting));
            }
            stats = bt::Engine::get().stats(f.torrent->hex);
            check(stats.peers == 2 && stats.seeders == 1, "partial, disconnected and handshaking peers never inflate connected seeders");
            check(bt::Engine::get().stats("not-running").seeders == -1, "missing torrent has unknown seeders, not a fabricated zero");
            int auxiliary = -1; int64_t total = 0; std::string error;
            check(bool(bt::Engine::get().open_reader(f.torrent->hex, 0, &auxiliary, &total, &error, bt::ReaderRole::Auxiliary)),
                  "metadata tail reader opens alongside a full RAM download window");
            {
                std::lock_guard<std::mutex> lock(bt::impl().mu);
                f.torrent->readers[auxiliary] = 127 * f.torrent->plen; f.torrent->wanted_dirty = true;
            }
            f.deliver(127); f.idle();
            uint8_t bytes[32]{};
            check(bt::Engine::get().read(f.torrent, auxiliary, 0, 127 * f.torrent->plen, bytes, sizeof(bytes), nullptr) == int(sizeof(bytes)) && bytes[0] == 'P',
                  "auxiliary metadata is served from its bounded verified tail slot");
            check(f.torrent->have[0] && f.torrent->have[30] && f.torrent->have[127] && !f.torrent->have[31] &&
                  bt::payload_memory_bytes == (64ll << 20),
                  "tail reservation shrinks read-ahead once while preserving the current prefix and the 64 MiB handoff bound");
            bt::Engine::get().close_reader(f.torrent, auxiliary);
            f.deliver(31); f.idle();
            check(!f.torrent->have[127] && f.torrent->have[0] && f.torrent->have[31],
                  "closing metadata returns its slot to sequential download read-ahead");
            check(bt::Engine::get().read(f.torrent, f.reader, 0, f.torrent->plen, bytes, sizeof(bytes), nullptr) == int(sizeof(bytes)) && bytes[0] == 'P',
                  "the advancing download reader receives the exact verified prefix bytes");
            f.deliver(32); f.idle();
            check(!f.torrent->have[0] && f.torrent->have[1] && f.torrent->have[32] && writes == before_writes && reads == before_reads,
                  "advancing the writer reuses consumed RAM with no disk-cache writes or rereads");
        }
        check(bt::payload_memory_bytes == 0, "download handoff reservations release after shutdown");
        {
            Fixture f(root / "pin");
            f.deliver(0); f.idle(); f.deliver(1); f.idle();
            read_gate.arm(fileno(f.torrent->cache));
            std::vector<uint8_t> bytes(16384);
            auto reading = std::async(std::launch::async, [&] { return bt::Engine::get().read(f.torrent, f.reader, 0, 0, bytes.data(), int(bytes.size()), nullptr); });
            const bool entered = read_gate.wait();
            if (!entered) read_gate.release();
            check(entered, "older cache piece takes real gated pread path");
            responsive_stats(f.torrent->hex);
            {
                std::lock_guard<std::mutex> lock(bt::impl().mu);
                const int pinned = f.torrent->slot_of[0];
                check(f.torrent->slot_pins[size_t(pinned)] == 1, "pread pins its exact cache slot");
                const int victim = bt::take_slot(f.torrent.get(), now_seconds());
                check(victim >= 0 && victim != pinned && f.torrent->have[0], "eviction cannot reuse the slot of an in-flight read");
            }
            f.deliver(2); f.idle();
            read_gate.release();
            check(reading.get() == int(bytes.size()) && memcmp(bytes.data(), f.data.data(), bytes.size()) == 0,
                  "pinned read returns original bytes while other slots are overwritten");
            check(std::all_of(f.torrent->slot_pins.begin(), f.torrent->slot_pins.end(), [](unsigned n) { return n == 0; }), "read releases slot pins");
        }
        {
            Fixture f(root / "corrupt");
            const int before = writes.load(); f.deliver(0, true); f.idle();
            check(!f.torrent->have[0] && !f.torrent->ever[0] && writes == before, "SHA-1 failure never writes or publishes corrupt data");
            check(f.torrent->peers[0]->dead && f.torrent->active.at(0).got_count == 0,
                  "corrupt sender is rejected and piece is reset for retry");
        }
        {
            Fixture f(root / "read-recovery", 3);
            f.deliver(0); f.idle(); f.deliver(1); f.idle();
            const int old_slot = f.torrent->slot_of[0];
            read_gate.arm(fileno(f.torrent->cache));
            uint8_t original_bytes[16]{};
            auto older_read = std::async(std::launch::async, [&] {
                return bt::Engine::get().read(f.torrent, f.reader, 0, 0, original_bytes, sizeof(original_bytes), nullptr);
            });
            const bool entered = read_gate.wait();
            if (!entered) read_gate.release();
            check(entered, "first reader holds the original slot during a concurrent cache error");
            fail_read_fd = fileno(f.torrent->cache);
            uint8_t retry_bytes[16]{};
            const int failed = bt::Engine::get().read(f.torrent, f.reader, 0, 0, retry_bytes, sizeof(retry_bytes), nullptr);
            fail_read_fd = -1;
            bool invalidated = false;
            {
                std::lock_guard<std::mutex> lock(bt::impl().mu);
                invalidated = failed == -1 && !f.torrent->have[0] && f.torrent->slot_of[0] == -1 &&
                              f.torrent->piece_in_slot[size_t(old_slot)] == -1 && f.torrent->slot_pins[size_t(old_slot)] == 1;
            }
            if (!invalidated) read_gate.release();
            check(invalidated, "EIO invalidates both mappings without releasing another reader's pin");
            f.deliver(0); f.idle();
            const int fresh_slot = f.torrent->slot_of[0];
            const bool fresh = f.torrent->have[0] && fresh_slot >= 0 && fresh_slot != old_slot;
            if (!fresh) read_gate.release();
            check(fresh, "redownload publishes the same piece into a different unpinned slot");
            read_gate.release();
            check(older_read.get() == int(sizeof(original_bytes)) && original_bytes[0] == 'P',
                  "older pinned read can finish after recovery without changing the new mapping");
            {
                std::lock_guard<std::mutex> lock(bt::impl().mu);
                bt::forget_slot_locked(f.torrent.get(), old_slot);
                check(f.torrent->have[0] && f.torrent->slot_of[0] == fresh_slot &&
                      f.torrent->piece_in_slot[size_t(fresh_slot)] == 0,
                      "evicting the old slot preserves the newly verified mapping");
                // Reproduce a stale backlink explicitly as a defensive check.
                f.torrent->piece_in_slot[size_t(old_slot)] = 0;
                bt::forget_slot_locked(f.torrent.get(), old_slot);
                check(f.torrent->have[0] && f.torrent->slot_of[0] == fresh_slot,
                      "stale slot backlink cannot invalidate a newer copy of the same piece");
            }
            f.deliver(2); f.idle();
            check(bt::Engine::get().read(f.torrent, f.reader, 0, 0, retry_bytes, sizeof(retry_bytes), nullptr) == int(sizeof(retry_bytes)) && retry_bytes[0] == 'P',
                  "verified recovered bytes remain readable after the old cache slot is reused");
        }
        {
            Fixture f(root / "disk-errors");
            f.deliver(0); f.idle(); f.deliver(1); f.idle();
            fail_read_fd = fileno(f.torrent->cache);
            uint8_t bytes[16]{};
            check(bt::Engine::get().read(f.torrent, f.reader, 0, 0, bytes, sizeof(bytes), nullptr) == -1 && !f.torrent->have[0],
                  "failed positional read invalidates the cached piece and requests a fresh copy");
            fail_read_fd = -1;
            fail_write_fd = fileno(f.torrent->cache);
            f.deliver(2); f.idle();
            fail_write_fd = -1;
            check(f.torrent->cache_failed && f.torrent->have[2] && f.torrent->ever[2], "cache ENOSPC falls back to verified RAM without losing the completed piece");
            check(bt::Engine::get().read(f.torrent, f.reader, 0, 2 * f.torrent->plen, bytes, sizeof(bytes), nullptr) == int(sizeof(bytes)) && bytes[0] == 'P',
                  "bounded fallback cache serves exact bytes after a disk write error");
            check(bt::payload_memory_bytes <= bt::kMaxActiveBytes, "fallback and active work share the same payload budget");
        }
        {
            Fixture f(root / "stop");
            write_gate.arm(fileno(f.torrent->cache)); f.deliver(0);
            check(write_gate.wait(), "stop case holds real pwrite in flight");
            f.deliver(1);
            std::shared_ptr<bt::Torrent> replacement = std::make_shared<bt::Torrent>();
            {
                std::lock_guard<std::mutex> lock(bt::impl().mu);
                BValue metadata = BValue::dict();
                metadata.d["name"] = BValue(std::string("replacement.bin"));
                metadata.d["length"] = BValue(int64_t(8) * f.torrent->plen);
                metadata.d["piece length"] = BValue(f.torrent->plen);
                metadata.d["pieces"] = BValue(f.torrent->hashes);
                replacement->hex = f.torrent->hex;
                check(bt::parse_info(replacement.get(), bencode(metadata)), "metadata for a new torrent can initialize while the old write is held");
                struct stat old_stat{}, new_stat{};
                check(fstat(fileno(f.torrent->cache), &old_stat) == 0 && fstat(fileno(replacement->cache), &new_stat) == 0 &&
                      (old_stat.st_dev != new_stat.st_dev || old_stat.st_ino != new_stat.st_ino),
                      "new torrent cache uses a distinct inode even for the same info hash");
            }
            auto stopping = std::async(std::launch::async, [] { bt::Engine::get().shutdown(); });
            for (int attempt = 0; attempt < 200 && !f.torrent->stopped; ++attempt) std::this_thread::sleep_for(1ms);
            check(f.torrent->stopped, "shutdown marks torrent stopped without waiting for disk lock");
            responsive_stats(f.torrent->hex);
            write_gate.release(); stopping.get();
            check(!f.torrent->have[0] && !f.torrent->have[1] && !f.torrent->ever[0] && f.torrent->disk_pending.empty(), "late and queued writes cannot publish into a stopped torrent");
            check(!bt::impl().disk_running && !bt::impl().disk_worker.joinable(), "shutdown drains and joins worker");
            struct stat replacement_stat{};
            check(fstat(fileno(replacement->cache), &replacement_stat) == 0 && replacement_stat.st_size == 0,
                  "old in-flight write cannot change the replacement cache file");
        }
        check(bt::payload_memory_bytes == 0, "cancelled work releases all payload memory");
        {
            Fixture f(root / "large-ram-tail", 2, 64ll << 20, 4);
            fail_write_fd = fileno(f.torrent->cache);
            f.deliver(0); f.idle(); fail_write_fd = -1;
            int auxiliary = -1; int64_t total = 0; std::string error;
            check(bool(bt::Engine::get().open_reader(f.torrent->hex, 0, &auxiliary, &total, &error, bt::ReaderRole::Auxiliary)),
                  "large-piece fallback opens auxiliary reader beside the held download head");
            {
                std::lock_guard<std::mutex> lock(bt::impl().mu);
                f.torrent->readers[auxiliary] = 3 * f.torrent->plen;
            }
            f.deliver(3); f.idle();
            uint8_t bytes[16]{};
            check(bt::Engine::get().read(f.torrent, auxiliary, 0, 3 * f.torrent->plen, bytes, sizeof(bytes), nullptr) == int(sizeof(bytes)) && bytes[0] == 'P',
                  "maximum-sized tail piece is readable while the download head remains in bounded RAM");
            check(f.torrent->have[0] && f.torrent->have[3] && bt::payload_memory_bytes == (128ll << 20),
                  "two 64 MiB verified pieces coexist without exceeding the global payload cap");
            bt::Engine::get().close_reader(f.torrent, auxiliary);
            {
                std::lock_guard<std::mutex> lock(bt::impl().mu);
                check(bt::make_payload_room_locked(f.torrent.get(), f.torrent->plen) && f.torrent->have[0],
                      "closing the tail reader reclaims its cache buffer while preserving the download head");
            }
        }
        check(bt::payload_memory_bytes == 0, "large fallback reservations release after shutdown");
        {
            Fixture f(root / "large-download-tail", 2, 64ll << 20, 4, true);
            const int before = writes.load();
            f.deliver(0); f.idle();
            int auxiliary = -1; int64_t total = 0; std::string error;
            check(bool(bt::Engine::get().open_reader(f.torrent->hex, 0, &auxiliary, &total, &error, bt::ReaderRole::Auxiliary)),
                  "64 MiB download piece permits same-torrent auxiliary metadata");
            {
                std::lock_guard<std::mutex> lock(bt::impl().mu);
                f.torrent->readers[auxiliary] = 3 * f.torrent->plen;
            }
            f.deliver(3); f.idle();
            uint8_t bytes[16]{};
            check(bt::Engine::get().read(f.torrent, auxiliary, 0, 3 * f.torrent->plen, bytes, sizeof(bytes), nullptr) == int(sizeof(bytes)) && bytes[0] == 'P' &&
                  f.torrent->have[0] && bt::payload_memory_bytes == (128ll << 20) && writes == before,
                  "maximum-sized download head and tail share the hard 128 MiB budget without duplicate cache I/O");
            bt::Engine::get().close_reader(f.torrent, auxiliary);
        }
        check(bt::payload_memory_bytes == 0, "maximum-piece download handoff releases every reservation on shutdown");
        {
            Fixture f(root / "memory", 2, 64ll << 20, 4);
            std::lock_guard<std::mutex> lock(bt::impl().mu);
            auto* peer = f.torrent->peers[0].get(); peer->max_reqs = 9000; peer->reqq = 9000;
            bt::pick(f.torrent.get(), peer, now_seconds());
            check(f.torrent->active.size() == 2 && bt::payload_memory_bytes == (128ll << 20), "64 MiB pieces obey the 128 MiB aggregate budget without a four-piece exception");
        }
        check(bt::payload_memory_bytes == 0, "large piece reservations released");
        check(network_attempts == 0, "no socket, tracker or DNS operation was attempted");
        std::cout << "PASS: " << checks << " asynchronous disk worker assertions; pwrite=" << writes << " pread=" << reads << " network=" << network_attempts << '\n';
        return 0;
    } catch (const std::exception& error) {
        write_gate.release(); read_gate.release(); bt::Engine::get().shutdown();
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
