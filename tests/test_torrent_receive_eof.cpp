// Regression: process complete peer messages received before TCP EOF. Includes
// the real engine and loopback/network guard helpers used by the larger test.
#define main tcp_benchmark_unused_main
#include "test_torrent_network.cpp"
#undef main
#include <sys/ioctl.h>

int main(int argc, char** argv) {
	try {
		require(argc >= 2, "usage: recv-fin cache_dir [expect-old-loss]");
		const bool expect_old_loss = argc > 2;
		const fs::path directory = argv[1]; fs::create_directories(directory);
		const std::string payload(65536, 'F');
		BValue info = BValue::dict();
		info.d["name"] = BValue(std::string("local-eof-fixture.mp4"));
		info.d["length"] = BValue(int64_t(payload.size()));
		info.d["piece length"] = BValue(int64_t(payload.size()));
		info.d["pieces"] = BValue(digest(payload));
		auto torrent = std::make_shared<bt::Torrent>();
		torrent->hex = hex(digest(bencode(info)));
		auto& impl = bt::impl(); impl.data_dir = directory.string(); impl.cache_bytes = 1 << 20;
		require(bt::parse_info(torrent.get(), bencode(info)), "real metadata parser");
		impl.torrents.push_back(torrent);
		int reader_id = -1; int64_t file_size = 0; std::string error;
		auto reader = bt::Engine::get().open_reader(torrent->hex, 0, &reader_id, &file_size, &error, bt::ReaderRole::Download);
		require(reader && file_size == 65536, "real download reader");
		#ifdef BT_ASYNC_DISK_WORKER
		{ std::lock_guard<std::mutex> lock(impl.mu); bt::start_disk_worker_locked(); }
		#endif
		StopEngine cleanup;
		const int listener = socket(AF_INET, SOCK_STREAM, 0);
		require(listener >= 0, "listener socket");
		sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 1) == 0,
		        "loopback listener");
		socklen_t length = sizeof(address);
		require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0, "listener address");
		const int receiver = socket(AF_INET, SOCK_STREAM, 0);
		require(receiver >= 0 && connect(receiver, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "real TCP connect");
		const int sender = accept(listener, nullptr, nullptr);
		require(sender >= 0, "real TCP accept");
		close(listener);
		bt::prepare_peer_socket(receiver);
		auto peer = std::make_unique<bt::Peer>();
		peer->fd = receiver; peer->state = bt::Peer::Active; peer->peer_choking = false;
		peer->has.assign(1, 1); peer->has_count = 1; peer->max_reqs = 32;
		bt::Peer* p = peer.get(); torrent->peers.push_back(std::move(peer));
		bt::pick(torrent.get(), p, now_seconds());
		require(p->reqs.size() == 4, "production picker made all four block requests");
		std::string wire;
		for (const auto& request : p->reqs) {
			bt::put32(wire, request.len + 9); wire += char(7);
			bt::put32(wire, request.piece); bt::put32(wire, request.begin);
			wire.append(payload.data() + request.begin, request.len);
		}
		// Valid keep-alives make the wire stream an exact multiple of recv's
		// 64 KiB buffer. Both full recv calls therefore precede the EOF call.
		while (wire.size() < (128u << 10)) bt::put32(wire, 0);
		require(wire.size() == (128u << 10), "whole complete framed stream");
		for (size_t offset = 0; offset < wire.size();) {
			ssize_t n = send(sender, wire.data() + offset, wire.size() - offset, MSG_NOSIGNAL);
			require(n > 0, "write real framed bytes"); offset += size_t(n);
		}
		require(shutdown(sender, SHUT_WR) == 0, "TCP FIN after framed bytes");
		int available = 0;
		const double until = now_seconds() + 5;
		do {
			require(ioctl(receiver, FIONREAD, &available) == 0, "read kernel pending byte count");
			if (available == int(wire.size())) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (now_seconds() < until);
		require(available == int(wire.size()), "all 128 KiB are kernel-buffered before production recv");
		{
			std::lock_guard<std::mutex> lock(impl.mu);
			bt::on_readable(torrent.get(), p, now_seconds());
		}
		bool verified = false;
		{
			std::unique_lock<std::mutex> lock(impl.mu);
			impl.cv.wait_for(lock, std::chrono::seconds(2), [&] { return torrent->have[0] != 0; });
			verified = torrent->have[0];
		}
		bool exact = false;
		if (verified) {
			std::string received(payload.size(), '\0');
			const int n = bt::Engine::get().read(reader, reader_id, 0, 0,
			                                   reinterpret_cast<uint8_t*>(received.data()), int(received.size()), nullptr);
			exact = n == int(payload.size()) && received == payload;
		}
		const bool pass = expect_old_loss ? (!verified && p->in.size() == wire.size() && p->in_off == 0) : verified && exact;
		json result{{"result", pass ? "PASS" : "FAIL"}, {"expected_old_loss", expect_old_loss},
		            {"kernel_buffered_bytes", available}, {"payload_bytes", payload.size()}, {"verified", verified},
		            {"exact", exact}, {"unparsed_bytes", p->in.size() - p->in_off}, {"peer_closed", p->dead},
		            {"external_attempts", external_attempts.load()}};
		bt::Engine::get().close_reader(reader, reader_id); bt::Engine::get().shutdown();
		close(sender); close(receiver); torrent->peers[0]->fd = -1;
		std::cout << result.dump() << '\n';
		return pass && !external_attempts ? 0 : 1;
	} catch (const std::exception& error) {
		std::cerr << "FAIL: " << error.what() << '\n'; return 1;
	}
}
