// The production Engine implementation is included to expose only its internal
// fixture boundary. No Engine method, scheduler, piece verifier, cache reader,
// transfer operation or filesystem operation is replaced in this test.
#include "../src/torrent/engine.cpp"
#include "download_transfer.h"
#include "growing_file_stream.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {
int checks = 0;
std::atomic<int> network_attempts{0};
std::atomic<int> media_syncs{0};

void check(bool condition, const std::string& message) {
	++checks;
	if (!condition) throw std::runtime_error(message);
}

std::string read_bytes(const fs::path& path) {
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input || input.tellg() < 0) return {};
	std::string bytes(size_t(input.tellg()), '\0'); input.seekg(0);
	input.read(bytes.data(), std::streamsize(bytes.size()));
	return input ? bytes : std::string{};
}

std::string sha1_bytes(const std::string& input) {
	unsigned char digest[20];
	SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
	return {reinterpret_cast<const char*>(digest), sizeof(digest)};
}

std::string hex_bytes(const std::string& input) {
	constexpr char digits[] = "0123456789abcdef";
	std::string output;
	for (unsigned char ch : input) { output += digits[ch >> 4]; output += digits[ch & 15]; }
	return output;
}

// A watchdog bounds failures of the real blocking reader, including the exact
// old defect (no requested pieces). It is not the transfer's success signal.
struct Watchdog {
	std::atomic<bool> timed_out{false};
	std::mutex mutex;
	std::condition_variable wake;
	bool finished = false;
	std::thread thread;
	explicit Watchdog(std::atomic<bool>& cancel, std::chrono::seconds timeout = 10s) : thread([this, &cancel, timeout] {
		std::unique_lock<std::mutex> lock(mutex);
		if (!wake.wait_for(lock, timeout, [this] { return finished; })) { timed_out = true; cancel = true; }
	}) {}
	~Watchdog() {
		{ std::lock_guard<std::mutex> lock(mutex); finished = true; }
		wake.notify_all(); thread.join();
	}
};

// This replaces only a remote peer's input bytes and the network event loop.
// Metadata comes from a synthetic, correctly hashed multi-file torrent. A
// synthetic peer advertises pieces, then the REAL picker issues block requests;
// the feeder answers only those requests through the REAL on_block verifier.
// Engine::read therefore blocks until an actually requested, SHA-1-verified
// piece reaches the rolling cache, just as it does in the application.
struct ControlledSwarm {
	static constexpr int64_t prefix_size = 81937;  // intentionally not piece aligned
	static constexpr int64_t default_piece_length = 65536;
	const int64_t piece_length;
	const int block_burst;
	std::shared_ptr<bt::Torrent> torrent;
	std::string payload;
	std::atomic<bool> finish{false};
	std::thread feeder;
	std::exception_ptr failure;
	int first_requested_piece = -1;
	int last_requested_piece = -1;
	int64_t served_bytes = 0;
	int verified_pieces = 0;
	bool selected_playback = false;
	bool shared_auxiliary_download = false;
	std::set<int> auxiliary_tail_pieces;

