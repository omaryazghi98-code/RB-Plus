// Deterministic peer-policy tests against an actual engine implementation.
// Connections and useful-payload rates in the discovery scenario are simulated;
// block-credit, request release and SHA-1 tests use the production functions.
#ifndef ENGINE_IMPLEMENTATION
#define ENGINE_IMPLEMENTATION "../src/torrent/engine.cpp"
#endif
#include ENGINE_IMPLEMENTATION

#include <iostream>
#include <stdexcept>

namespace {
int checks = 0, network_attempts = 0;
void check(bool condition, const char* message) {
	++checks;
	if (!condition) throw std::runtime_error(message);
}
struct Fixture {
	std::shared_ptr<bt::Torrent> t = std::make_shared<bt::Torrent>();
	std::string piece;
	bool worker_started = false;
	Fixture(int count = 80, int pieces = 512, int slots = 256) {
		piece.resize(65536);
		for (size_t i = 0; i < piece.size(); ++i) piece[i] = char((i * 31 + i / 997) & 255);
		unsigned char digest[20];
		SHA1(reinterpret_cast<const unsigned char*>(piece.data()), piece.size(), digest);
		t->has_meta = true; t->plen = int64_t(piece.size()); t->npieces = pieces; t->total = t->plen * pieces;
		t->files = {{"synthetic.bin", t->total, 0}}; t->file = -1;
		t->readers[1] = 0; t->reader_files[1] = 0; t->download_readers.insert(1);
		t->started = t->last_reader = 100; t->rate_at = 99;
		t->have.resize(size_t(pieces)); t->ever.resize(size_t(pieces)); t->slot_of.assign(size_t(pieces), -1);
		t->nslots = slots; t->ram.resize(size_t(slots)); t->piece_in_slot.assign(size_t(slots), -1);
		t->slot_touch.resize(size_t(slots));
		for (int i = 0; i < pieces; ++i) t->hashes.append(reinterpret_cast<char*>(digest), 20);
		for (int i = 0; i < count; ++i) add_peer(i + 1, 100);
		bt::refresh_wanted(t.get());
	}
	void start_worker() {
#ifdef BT_ASYNC_DISK_WORKER
		std::lock_guard<std::mutex> lock(bt::impl().mu);
		bt::impl().torrents.push_back(t);
		bt::ensure_slot_state(t.get());
		bt::start_disk_worker_locked();
		worker_started = true;
#endif
	}
	void drain_worker() {
#ifdef BT_ASYNC_DISK_WORKER
		if (worker_started) bt::wait_for_disk_idle();
#endif
	}
	~Fixture() {
#ifdef BT_ASYNC_DISK_WORKER
		if (worker_started) {
			bt::Engine::get().shutdown();
			std::lock_guard<std::mutex> lock(bt::impl().mu);
			bt::impl().torrents.clear();
		}
#endif
	}
	bt::Peer* add_peer(int id, double now) {
		auto peer = std::make_unique<bt::Peer>();
		peer->key = uint64_t(id) << 16; peer->state = bt::Peer::Active;
		peer->peer_choking = false; peer->am_interested = true;
		peer->since = peer->last_data = peer->last_send = now;
		peer->has.assign(size_t(t->npieces), 1); peer->has_count = t->npieces;
		peer->reqq = 250; peer->max_reqs = 32;
		t->known[peer->key].connected = true;
		auto* result = peer.get(); t->peers.push_back(std::move(peer)); return result;
	}
	void candidates(int count) {
		for (int i = 0; i < count; ++i) {
			sockaddr_in address{}; address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(uint32_t(1001 + i)); address.sin_port = 0;
			bt::add_candidates(t.get(), {address});
		}
	}
	void rates(double now, bool healthy = false) {
		for (auto& peer : t->peers) {
			peer->last_data = now;
			const int64_t rate = healthy || (peer->key >> 16) >= 1000 ? 4ll << 20 : 8ll << 10;
			peer->bytes_window = rate;
#ifdef TURNOVER_NEW
			peer->useful_window = rate;
			peer->payload_at = now;
#endif
			// The policy fixture simulates continued progress without needing to
			// allocate hundreds of seconds of media. Keep outstanding work live.
			for (auto& request : peer->reqs) request.at = now;
		}
	}
	void ledger() {
		for (const auto& [index, piece_state] : t->active) {
			std::vector<unsigned> owners(piece_state.asked.size());
			for (const auto& peer : t->peers) if (!peer->dead)
				for (const auto& request : peer->reqs)
					if (request.piece == uint32_t(index)) ++owners[request.begin / bt::kBlock];
			for (size_t block = 0; block < owners.size(); ++block)
				check(owners[block] == piece_state.asked[block], "request reservations match only live owners");
		}
	}
};

struct Discovery { int replacements = 0, admitted_fast = 0, released_blocks = 0; double first = 0; };
Discovery discovery_case() {
	Fixture f; f.candidates(8); Discovery outcome;
	for (int second = 100; second <= 380; ++second) {
		const double now = second;
		f.rates(now);
		std::map<uint64_t, std::vector<bt::Peer::Req>> before;
		for (const auto& peer : f.t->peers) before[peer->key] = peer->reqs;
		bt::upkeep(f.t.get(), now);
		std::vector<bt::Peer::Req> released;
		for (const auto& peer : f.t->peers) if (peer->dead) {
			++outcome.replacements;
			if (!outcome.first) outcome.first = now - 100;
			check((peer->key >> 16) < 1000, "fast replacement peers are never discarded");
			check(!before[peer->key].empty(), "retired slow peer owned real pending requests");
			check(peer->reqs.empty(), "retiring a slow peer releases every request");
			released = before[peer->key]; outcome.released_blocks += int(released.size());
			bt::close_peer(f.t.get(), peer.get(), now);
#ifdef TURNOVER_NEW
			check(f.t->known[peer->key].retry_at >= now + 180, "retired peer cannot immediately reconnect");
#endif
		}
		if (!released.empty()) {
			f.t->peers.erase(std::remove_if(f.t->peers.begin(), f.t->peers.end(),
			                              [](const auto& peer) { return peer->dead; }), f.t->peers.end());
			f.ledger();
			check(!f.t->candidates.empty(), "a ready replacement exists before retirement");
			const auto address = f.t->candidates.front(); f.t->candidates.pop_front();
			// Emulate a successful connection to that queued seed. Network tests
			// elsewhere cover TCP; this suite isolates discovery policy.
			auto* fast = f.add_peer(int(ntohl(address.sin_addr.s_addr)), now);
			fast->max_reqs = 250; ++outcome.admitted_fast;
			bt::pick(f.t.get(), fast, now);
			check(std::any_of(released.begin(), released.end(), [&](const auto& old) {
				return std::any_of(fast->reqs.begin(), fast->reqs.end(), [&](const auto& next) {
					return old.piece == next.piece && old.begin == next.begin;
				});
			}), "new seed can immediately request work released by slow peer");
			f.ledger();
		}
		check(f.t->peers.size() == 80, "turnover preserves established connection budget");
		for (auto& peer : f.t->peers) peer->out.clear();
	}
#ifdef TURNOVER_NEW
	check(outcome.replacements == 8 && outcome.admitted_fast == 8,
	      "all eight late fast seeds get an opportunity within 280 simulated seconds");
	check(outcome.first >= 60, "first established peer gets full observation grace");
#else
	check(outcome.replacements == 0 && outcome.admitted_fast == 0,
	      "baseline full slow set never explores queued fast alternatives");
#endif
	return outcome;
}

#ifdef TURNOVER_NEW
int count_dead(Fixture& f) {
	return int(std::count_if(f.t->peers.begin(), f.t->peers.end(), [](const auto& peer) { return peer->dead; }));
}
void prepare(Fixture& f, double now, bool healthy = false) {
	f.rates(now, healthy); f.t->rate_at = now - 1;
	for (auto& peer : f.t->peers) {
		peer->useful_rate = healthy ? 4ll << 20 : 8ll << 10;
		bt::pick(f.t.get(), peer.get(), now);
	}
}
void protections() {
	{
		Fixture f; f.candidates(8); prepare(f, 200, true); bt::upkeep(f.t.get(), 200);
		check(count_dead(f) == 0, "healthy fast peers are never dropped for exploration");
	}
	{
		Fixture f; prepare(f, 200); bt::upkeep(f.t.get(), 200);
		check(count_dead(f) == 0, "empty candidate set causes no churn");
		check(f.t->want_peers, "full slow set continues peer discovery");
	}
	{
		Fixture f; f.candidates(1); prepare(f, 200);
		const auto key = bt::addr_key(f.t->candidates.front()); f.t->known[key].retry_at = 250;
		bt::upkeep(f.t.get(), 200);
		check(count_dead(f) == 0, "backoff-only candidates cannot trigger churn");
	}
	{
		Fixture f; f.candidates(1); prepare(f, 200);
		const auto key = bt::addr_key(f.t->candidates.front()); f.t->banned.insert(bt::key_ip(key));
		bt::upkeep(f.t.get(), 200);
		check(count_dead(f) == 0, "banned candidates cannot trigger churn");
	}
	{
		Fixture f; f.candidates(8); prepare(f, 200);
		for (auto& peer : f.t->peers) peer->since = 190;
		bt::upkeep(f.t.get(), 200);
		check(count_dead(f) == 0, "recent established peers retain observation grace");
	}
	{
		Fixture f; f.candidates(8); prepare(f, 200);
		f.t->peers.back()->state = bt::Peer::Handshaking; f.t->peers.back()->since = 199;
		bt::upkeep(f.t.get(), 200);
		check(count_dead(f) == 0, "79 established plus one recent handshake does not trigger turnover");
	}
	{
		Fixture f; f.candidates(8); prepare(f, 200, true);
		auto* rare = f.t->peers[0].get(); rare->useful_rate = 0; rare->useful_window = 0;
		for (size_t i = 1; i < f.t->peers.size(); ++i) {
			f.t->peers[i]->has[0] = 0; --f.t->peers[i]->has_count;
		}
		check(bt::uniquely_useful_peer(f.t.get(), rare), "rarity uses bitfield availability, not request ownership");
		bt::upkeep(f.t.get(), 200);
		check(count_dead(f) == 0 && !rare->dead, "only copy of wanted piece remains connected");
	}
	{
		Fixture f; f.candidates(8); prepare(f, 200, true);
		auto* idle = f.t->peers[0].get(); bt::release_all(f.t.get(), idle);
		idle->useful_rate = 0; idle->useful_window = 0;
		// Direct policy call ensures no new request is assigned between the
		// fixture's no-opportunity condition and the decision under test.
		bt::turnover_peer(f.t.get(), 200);
		check(count_dead(f) == 0, "unchoked peer without work is not punished for picker starvation");
	}
	{
		Fixture f; f.candidates(8); prepare(f, 200); f.t->download_readers.clear();
		bt::upkeep(f.t.get(), 200);
		check(count_dead(f) == 0, "playback-only torrent keeps existing turnover behavior");
	}
	{
		Fixture f; f.candidates(8); prepare(f, 200);
		bt::turnover_peer(f.t.get(), 200); const int first = count_dead(f);
		bt::turnover_peer(f.t.get(), 201); bt::turnover_peer(f.t.get(), 229.99);
		check(first == 1 && count_dead(f) == 1, "only one peer may be retired in a 30-second interval");
	}
	{
		Fixture f; prepare(f, 200, true); bt::upkeep(f.t.get(), 200);
		check(!f.t->want_peers, "healthy full set does not force frequent tracker announces");
		const auto health = bt::peer_health(f.t.get(), 200);
		check(health.unchoked == 80 && health.productive == 80, "diagnostics count productive and unchoked peers");
	}
	{
		Fixture f; f.candidates(4);
		for (auto& peer : f.t->peers) peer->max_reqs = 8;
		prepare(f, 200);
		for (size_t index = 0; index < 34; ++index) {
			auto* idle = f.t->peers[index].get(); bt::release_all(f.t.get(), idle);
			idle->useful_rate = 0; idle->useful_window = 0;
		}
		for (size_t index = 34; index < 79; ++index)
			check(f.t->peers[index]->reqs.size() == 8, "45 slow peers hold real requests beside 34 idle peers");
		auto* moderate = f.t->peers.back().get();
		moderate->useful_rate = 128 * 1024; moderate->useful_window = 128 * 1024;
		bt::turnover_peer(f.t.get(), 200);
		check(count_dead(f) == 1, "34 idle peers cannot hide eligible slow senders after a moderate peer arrives");
		check(std::none_of(f.t->peers.begin(), f.t->peers.begin() + 34,
		                  [](const auto& peer) { return peer->dead; }) && !moderate->dead,
		      "eligibility filter preserves idle peers and the moderate sender while replacing a slower owner");
		f.ledger();
	}
	{
		Fixture f; f.candidates(4);
		for (auto& peer : f.t->peers) peer->max_reqs = 8;
		prepare(f, 200);
		for (size_t index = 0; index < 34; ++index) {
			auto* idle = f.t->peers[index].get(); bt::release_all(f.t.get(), idle);
			idle->useful_rate = 0; idle->useful_window = 0;
		}
		bt::turnover_peer(f.t.get(), 200);
		check(count_dead(f) == 1 && f.t->peers.front()->dead,
		      "an entirely unproductive pool yields its first old idle slot for exploration");
		f.ledger();
	}
}

void payload_credit() {
	Fixture f(1, 4, 8); auto* peer = f.t->peers[0].get(); peer->max_reqs = 4;
	bt::pick(f.t.get(), peer, 100); auto requests = peer->reqs;
	const auto first = requests.front();
	bt::on_block(f.t.get(), peer, first.piece, first.begin, f.piece.data(), first.len, 100.1);
	check(peer->useful_window == 16384 && peer->payload_at == 100.1, "new valid block earns useful credit");
	bt::on_block(f.t.get(), peer, first.piece, first.begin, f.piece.data(), first.len, 101);
	check(peer->useful_window == 16384 && peer->payload_at == 100.1, "duplicate does not refresh useful credit or last payload");
	const auto malformed = requests[1];
	bt::on_block(f.t.get(), peer, malformed.piece, malformed.begin, f.piece.data(), 1, 102);
	check(peer->useful_window == 16384 && peer->payload_at == 100.1, "malformed block earns no useful credit");
	peer->in.assign(4, '\0'); bt::process_input(f.t.get(), peer, 103);
	check(peer->last_data == 103 && peer->payload_at == 100.1 && peer->useful_window == 16384,
	      "keepalive keeps protocol alive without disguising payload stall");
	bt::pick(f.t.get(), peer, 104); requests = peer->reqs;
	std::string corrupt = f.piece; corrupt[malformed.begin] ^= 1;
	f.start_worker();
	{
		std::lock_guard<std::mutex> lock(bt::impl().mu);
		for (const auto& request : requests)
			bt::on_block(f.t.get(), peer, request.piece, request.begin, corrupt.data() + request.begin, request.len, 104);
	}
	f.drain_worker();
	check(!f.t->have[0] && peer->dead && f.t->banned.count(bt::key_ip(peer->key)),
	      "payload rate credit cannot bypass SHA-1 rejection or corrupt-peer ban");
}
#endif
}

