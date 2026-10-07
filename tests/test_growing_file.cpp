// Real growing-file AVIO and FFmpeg demux/decode; the optional torrent tail
// transport is a controlled boundary. No network, account or provider is used.
#include "growing_file_stream.h"
#include "torrent/engine.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace {
int checks = 0, tail_reads = 0, tail_opens = 0, tail_closes = 0;
std::string torrent_bytes;
bool tail_blocks = false;
void expect(bool ok, const char* message) {
	++checks;
	if (!ok) throw std::runtime_error(message);
}
void write_file(const fs::path& path, const std::string& bytes, bool append = false) {
	std::ofstream out(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
	out.write(bytes.data(), std::streamsize(bytes.size()));
	if (!out) throw std::runtime_error("fixture write failed");
}
std::string read_file(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), {}};
}
std::shared_ptr<GrowingFilePlayback> fixture(const fs::path& path, const std::string& prefix, int64_t total) {
	write_file(path, prefix);
	auto source = std::make_shared<GrowingFilePlayback>();
	source->path = path.string(); source->descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
	source->state = std::make_shared<GrowingFileState>(); source->generation = 7; source->total = total;
	source->state->publish({int64_t(prefix.size()), total, 7, GrowingFilePhase::Downloading});
	expect(source->descriptor >= 0, "growing fixture opens its own read descriptor");
	return source;
}
struct Input {
	AVIOContext* pb = nullptr;
	~Input() { GrowingFileStream::close_avio(&pb); }
	GrowingFileStream* stream() { return GrowingFileStream::of(pb); }
	void open(std::shared_ptr<GrowingFilePlayback> source, const std::atomic<bool>* abort,
		const std::atomic<bool>* reposition = nullptr) {
		std::string error; pb = GrowingFileStream::open_avio(std::move(source), abort, reposition, &error);
		expect(pb && error.empty(), "real growing AVIO opens");
	}
};
void lifecycle(const fs::path& root) {
	expect(!progressive_download_threshold(499, 10000), "4.99 percent is ineligible");
	expect(progressive_download_threshold(500, 10000), "exactly 5 percent is eligible");
	expect(!progressive_download_threshold(500, 10001) && progressive_download_threshold(501, 10001), "fractional threshold rounds upward");
	expect(!progressive_download_threshold(999, -1) && !progressive_download_threshold(999, 0), "unknown and zero totals cannot qualify");
	expect(progressive_download_threshold(INT64_MAX, INT64_MAX), "threshold cannot overflow for a large file");
	std::atomic<bool> abort{false}, reposition{false};
	auto source = fixture(root / "lifecycle.part", "abcde", 100);
	Input input; input.open(source, &abort, &reposition); input.stream()->finish_opening();
	uint8_t bytes[128]{};
	expect(input.stream()->read(bytes, 5) == 5 && std::memcmp(bytes, "abcde", 5) == 0, "reads exactly the downloaded prefix");
	auto waiting = std::async(std::launch::async, [&] { return input.stream()->read(bytes, 5); });
	expect(waiting.wait_for(120ms) == std::future_status::timeout, "temporary end of prefix waits instead of EOF");
	write_file(source->path, "fghij", true);
	source->state->publish({10, 100, 7, GrowingFilePhase::Downloading});
	expect(waiting.wait_for(500ms) == std::future_status::ready && waiting.get() == 5 && std::memcmp(bytes, "fghij", 5) == 0,
		"same AVIO resumes when the background writer appends bytes");
	expect(input.stream()->seek(2, SEEK_SET) == 2 && input.stream()->read(bytes, 4) == 4 && std::memcmp(bytes, "cdef", 4) == 0,
		"seeking backward reads the exact already local bytes");
	expect(input.stream()->seek(90, SEEK_SET) == AVERROR(EAGAIN), "future seek is rejected without moving into unreceived bytes");
	expect(input.stream()->seek(0, AVSEEK_SIZE) == 100, "AVIO reports logical full size, not temporary prefix size");
	expect(input.stream()->seek(INT64_MAX, SEEK_CUR) == AVERROR(EINVAL), "seek overflow is rejected");
	input.stream()->seek(10, SEEK_SET);
	waiting = std::async(std::launch::async, [&] { return input.stream()->read(bytes, 5); });
	expect(waiting.wait_for(70ms) == std::future_status::timeout, "second prefix boundary remains a wait");
	reposition = true;
	expect(waiting.wait_for(500ms) == std::future_status::ready && waiting.get() == AVERROR(EAGAIN) && input.stream()->failure().empty(),
		"a controller seek interrupts boundary waiting without a fatal failure");
	reposition = false;
	write_file(source->path, std::string(90, 'k'), true);
	fs::rename(source->path, root / "complete.mkv");
	source->state->publish({100, 100, 7, GrowingFilePhase::Complete});
	expect(input.stream()->read(bytes, 100) == 90, "held descriptor continues after final atomic rename");
	expect(input.stream()->read(bytes, 1) == AVERROR_EOF, "only full logical end returns EOF");
	expect(input.stream()->seek(-4, SEEK_END) == 96 && input.stream()->read(bytes, 4) == 4,
		"completed renamed file remains seekable");
	for (const auto phase : {GrowingFilePhase::Paused, GrowingFilePhase::Waiting, GrowingFilePhase::Failed, GrowingFilePhase::Removed}) {
		auto state_source = fixture(root / "state.part", "abcdefghij", 100);
		Input state_input; state_input.open(state_source, &abort); state_input.stream()->finish_opening();
		state_input.stream()->seek(10, SEEK_SET);
		auto pending = std::async(std::launch::async, [&] { return state_input.stream()->read(bytes, 1); });
		expect(pending.wait_for(60ms) == std::future_status::timeout, "state transition fixture waits at boundary");
		state_source->state->publish({10, 100, 7, phase});
		expect(pending.wait_for(500ms) == std::future_status::ready && pending.get() != AVERROR_EOF && !state_input.stream()->failure().empty(),
			"pause/wait/failure/removal wakes playback with a reason, not a false completed video");
	}
	auto restarted = fixture(root / "restart.part", "abcdefghij", 100);
	Input restart; restart.open(restarted, &abort);
	restarted->state->publish({5, 100, 8, GrowingFilePhase::Downloading});
	expect(restart.stream()->read(bytes, 1) == AVERROR(EIO) && restart.stream()->failure().find("restarted") != std::string::npos,
		"a truncated/restarted transfer invalidates old playback before any stale byte is read");
	auto cancelled = fixture(root / "cancel.part", "abcde", 100);
	Input cancellation; cancellation.open(cancelled, &abort); cancellation.stream()->finish_opening();
	cancellation.stream()->seek(5, SEEK_SET);
	waiting = std::async(std::launch::async, [&] { return cancellation.stream()->read(bytes, 5); });
	abort = true;
	expect(waiting.wait_for(500ms) == std::future_status::ready && waiting.get() == AVERROR_EXIT, "closing playback cancels a blocked prefix read promptly");
}