	ControlledSwarm(const fs::path& cache_dir, const std::string& media, int64_t resume = 0,
	                int64_t piece_bytes = default_piece_length, int burst = 1)
		: piece_length(piece_bytes), block_burst(burst) {
		fs::create_directories(cache_dir);
		auto& impl = bt::impl();
		std::lock_guard<std::mutex> lock(impl.mu);
		check(!impl.running && !impl.net.joinable() && impl.torrents.empty(), "each integration case starts with a cold engine");
		impl.data_dir = cache_dir.string();
		impl.cache_bytes = 8 * piece_length;  // smaller than the media: exercise eviction
		payload = std::string(size_t(prefix_size), 'P') + media + std::string(90123, 'T');
		std::string hashes;
		for (size_t offset = 0; offset < payload.size(); offset += size_t(piece_length))
			hashes += sha1_bytes(payload.substr(offset, size_t(piece_length)));
		BValue info = BValue::dict();
		info.d["name"] = BValue(std::string("offline-regression"));
		info.d["piece length"] = BValue(piece_length);
		info.d["pieces"] = BValue(hashes);
		BValue files = BValue::list();
		for (const auto& file : std::vector<std::pair<std::string, int64_t>>{
			{"unselected-before.bin", prefix_size}, {"episode.s02e03.mp4", int64_t(media.size())},
			{"unselected-after.bin", 90123}}) {
			BValue entry = BValue::dict(), path = BValue::list();
			entry.d["length"] = BValue(file.second);
			path.l.emplace_back(file.first); entry.d["path"] = std::move(path);
			files.l.push_back(std::move(entry));
		}
		info.d["files"] = std::move(files);
		const std::string metadata = bencode(info);
		torrent = std::make_shared<bt::Torrent>();
		torrent->hex = hex_bytes(sha1_bytes(metadata));
		check(bt::parse_info(torrent.get(), metadata), "production metadata parser accepts the controlled multi-file torrent");
		check(torrent->file == -1 && torrent->readers.empty(), "no movie was selected or opened before downloading");
		check(torrent->files[1].offset == prefix_size && torrent->cache != nullptr && torrent->nslots == 8,
		      "selected media starts at a nonzero offset and uses the real disk cache");
		auto peer = std::make_unique<bt::Peer>();
		peer->state = bt::Peer::Active;
		peer->peer_choking = false;
		peer->has.assign(size_t(torrent->npieces), 1);
		peer->has_count = torrent->npieces;
		peer->max_reqs = 8;
		torrent->peers.push_back(std::move(peer));
		impl.torrents.push_back(torrent);
		// start() reuses this fixture. Mark the event loop as supplied by the
		// feeder, so ensure_running() cannot create network/tracker threads.
		impl.running = true;
		bt::start_disk_worker_locked();
		feeder = std::thread([this, resume] {
			try {
				while (!finish.load()) {
					{
						std::lock_guard<std::mutex> guard(bt::impl().mu);
						selected_playback |= torrent->file != -1;
						shared_auxiliary_download |= !torrent->download_readers.empty() && !torrent->auxiliary_readers.empty();
						bool positioned = false;
						for (const auto& reader : torrent->readers) {
							const auto file = torrent->reader_files.find(reader.first);
							positioned |= file != torrent->reader_files.end() && file->second == 1 &&
							              reader.second >= prefix_size + resume;
						}
						if (positioned) {
							auto* peer = torrent->peers[0].get();
							for (int block = 0; block < block_burst; ++block) {
								bt::pick(torrent.get(), peer, now_seconds());
								if (!peer->reqs.empty()) {
									const auto request = peer->reqs.front();
									const int64_t offset = int64_t(request.piece) * piece_length + request.begin;
									if (offset < 0 || offset + request.len > int64_t(payload.size()))
										throw std::runtime_error("picker requested a block outside the synthetic torrent");
									if (first_requested_piece < 0) first_requested_piece = int(request.piece);
									last_requested_piece = std::max(last_requested_piece, int(request.piece));
									
									for (int id : torrent->auxiliary_readers) {
										const int64_t position = torrent->readers.at(id);
										const int64_t end = std::min<int64_t>(prefix_size + torrent->files[1].size, position + 65536);
										if (position > prefix_size + torrent->files[1].size / 2 && position < end &&
										    int64_t(request.piece) * piece_length < end && (int64_t(request.piece) + 1) * piece_length > position)
											auxiliary_tail_pieces.insert(int(request.piece));
									}
									bt::on_block(torrent.get(), peer, request.piece, request.begin,
									             payload.data() + offset, request.len, now_seconds());
									served_bytes += request.len;
									
								}
							}
							peer->out.clear();  // transport-only output is never sent to a socket
						}
					}
					std::this_thread::sleep_for(block_burst > 1 ? 1ms : 8ms);
				}
			} catch (...) {
				failure = std::current_exception();
				torrent->stopped = true; bt::impl().cv.notify_all();
			}
		});
	}

	void stop() {
		finish = true;
		if (feeder.joinable()) feeder.join();
		bt::wait_for_disk_idle();
		std::lock_guard<std::mutex> lock(bt::impl().mu);
		verified_pieces = int(std::count(torrent->ever.begin(), torrent->ever.end(), uint8_t(1)));
	}

