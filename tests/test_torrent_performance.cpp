// Controlled scheduler/peer-payload benchmark. Uses the production picker,
// request parser, verifier and cache. It does not contact a real swarm and its
// virtual link rates are not PS5 measurements.
#ifndef ENGINE_IMPLEMENTATION
#define ENGINE_IMPLEMENTATION "../src/torrent/engine.cpp"
#endif
#include ENGINE_IMPLEMENTATION

#include <chrono>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <tuple>

namespace {
int checks = 0;
int network_attempts = 0;
// Configure the rolling cache explicitly; standalone download readers use the
// engine's bounded memory handoff. RAM modes also cover unavailable storage.
enum class CacheMode { Disk, LegacyRam, BoundedRam };
CacheMode cache_mode = CacheMode::Disk;
void check(bool condition, const char* message) {
	++checks;
	if (!condition) throw std::runtime_error(message);
}

// This fixture advances virtual time itself. Run the actual disk operation
// deterministically between simulated responses; real worker scheduling and
// lock concurrency are exercised by test_torrent_disk_worker and the TCP suite.
void drain_disk_jobs() {
#ifdef BT_ASYNC_DISK_WORKER
	for (;;) {
		bt::DiskJob job;
		{
			std::lock_guard<std::mutex> lock(bt::impl().mu);
			if (bt::impl().disk_jobs.empty()) break;
			job = std::move(bt::impl().disk_jobs.front());
			bt::impl().disk_jobs.pop_front();
		}
		bt::process_disk_job(std::move(job));
	}
#endif
}

struct Fixture {
	std::shared_ptr<bt::Torrent> torrent = std::make_shared<bt::Torrent>();
	std::string piece;
	Fixture(int peers, int64_t length, int pieces, int slots, int queue = 250) {
		piece.resize(size_t(length));
		for (size_t i = 0; i < piece.size(); ++i) piece[i] = char((i * 31 + i / 997) & 255);
		unsigned char digest[20];
		SHA1(reinterpret_cast<const unsigned char*>(piece.data()), piece.size(), digest);
		torrent->has_meta = true; torrent->plen = length;
		torrent->npieces = pieces; torrent->total = pieces * length;
		torrent->files = {{"synthetic.bin", torrent->total, 0}};
		torrent->file = -1;
		torrent->readers[1] = 0; torrent->reader_files[1] = 0; torrent->download_readers.insert(1);
		torrent->started = torrent->last_reader = 100;
		torrent->have.resize(size_t(pieces)); torrent->ever.resize(size_t(pieces));
		torrent->slot_of.assign(size_t(pieces), -1);
		if (cache_mode == CacheMode::BoundedRam) slots = std::max(1, std::min(slots, int((64ll << 20) / length)));
        torrent->nslots = slots; torrent->ram.resize(size_t(slots));
        if (cache_mode == CacheMode::Disk) {
            torrent->cache = tmpfile();
            check(torrent->cache != nullptr, "both compared engines have the same configured rolling-cache capacity");
        }
		torrent->piece_in_slot.assign(size_t(slots), -1); torrent->slot_touch.resize(size_t(slots));
		for (int i = 0; i < pieces; ++i) torrent->hashes.append(reinterpret_cast<char*>(digest), 20);
		for (int i = 0; i < peers; ++i) {
			auto peer = std::make_unique<bt::Peer>();
			peer->key = uint64_t(i + 1) << 16;
			peer->state = bt::Peer::Active; peer->peer_choking = false;
			peer->since = peer->last_data = peer->last_send = 100;
			peer->has.assign(size_t(pieces), 1); peer->has_count = pieces;
			peer->reqq = queue;
			torrent->peers.push_back(std::move(peer));
		}
		bt::refresh_wanted(torrent.get());
	}
	void advance(int& contiguous) {
		drain_disk_jobs();
		while (contiguous < torrent->npieces && torrent->have[size_t(contiguous)]) ++contiguous;
		const int64_t position = int64_t(contiguous) * torrent->plen;
		if (torrent->readers[1] != position) {
			torrent->readers[1] = position;
			torrent->wanted_dirty = true;
		}
	}
	~Fixture() { drain_disk_jobs(); }
};

double cpu_case() {
	Fixture f(37, 8ll << 20, 32, 32);
	for (auto& peer : f.torrent->peers) peer->max_reqs = 125;
	const auto started = std::chrono::steady_clock::now();
	int contiguous = 0;
	for (auto& peer : f.torrent->peers) bt::pick(f.torrent.get(), peer.get(), 100);
	while (contiguous < f.torrent->npieces) {
		bool progress = false;
		for (auto& peer : f.torrent->peers) {
			if (peer->reqs.empty()) bt::pick(f.torrent.get(), peer.get(), 100);
			if (peer->reqs.empty()) continue;
			const auto request = peer->reqs.front();
			bt::on_block(f.torrent.get(), peer.get(), request.piece, request.begin,
			             f.piece.data() + request.begin, request.len, 100);
			bt::pick(f.torrent.get(), peer.get(), 100);
			peer->out.clear();
			f.advance(contiguous); progress = true;
		}
		check(progress, "CPU fixture never stalls before all valid pieces complete");
	}
	check(std::all_of(f.torrent->ever.begin(), f.torrent->ever.end(), [](uint8_t value) { return value != 0; }),
	      "CPU benchmark data passes real SHA-1 verification");
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

using Key = std::tuple<int, uint32_t, uint32_t>;
struct Event {
	double due;
	uint64_t generation;
	int peer;
	bt::Peer::Req request;
	bool operator<(const Event& other) const { return due > other.due; }
};
struct Outcome { int64_t contiguous = 0, verified = 0, received = 0; double first_piece = 0; };

Outcome virtual_link_case(bool slow_head) {
	Fixture f(8, 8ll << 20, 256, 64, 1000);
	std::priority_queue<Event> events;
	std::map<Key, uint64_t> pending;
	std::vector<double> last_send(8, 101.25);  // a peer startup gap, identical for both versions
	uint64_t generation = 0;
	int contiguous = 0;
	Outcome result;
	double now = 100, next_upkeep = 100;
	auto flush = [&] {
		for (size_t i = 0; i < f.torrent->peers.size(); ++i) {
			auto& peer = *f.torrent->peers[i];
			for (size_t pos = 0; pos + 4 <= peer.out.size();) {
				const uint32_t length = bt::be32(peer.out.data() + pos);
				check(pos + 4 + length <= peer.out.size(), "real picker queues complete wire frames");
				const char* payload = peer.out.data() + pos + 4;
				if (length == 13 && (payload[0] == 6 || payload[0] == 8)) {
					bt::Peer::Req request{bt::be32(payload + 1), bt::be32(payload + 5), bt::be32(payload + 9), now};
					const Key key{int(i), request.piece, request.begin};
					if (payload[0] == 8) pending.erase(key);
					else {
						const double rate = (slow_head && i < 2 ? 0.2 : 12.0) * (1 << 20);
						const double due = std::max(now + 0.3, last_send[i]) + request.len / rate;
						last_send[i] = due;
						pending[key] = ++generation;
						events.push({due, generation, int(i), request});
					}
				}
				pos += 4 + length;
			}
			peer.out.clear();
		}
	};
	while (now < 120) {
		if (now >= next_upkeep) {
			bt::upkeep(f.torrent.get(), now); next_upkeep += 0.25; flush();
		}
		if (events.empty() || events.top().due > next_upkeep) { now = next_upkeep; continue; }
		const auto event = events.top(); events.pop(); now = event.due;
		if (now > 120) break;
		const Key key{event.peer, event.request.piece, event.request.begin};
		auto existing = pending.find(key);
		if (existing == pending.end() || existing->second != event.generation) continue;
		pending.erase(existing);
		auto* peer = f.torrent->peers[size_t(event.peer)].get();
		const auto& request = event.request;
		bt::on_block(f.torrent.get(), peer, request.piece, request.begin,
		             f.piece.data() + request.begin, request.len, now);
		peer->last_data = now;
		result.received += request.len;
		f.advance(contiguous);
		if (!result.first_piece && contiguous) result.first_piece = now - 100;
		bt::pick(f.torrent.get(), peer, now); flush();
	}
	result.contiguous = int64_t(contiguous) * f.torrent->plen;
	for (uint8_t have : f.torrent->ever) if (have) result.verified += f.torrent->plen;
	check(!bt::impl().running && !bt::impl().net.joinable(), "benchmark does not start the network thread");
	check(result.received > 0 && result.verified > 0, "virtual peer responses reach real verified pieces");
	return result;
}

#ifdef PERFORMANCE_NEW
void regressions() {
	auto& engine = bt::Engine::get();
	check(bt::impl().speed_profile == bt::SpeedProfile::UltraFast, "Ultra fast is the native default");
	for (const auto profile : {bt::SpeedProfile::Balanced, bt::SpeedProfile::Fast, bt::SpeedProfile::UltraFast}) {
		engine.set_speed_profile(profile);
		Fixture f(1, 1ll << 20, 512, 64, 1000);
		auto* peer = f.torrent->peers[0].get();
		bt::upkeep(f.torrent.get(), 100);
		check(peer->max_reqs >= bt::speed_limits().floor, "quiet startup preserves the selected minimum pipeline");
		peer->bytes_window = 100ll << 20;
		bt::upkeep(f.torrent.get(), 101);
		check(peer->max_reqs == bt::speed_limits().requests, "fast peer uses the selected bounded request capacity");
		bt::upkeep(f.torrent.get(), 102);
		check(peer->max_reqs == bt::speed_limits().requests, "one quiet second does not collapse a fast pipeline");
		peer->reqq = 20;
		bt::upkeep(f.torrent.get(), 103);
		check(peer->max_reqs <= 20, "peer advertised request limit is always respected");
	}
	engine.set_speed_profile(static_cast<bt::SpeedProfile>(999));
	check(bt::impl().speed_profile == bt::SpeedProfile::UltraFast, "invalid persisted profile safely selects Ultra fast");
	{
		Fixture f(1, 1ll << 20, 512, 64, 1000);
		auto* peer = f.torrent->peers[0].get();
		peer->pipeline_rate = 32ll << 20;
		peer->max_reqs = bt::speed_limits().requests;
		f.torrent->wanted.clear();
		f.torrent->wanted_dirty = false;
		f.torrent->rate_at = 100;
		for (int second = 101; second <= 141; ++second) {
			peer->last_data = second;
			bt::upkeep(f.torrent.get(), second);
		}
		check(peer->reqs.empty() && peer->pipeline_rate == 32ll << 20,
		      "storage backpressure does not reduce an unmeasured peer's learned capacity");
		check(peer->max_reqs == bt::speed_limits().requests,
		      "a fast peer keeps its request window through a long local-write pause");
		f.torrent->wanted_dirty = true;
		bt::pick(f.torrent.get(), peer, 142);
		check(peer->reqs.size() == size_t(bt::speed_limits().requests),
		      "reopening the local write window immediately feeds the established fast peer");
	}
	{
		Fixture f(3, 1ll << 20, 512, 256);
		auto* first = f.torrent->peers[0].get();
		auto* second = f.torrent->peers[1].get();
		auto* third = f.torrent->peers[2].get();
		first->max_reqs = 128; first->reqq = 1000;
		bt::pick(f.torrent.get(), first, 100);
		second->max_reqs = 1; third->max_reqs = 1;
		bt::pick(f.torrent.get(), second, 100.5);
		check(second->reqs[0].piece >= 2, "fresh urgent blocks are not immediately duplicated");
		bt::release_all(f.torrent.get(), second);
		bt::pick(f.torrent.get(), second, 101.1);
		check(second->reqs[0].piece == 0, "overdue head is rescued before unused bulk blocks");
		const auto rescued = second->reqs[0];
		bt::pick(f.torrent.get(), third, 101.1);
		bool ledger_matches = true;
		for (const auto& [index, piece] : f.torrent->active) {
			for (size_t block = 0; block < piece.asked.size(); ++block) {
				unsigned owners = 0;
				for (const auto& owner : f.torrent->peers)
					for (const auto& request : owner->reqs)
						if (request.piece == uint32_t(index) && request.begin == block * bt::kBlock) ++owners;
				ledger_matches &= owners <= 2 && owners == piece.asked[block];
			}
		}
		check(ledger_matches, "every urgent block has at most two owners matching its reservation ledger");
		auto owns_rescued = [&](const bt::Peer* owner) {
			return std::any_of(owner->reqs.begin(), owner->reqs.end(), [&](const auto& request) {
				return request.piece == rescued.piece && request.begin == rescued.begin;
			});
		};
		bool cancel_sent = false;
		for (size_t pos = 0; pos + 4 <= first->out.size();) {
			const uint32_t length = bt::be32(first->out.data() + pos);
			if (pos + 4 + length > first->out.size()) break;
			const char* payload = first->out.data() + pos + 4;
			if (length == 13 && payload[0] == 8 && bt::be32(payload + 1) == rescued.piece &&
			    bt::be32(payload + 5) == rescued.begin) cancel_sent = true;
			pos += 4 + length;
		}
		check(!owns_rescued(third) || (!owns_rescued(first) && cancel_sent),
		      "a replacement for an existing duplicate cancels and removes the previous owner first");
		bt::on_block(f.torrent.get(), second, rescued.piece, rescued.begin,
		             f.piece.data() + rescued.begin, rescued.len, 101.2);
		check(std::none_of(f.torrent->peers.begin(), f.torrent->peers.end(), [&](const auto& owner) {
			return owns_rescued(owner.get());
		}) && f.torrent->active[rescued.piece].asked[rescued.begin / bt::kBlock] == 0,
		      "a valid duplicate response cancels all other owners and clears the reservation ledger");
		const auto before = f.torrent->unique_window;
		bt::on_block(f.torrent.get(), first, rescued.piece, rescued.begin,
		             f.piece.data() + rescued.begin, rescued.len, 101.3);
		check(f.torrent->unique_window == before && f.torrent->duplicate_bytes == rescued.len,
		      "late duplicate does not inflate useful download rate");
		const auto single_owner = std::find_if(first->reqs.begin(), first->reqs.end(), [&](const auto& request) {
			return f.torrent->active.at(int(request.piece)).asked[request.begin / bt::kBlock] == 1;
		});
		check(single_owner != first->reqs.end(), "cursor-rewind regression selects a request without another live copy");
		const auto released = *single_owner;
		bt::release_request(f.torrent.get(), released);
		first->reqs.erase(single_owner);
		first->max_reqs = int(first->reqs.size()) + 1;
		bt::pick(f.torrent.get(), first, 101.3);
		check(std::any_of(first->reqs.begin(), first->reqs.end(), [&](const auto& r) {
			return r.piece == released.piece && r.begin == released.begin;
		}), "released request rewinds the fast block cursor so it can be requested again");
	}
	{
		Fixture f(1, 65536, 4, 8);
		auto* peer = f.torrent->peers[0].get(); peer->max_reqs = 4;
		bt::pick(f.torrent.get(), peer, 100);
		const auto first = peer->reqs.front();
		const auto cursor = f.torrent->active[first.piece].next_unasked;
		bt::on_block(f.torrent.get(), peer, first.piece, first.begin, f.piece.data(), first.len, 100.1);
		check(f.torrent->active[first.piece].next_unasked == cursor,
		      "a valid response does not rewind the block cursor into already reserved work");
		const auto malformed = peer->reqs.front();
		bt::on_block(f.torrent.get(), peer, malformed.piece, malformed.begin, f.piece.data(), 1, 100.2);
		bt::pick(f.torrent.get(), peer, 100.2);
		check(std::any_of(peer->reqs.begin(), peer->reqs.end(), [&](const auto& r) {
			return r.piece == malformed.piece && r.begin == malformed.begin;
		}), "malformed response returns its reservation to the picker instead of losing the block");
	}
	{
		Fixture f(1, 65536, 4, 8);
		auto* peer = f.torrent->peers[0].get(); peer->max_reqs = 4;
		bt::pick(f.torrent.get(), peer, 100);
		std::string bad = f.piece; bad[0] ^= 1;
		const auto requests = peer->reqs;
		for (const auto& r : requests)
			bt::on_block(f.torrent.get(), peer, r.piece, r.begin, bad.data() + r.begin, r.len, 100.1);
		drain_disk_jobs();
		const auto& piece = f.torrent->active[0];
		check(!f.torrent->have[0] && piece.got_count == 0 && piece.next_unasked == 0 && peer->dead,
		      "SHA-1 failure resets the complete picker state and rejects the corrupt sender");
	}
	{
		Fixture f(1, 1ll << 20, 512, 256);
		auto* peer = f.torrent->peers[0].get(); peer->max_reqs = 64;
		bt::pick(f.torrent.get(), peer, 100);
		peer->pipeline_rate = 100ll << 20;
		bt::upkeep(f.torrent.get(), 116);
		check(peer->penalty_until > 116 && peer->max_reqs <= 32,
		      "timeout penalty is not undone by the once-per-second rate update");
	}
}
#endif
}

extern "C" int __wrap_socket(int, int, int) { ++network_attempts; errno = ENETUNREACH; return -1; }
extern "C" int __wrap_connect(int, const sockaddr*, socklen_t) { ++network_attempts; errno = ENETUNREACH; return -1; }
extern "C" int __wrap_getaddrinfo(const char*, const char*, const addrinfo*, addrinfo**) {
	++network_attempts; return EAI_FAIL;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "legacy-ram") cache_mode = CacheMode::LegacyRam;
    else if (argc > 1 && std::string(argv[1]) == "bounded-ram") cache_mode = CacheMode::BoundedRam;
    else if (argc > 1 && std::string(argv[1]) != "disk") return 2;
	try {
#ifdef PERFORMANCE_NEW
		regressions();
#endif
		const int regression_checks = checks;
		std::vector<double> timings;
		for (int i = 0; i < 3; ++i) timings.push_back(cpu_case());
		std::sort(timings.begin(), timings.end());
		const auto fast = virtual_link_case(false);
		const auto mixed = virtual_link_case(true);
		check(network_attempts == 0, "no socket, connect or DNS operation was attempted");
		std::cout << "RESULT {\"cache_mode\":\"" << (cache_mode == CacheMode::Disk ? "disk" : cache_mode == CacheMode::LegacyRam ? "legacy-ram" : "bounded-ram")
                  << "\",\"checks\":" << checks << ",\"regression_assertions\":" << regression_checks
		          << ",\"network_attempts\":" << network_attempts << ",\"cpu_256MiB_median_s\":" << timings[1]
		          << ",\"fast_contiguous_bytes\":" << fast.contiguous << ",\"fast_verified_bytes\":" << fast.verified
		          << ",\"fast_first_piece_s\":" << fast.first_piece
		          << ",\"mixed_contiguous_bytes\":" << mixed.contiguous << ",\"mixed_verified_bytes\":" << mixed.verified
		          << ",\"mixed_first_piece_s\":" << mixed.first_piece << "}\n";
		return 0;
	} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