struct DecodeResult { int frames = 0; bool frame_at_five_percent = false; int64_t first_frame_prefix = 0; };
DecodeResult decode_growing(const fs::path& root, const fs::path& fixture_path, bool auxiliary) {
	const auto bytes = read_file(fixture_path);
	const size_t prefix = (bytes.size() + 19) / 20;
	auto source = fixture(root / (fixture_path.filename().string() + ".part"), bytes.substr(0, prefix), int64_t(bytes.size()));
	if (auxiliary) { source->torrent_hash = std::string(40, 'a'); source->torrent_file_idx = 3; torrent_bytes = bytes; }
	std::atomic<bool> abort{false}, first_frame{false}, failed{false};
	std::thread writer([&] {
		const auto deadline = std::chrono::steady_clock::now() + 6s;
		while (!first_frame && !failed && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
		if (failed || !first_frame) { abort = true; return; }
		size_t written = prefix;
		while (written < bytes.size() && !abort) {
			const size_t count = std::min<size_t>(32768, bytes.size() - written);
			write_file(source->path, bytes.substr(written, count), true); written += count;
			source->state->publish({int64_t(written), int64_t(bytes.size()), 7, GrowingFilePhase::Downloading});
			std::this_thread::sleep_for(1ms);
		}
		if (!abort) { fs::rename(source->path, source->path + ".complete"); source->state->publish({int64_t(bytes.size()), int64_t(bytes.size()), 7, GrowingFilePhase::Complete}); }
	});
	Input input; input.open(source, &abort);
	AVFormatContext* format = avformat_alloc_context();
	format->pb = input.pb; format->flags |= AVFMT_FLAG_CUSTOM_IO;
	format->probesize = 1 << 20; format->max_analyze_duration = AV_TIME_BASE / 2;
	if (bytes.substr(0, 4) == "\x1A\x45\xDF\xA3") input.pb->seekable = 0;
	DecodeResult result;
	AVCodecContext* codec = nullptr; AVPacket* packet = nullptr; AVFrame* frame = nullptr;
	try {
		expect(avformat_open_input(&format, source->path.c_str(), nullptr, nullptr) >= 0, "FFmpeg opens while only five percent is local");
		expect(avformat_find_stream_info(format, nullptr) >= 0, "FFmpeg discovers streams without waiting for full download");
		input.stream()->finish_opening(); input.pb->seekable = AVIO_SEEKABLE_NORMAL;
		const AVCodec* decoder = nullptr;
		const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
		expect(video >= 0 && decoder, "growing file has its real video decoder");
		codec = avcodec_alloc_context3(decoder);
		avcodec_parameters_to_context(codec, format->streams[video]->codecpar);
		expect(avcodec_open2(codec, decoder, nullptr) >= 0, "real FFmpeg decoder opens");
		packet = av_packet_alloc(); frame = av_frame_alloc();
		auto receive = [&] {
			for (;;) {
				const int code = avcodec_receive_frame(codec, frame);
				if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) break;
				if (code < 0) throw std::runtime_error("decoder rejected growing-file bytes");
				if (result.frames++ == 0) {
					const auto snapshot = source->state->snapshot(); result.first_frame_prefix = snapshot.available;
					result.frame_at_five_percent = snapshot.available == int64_t(prefix) && snapshot.available < snapshot.total;
					first_frame = true;
				}
				av_frame_unref(frame);
			}
		};
		int code = 0;
		while ((code = av_read_frame(format, packet)) >= 0) {
			if (packet->stream_index == video) { expect(avcodec_send_packet(codec, packet) >= 0, "complete growing video packet decodes"); receive(); }
			av_packet_unref(packet);
		}
		expect(code == AVERROR_EOF && input.stream()->failure().empty(), "growing stream finishes at real EOF with no AVIO failure");
		avcodec_send_packet(codec, nullptr); receive();
		expect(result.frame_at_five_percent, "first decoded video frame appears at five percent before writer continues");
		expect(result.frames == 500, "all 500 generated frames decode through temporary boundaries and final rename");
	} catch (...) {
		failed = true; abort = true;
		if (writer.joinable()) writer.join();
		av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&codec); if (format) avformat_close_input(&format);
		throw;
	}
	writer.join(); av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&codec); avformat_close_input(&format);
	std::cout << "Decoded " << fixture_path.filename().string() << ": frames=" << result.frames << " first_frame_prefix=" << result.first_frame_prefix
		<< " total=" << bytes.size() << " percent=" << 100.0 * result.first_frame_prefix / double(bytes.size()) << '\n';
	return result;
}