	void verify_finished(int64_t media_size) {
		stop();
		if (failure) std::rethrow_exception(failure);
		std::lock_guard<std::mutex> lock(bt::impl().mu);
		bt::refresh_wanted(torrent.get());
		check(!selected_playback && torrent->file == -1, "download never assigns the selected playback file");
		check(torrent->readers.empty() && torrent->reader_files.empty() && torrent->download_readers.empty() &&
		      torrent->auxiliary_readers.empty(), "transfer closes its real engine reader on completion/cancellation");
		check(torrent->wanted.empty() && torrent->active.empty() && torrent->peers[0]->reqs.empty(),
		      "closing the final reader cancels outstanding block requests and releases active pieces");
		check(first_requested_piece >= int(prefix_size / piece_length) &&
		      last_requested_piece <= int((prefix_size + media_size - 1) / piece_length),
		      "only pieces intersecting the selected media file were requested");
		check(verified_pieces > 0 && served_bytes > 0, "the real peer verifier admitted downloaded pieces to the cache");
		check(!bt::impl().net.joinable() && torrent->tracker_threads.empty() && bt::impl().listen_fd == -1 &&
		      bt::impl().dht_fd == -1 && network_attempts.load() == 0,
		      "no socket, DNS lookup, tracker, DHT or network thread was used");
	}

	~ControlledSwarm() {
		stop();
        bt::stop_disk_worker();
		std::lock_guard<std::mutex> lock(bt::impl().mu);
		bt::impl().torrents.clear(); bt::impl().running = false;
	}
};

DownloadTransferRequest request_for(const fs::path& work_dir, const std::string& hash) {
	fs::create_directories(work_dir);
	DownloadTransferRequest request;
	request.work_dir = work_dir.string();
	request.partial_path = request.work_dir + "/media.part";
	request.checkpoint_path = request.work_dir + "/transfer.json";
	request.stream.kind = StreamKind::Torrent;
	request.stream.info_hash = hash;
	request.stream.file_idx = 1;
	request.season = 2; request.episode = 3;
	return request;
}

struct ProgressAudit {
	std::vector<DownloadTransferProgress> values;
	bool valid = true, observed_partial_file = false;
	void record(const DownloadTransferProgress& value, const std::string& path) {
		valid &= value.done >= 0 && value.total > 0 && value.done <= value.total &&
		         std::isfinite(value.bytes_per_second) && value.bytes_per_second >= 0 && value.torrent_file_idx == 1;
		if (!values.empty()) valid &= value.done >= values.back().done;
		values.push_back(value);
		if (value.done > 0 && value.done < value.total) {
			std::error_code error;
			const auto bytes = fs::file_size(path, error);
			observed_partial_file |= !error && bytes >= uint64_t(value.done);
		}
	}
};