extern "C" int __wrap_socket(int, int, int) { ++network_attempts; errno = ENETUNREACH; return -1; }
extern "C" int __wrap_connect(int, const sockaddr*, socklen_t) { ++network_attempts; errno = ENETUNREACH; return -1; }
extern "C" int __wrap_getaddrinfo(const char*, const char*, const addrinfo*, addrinfo**) { ++network_attempts; return EAI_FAIL; }

int main() {
	try {
		const auto result = discovery_case();
#ifdef TURNOVER_NEW
		protections(); payload_credit();
#endif
#ifdef BT_ASYNC_DISK_WORKER
		check(bt::payload_memory_bytes == 0, "all payload reservations are released after policy and SHA tests");
#endif
		check(network_attempts == 0, "all tests run without network access");
		std::cout << "RESULT {\"result\":\"PASS\",\"checks\":" << checks
		          << ",\"network_attempts\":" << network_attempts
		          << ",\"slow_peers_initial\":80,\"fast_candidates_late\":8,\"simulated_seconds\":280"
		          << ",\"replacements\":" << result.replacements << ",\"fast_admitted\":" << result.admitted_fast
		          << ",\"released_pending_blocks\":" << result.released_blocks
		          << ",\"first_replacement_after_s\":" << result.first << "}\n";
		return 0;
	} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