void missing_metadata(const fs::path& root) {
	std::atomic<bool> abort{false};
	auto source = fixture(root / "missing-metadata.part", std::string(100, 'a'), 8 << 20);
	Input input; input.open(source, &abort);
	expect(input.stream()->seek((8 << 20) - 4096, SEEK_SET) >= 0, "container can ask for tail metadata");
	uint8_t bytes[64]{};
	expect(input.stream()->read(bytes, sizeof(bytes)) == AVERROR(EAGAIN) && input.stream()->failure().find("metadata") != std::string::npos,
		"HTTP tail metadata outside the local prefix reports a clear retry reason without waiting to 100 percent");
	source = fixture(root / "aux-cancel.part", std::string(100, 'a'), 8 << 20);
	source->torrent_hash = std::string(40, 'a'); source->torrent_file_idx = 3;
	torrent_bytes.assign(8 << 20, 'b'); tail_blocks = true;
	Input auxiliary; auxiliary.open(source, &abort); auxiliary.stream()->seek((8 << 20) - 4096, SEEK_SET);
	auto reading = std::async(std::launch::async, [&] { return auxiliary.stream()->read(bytes, sizeof(bytes)); });
	expect(reading.wait_for(100ms) == std::future_status::timeout, "auxiliary metadata fixture genuinely waits for torrent bytes");
	source->state->publish({100, 8 << 20, 7, GrowingFilePhase::Removed});
	expect(reading.wait_for(500ms) == std::future_status::ready && reading.get() != AVERROR_EOF,
		"deleting the download cancels a blocked auxiliary read promptly");
	tail_blocks = false;
}