void decode_active_torrent(const fs::path& root, const std::string& media) {
	ControlledSwarm swarm(root / "cache-progressive", media);
	auto request = request_for(root / "progressive", swarm.torrent->hex);
	auto state = std::make_shared<GrowingFileState>();
	std::atomic<bool> cancel{false}, held{false}, first_frame{false}, worker_done{false};
	std::atomic<int64_t> held_prefix{0};
	std::atomic<int> resolved_file{-1};
	Watchdog watchdog(cancel, 20s);
	DownloadTransferResult result;
	std::exception_ptr worker_failure;
	std::thread writer([&] {
		try {
			result = download_transfer(request, [&](const DownloadTransferProgress& value) {
				resolved_file = value.torrent_file_idx;
				state->publish({value.done, value.total, 1, GrowingFilePhase::Downloading});
				if (!held && progressive_download_threshold(value.done, value.total)) {
					held_prefix = value.done; held = true;
					// The actual transfer remains registered and the peer engine
					// keeps serving requests, but its sequential writer is held
					// until the decoder produces its first video frame.
					while (!first_frame && !cancel) std::this_thread::sleep_for(2ms);
				}
			}, cancel);
			if (result.status == DownloadTransferStatus::Complete) {
				fs::rename(request.partial_path, request.work_dir + "/media.mp4");
				state->publish({result.done, result.total, 1, GrowingFilePhase::Complete});
			} else state->publish({result.done, result.total, 1, GrowingFilePhase::Failed});
		} catch (...) { worker_failure = std::current_exception(); cancel = true; }
		worker_done = true;
	});
	AVIOContext* pb = nullptr;
	AVFormatContext* format = nullptr;
	AVCodecContext* codec = nullptr;
	AVPacket* packet = nullptr;
	AVFrame* frame = nullptr;
	auto cleanup = [&] {
		if (writer.joinable()) writer.join();
		av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&codec);
		if (format) avformat_close_input(&format);
		GrowingFileStream::close_avio(&pb);
	};
	try {
		while (!held && !worker_done && !cancel) std::this_thread::sleep_for(2ms);
		check(held && !cancel && !worker_done && progressive_download_threshold(held_prefix, media.size()) &&
		      held_prefix < int64_t(media.size()) / 5,
		      "real background torrent reaches a playable prefix from five percent and remains active");
		auto source = std::make_shared<GrowingFilePlayback>();
		source->path = request.partial_path;
		source->descriptor = ::open(request.partial_path.c_str(), O_RDONLY | O_NOFOLLOW);
		source->state = state; source->generation = 1; source->total = media.size();
		source->torrent_hash = swarm.torrent->hex; source->torrent_file_idx = resolved_file;
		check(source->descriptor >= 0 && resolved_file == 1, "progressive lease uses the real transfer's resolved torrent file");
		std::string error;
		pb = GrowingFileStream::open_avio(source, &cancel, nullptr, &error);
		check(pb && error.empty(), "production growing-file AVIO opens the real download prefix");
		format = avformat_alloc_context(); format->pb = pb; format->flags |= AVFMT_FLAG_CUSTOM_IO;
		format->probesize = 1 << 20; format->max_analyze_duration = AV_TIME_BASE / 2;
		check(avformat_open_input(&format, request.partial_path.c_str(), nullptr, nullptr) >= 0 &&
		      avformat_find_stream_info(format, nullptr) >= 0,
		      "FFmpeg reads unavailable MP4 tail metadata through the real shared torrent engine");
		{
			std::lock_guard<std::mutex> guard(bt::impl().mu);
			check(bt::impl().torrents.size() == 1 && bt::impl().torrents[0] == swarm.torrent &&
			      swarm.torrent->file == -1 && !swarm.torrent->stopped && swarm.torrent->download_readers.size() == 1 &&
			      swarm.torrent->auxiliary_readers.size() == 1 && swarm.shared_auxiliary_download,
			      "tail reader and downloader share the same running torrent/cache without selecting playback or stopping the download");
			check(!swarm.auxiliary_tail_pieces.empty() && swarm.auxiliary_tail_pieces.size() <= 2,
			      "MP4 tail metadata stays within one 64 KiB auxiliary window, allowing two whole SHA-verified boundary pieces");
		}
		GrowingFileStream::of(pb)->finish_opening();
		{
			std::lock_guard<std::mutex> guard(bt::impl().mu);
			check(swarm.torrent->auxiliary_readers.empty() && swarm.torrent->download_readers.size() == 1 &&
			      !swarm.torrent->stopped && swarm.torrent->file == -1,
			      "container opening releases only its auxiliary reader and leaves the real download running");
		}
		const AVCodec* decoder = nullptr;
		const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
		check(video >= 0 && decoder, "real progressive torrent has a video decoder");
		codec = avcodec_alloc_context3(decoder);
		check(codec && avcodec_parameters_to_context(codec, format->streams[video]->codecpar) >= 0 &&
		      avcodec_open2(codec, decoder, nullptr) >= 0, "real progressive torrent decoder opens");
		packet = av_packet_alloc(); frame = av_frame_alloc();
		int frames = 0;
		auto receive = [&] {
			for (;;) {
				const int code = avcodec_receive_frame(codec, frame);
				if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) break;
				if (code < 0) throw std::runtime_error("real progressive torrent decoder rejected media bytes");
				if (++frames == 1) {
					check(state->snapshot().available == held_prefix && !worker_done,
					      "first video frame decodes while the sequential download is held at its initial playable prefix");
					first_frame = true;
				}
				av_frame_unref(frame);
			}
		};
		int code = 0;
		while ((code = av_read_frame(format, packet)) >= 0) {
			if (packet->stream_index == video) {
				if (avcodec_send_packet(codec, packet) < 0) throw std::runtime_error("real progressive torrent packet decode failed");
				receive();
			}
			av_packet_unref(packet);
		}
		check(code == AVERROR_EOF && GrowingFileStream::of(pb)->failure().empty(),
		      "real progressive torrent reaches logical EOF across temporary prefix ends and final rename");
		avcodec_send_packet(codec, nullptr); receive();
		check(frames == 500 && first_frame && !watchdog.timed_out, "all 500 frames decode while the real torrent download continues");
		cleanup();
		if (worker_failure) std::rethrow_exception(worker_failure);
		check(result.status == DownloadTransferStatus::Complete && read_bytes(request.work_dir + "/media.mp4") == media &&
		      state->snapshot().phase == GrowingFilePhase::Complete,
		      "background download still finishes with exact selected bytes and a completed durable file");
		swarm.verify_finished(media.size());
		std::cout << "Progressive real-engine decode: frames=" << frames << " first_frame_prefix=" << held_prefix
		          << " total=" << media.size() << " percent=" << 100.0 * held_prefix / double(media.size())
		          << " auxiliary_tail_pieces=" << swarm.auxiliary_tail_pieces.size() << '\n';
	} catch (...) {
		cancel = true; first_frame = true;
		cleanup();
		throw;
	}
}
}  // namespace