void metadata_is_not_playback(const fs::path& root) {
	std::atomic<bool> abort{false};
	constexpr int64_t total = 8 << 20;
	auto source = fixture(root / "metadata-cache.part", std::string(100, 'a'), total);
	source->torrent_hash = std::string(40, 'a'); source->torrent_file_idx = 3;
	torrent_bytes.assign(size_t(total), 'b');
	Input input; input.open(source, &abort);
	uint8_t bytes[32]{};
	expect(avio_seek(input.pb, total - 4096, SEEK_SET) == total - 4096,
		"opening may seek to bounded metadata outside the local prefix");
	expect(avio_read(input.pb, bytes, 16) == 16 && bytes[0] == 'b',
		"tail metadata enters the real FFmpeg AVIO buffer during opening");
	const int auxiliary_before = tail_reads;
	input.stream()->finish_opening();
	const int64_t previous = avio_tell(input.pb);
	expect(avio_seek(input.pb, total - 2048, SEEK_SET) == AVERROR(EAGAIN) && avio_tell(input.pb) == previous,
		"cached tail bytes cannot bypass the playback seek guard or move the AVIO cursor");
	expect(avio_read(input.pb, bytes, 1) == AVERROR(EAGAIN),
		"playback cannot read cached future metadata or block on an unsupported future cursor");
	expect(tail_reads == auxiliary_before, "playback never restarts the metadata reader");
	input.pb->error = input.pb->eof_reached = 0;
	expect(avio_seek(input.pb, 20, SEEK_SET) == 20 && avio_read(input.pb, bytes, 16) == 16 && bytes[0] == 'a',
		"local seeking remains readable after a denied metadata-buffer shortcut");
	source->state->publish({total, total, 7, GrowingFilePhase::Complete});
	expect(input.stream()->seek(1000, SEEK_SET) == AVERROR(EAGAIN),
		"a completion flag cannot admit a seek past the bytes physically present");
	write_file(source->path, std::string(size_t(total - 100), 'c'), true);
	expect(input.stream()->seek(total - 1, SEEK_SET) == total - 1 && input.stream()->read(bytes, 1) == 1 && bytes[0] == 'c',
		"completion unlocks the full range only after its bytes exist on disk");
}
}

void dlog(const char*, ...) {}
namespace bt {
struct Torrent {};
Engine& Engine::get() { static Engine engine; return engine; }
std::shared_ptr<Torrent> Engine::open_reader(const std::string& hash, int file, int* reader, int64_t* size, std::string* error, ReaderRole role) {
	expect(hash == std::string(40, 'a') && file == 3 && role == ReaderRole::Auxiliary, "metadata uses the same hash and resolved file with Auxiliary role");
	++tail_opens; *reader = 9; *size = int64_t(torrent_bytes.size()); error->clear(); return std::make_shared<Torrent>();
}
void Engine::close_reader(const std::shared_ptr<Torrent>&, int reader) { expect(reader == 9, "auxiliary reader closes its own registration"); ++tail_closes; }
int Engine::read(const std::shared_ptr<Torrent>&, int, int, int64_t position, uint8_t* buffer, int count, const std::atomic<bool>* abort) {
	++tail_reads;
	while (tail_blocks && !abort->load()) std::this_thread::sleep_for(5ms);
	if (abort->load()) return -1;
	if (position < 0 || position >= int64_t(torrent_bytes.size())) return 0;
	count = int(std::min<int64_t>(count, int64_t(torrent_bytes.size()) - position));
	std::memcpy(buffer, torrent_bytes.data() + position, size_t(count)); return count;
}
}

int main(int argc, char** argv) {
	if (argc != 5) return 2;
	try {
		const fs::path root = argv[1]; fs::create_directories(root);
		lifecycle(root);
		decode_growing(root, argv[2], false);
		decode_growing(root, argv[3], false);
		const int before = tail_reads;
		decode_growing(root, argv[4], true);
		expect(tail_reads > before, "non-faststart MP4 actually read its unavailable moov through the auxiliary boundary");
		missing_metadata(root);
		metadata_is_not_playback(root);
		expect(tail_opens == tail_closes, "all auxiliary registrations are released");
		std::cout << "PASS: " << checks << " growing-file lifecycle and real FFmpeg decode checks; auxiliary_reads=" << tail_reads << '\n';
		return 0;
	} catch (const std::exception& error) { std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1; }
}