// Any accidental attempt to create a network connection makes the final
// assertion fail and is rejected immediately. There are no external providers.
extern "C" int __wrap_socket(int, int, int) { ++network_attempts; errno = ENETUNREACH; return -1; }
extern "C" int __wrap_connect(int, const sockaddr*, socklen_t) { ++network_attempts; errno = ENETUNREACH; return -1; }
extern "C" int __wrap_getaddrinfo(const char*, const char*, const addrinfo*, addrinfo**) {
	++network_attempts; return EAI_FAIL;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
	char path[4096];
	const ssize_t n = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), path, sizeof(path) - 1);
	if (n > 0 && ends_with(std::string(path, size_t(n)), "/media.part")) ++media_syncs;
	return __real_fsync(fd);
}

int main(int argc, char** argv) {
	try {
		if (argc != 4) return 2;
		const fs::path root = fs::absolute(argv[1]);
		const std::string media = read_bytes(argv[2]);
		check(media.size() > (1u << 20), "synthetic video exceeds the rolling cache and allows intermediate progress");
		http_init("");
		{
			ControlledSwarm swarm(root / "cache-cold", media);
			auto request = request_for(root / "cold", swarm.torrent->hex);
			std::atomic<bool> cancel{false}; Watchdog watchdog(cancel);
			ProgressAudit progress;
			const auto result = download_transfer(request, [&](const DownloadTransferProgress& value) {
				progress.record(value, request.partial_path);
			}, cancel);
			check(!watchdog.timed_out && result.status == DownloadTransferStatus::Complete,
			      "cold standalone torrent download completes through the real engine: " + result.error);
			check(read_bytes(request.partial_path) == media && result.done == int64_t(media.size()) && result.total == result.done,
			      "downloaded media.part is the exact selected file, without adjacent torrent files");
			check(progress.valid && progress.observed_partial_file && progress.values.size() >= 3 &&
			      progress.values.front().done == 0 && progress.values.back().done == int64_t(media.size()),
			      "progress advances above zero while media.part grows, then reports completion");
			swarm.verify_finished(media.size());
			check(swarm.verified_pieces > swarm.torrent->nslots, "download survives real rolling-cache eviction");
			std::cout << "Cold download: " << result.done << " bytes, " << progress.values.size()
			          << " progress events, " << swarm.verified_pieces << " SHA-1-verified pieces\n";
		}
		int64_t paused_bytes = 0;
		DownloadTransferRequest paused_request;
		{
			ControlledSwarm swarm(root / "cache-paused", media);
			paused_request = request_for(root / "resume", swarm.torrent->hex);
			paused_request.stream.file_idx = -1;  // real episode-selection code also participates
			std::atomic<bool> cancel{false}; Watchdog watchdog(cancel);
			ProgressAudit progress;
			const auto result = download_transfer(paused_request, [&](const DownloadTransferProgress& value) {
				progress.record(value, paused_request.partial_path);
				if (value.done > 0) cancel = true;
			}, cancel);
			check(!watchdog.timed_out && result.status == DownloadTransferStatus::Cancelled && result.done > 0 && result.done < result.total,
			      "pausing a real active torrent download preserves a partial transfer");
			paused_bytes = result.done;
			const json checkpoint = json::parse(read_bytes(paused_request.checkpoint_path));
			check(checkpoint.at("bytes").get<int64_t>() == paused_bytes && checkpoint.at("total").get<int64_t>() == int64_t(media.size()) &&
			      read_bytes(paused_request.partial_path) == media.substr(0, size_t(paused_bytes)),
			      "pause persists the exact verified prefix and durable resume offset");
			swarm.verify_finished(media.size());
		}
		// Simulate shutdown/crash and a fresh engine/cache, including an unsafe
		// tail beyond the checkpoint that the production transfer must trim.
		{ std::ofstream file(paused_request.partial_path, std::ios::binary | std::ios::app); file << "uncommitted-tail"; }
		{
			ControlledSwarm swarm(root / "cache-resumed", media, paused_bytes);
			std::atomic<bool> cancel{false}; Watchdog watchdog(cancel);
			ProgressAudit progress;
			const auto result = download_transfer(paused_request, [&](const DownloadTransferProgress& value) {
				progress.record(value, paused_request.partial_path);
			}, cancel);
			check(!watchdog.timed_out && result.status == DownloadTransferStatus::Complete && result.done == int64_t(media.size()),
			      "download resumes and completes after engine/cache restart: " + result.error);
			check(progress.valid && !progress.values.empty() && progress.values.front().done == paused_bytes &&
			      progress.values.back().done == int64_t(media.size()) && read_bytes(paused_request.partial_path) == media,
			      "resume continues from the checkpoint and never appends the uncommitted crash tail");
			swarm.verify_finished(media.size());
			check(swarm.first_requested_piece == int((ControlledSwarm::prefix_size + paused_bytes) / swarm.piece_length),
			      "fresh peer/cache starts at the resumed piece in a nonzero-offset torrent file");
			std::cout << "Resume: " << paused_bytes << " -> " << result.done << " bytes; first piece "
			          << swarm.first_requested_piece << "; exact completed media bytes verified\n";
		}
		{
			// Large pieces exercise the 1 MiB copy buffer, real disk cache and
			// repeated durable batches together. The valid trailing MP4 free atom
			// keeps this synthetic file finite without expensive video encoding.
			std::string large = media;
			const uint32_t padding = uint32_t((64u << 20) - large.size());
			for (int shift : {24, 16, 8, 0}) large += char((padding >> shift) & 255);
			large += "free"; large.resize(64u << 20, '\0');
			ControlledSwarm swarm(root / "cache-large", large, 0, 8ll << 20, 64);
			auto request = request_for(root / "large", swarm.torrent->hex);
			std::atomic<bool> cancel{false}; Watchdog watchdog(cancel); ProgressAudit progress;
			media_syncs = 0;
			const auto result = download_transfer(request, [&](const DownloadTransferProgress& value) {
				progress.record(value, request.partial_path);
			}, cancel);
			check(!watchdog.timed_out && result.status == DownloadTransferStatus::Complete && result.done == int64_t(large.size()),
			      "large-piece real-engine download completes through batched durable I/O: " + result.error);
			check(read_bytes(request.partial_path) == large && progress.valid && progress.values.back().done == result.total,
			      "large-piece copy preserves exact selected bytes and resolved torrent file index");
			const json checkpoint = json::parse(read_bytes(request.checkpoint_path));
			check(checkpoint.at("bytes").get<int64_t>() == result.done && media_syncs == 2,
			      "64 MiB real-engine transfer needs only initial and final durable media syncs");
			swarm.verify_finished(large.size());
			std::cout << "Large-piece download: " << result.done << " bytes, " << swarm.verified_pieces
			          << " verified 8 MiB pieces, " << media_syncs << " durable media syncs\n";
		}
		decode_active_torrent(root, read_bytes(argv[3]));
		check(network_attempts.load() == 0, "every torrent integration case remained offline");
		std::cout << "PASS: " << checks << " production torrent download integration assertions\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "FAIL after " << checks << " assertions: " << error.what() << '\n';
		return 1;
	}
}
