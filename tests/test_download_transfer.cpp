#include "download_transfer.h"
#include "http.h"
#include "torrent/engine.h"
#include "util.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {
int checks = 0;
bool fail_writes = false, fail_sync = false;
std::string fixture, torrent_bytes;
std::atomic<bool>* torrent_cancel = nullptr;
int64_t torrent_cancel_at = -1, torrent_stopped_at = -1, first_torrent_read = -1;
int torrent_opens = 0, torrent_closes = 0;
int guessed_season = -100, guessed_episode = -100;
bt::ReaderRole torrent_reader_role = bt::ReaderRole::Playback;

void check(bool value, const std::string& label) {
	++checks;
	if (!value) throw std::runtime_error(label);
}

std::string file_bytes(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void media_tracks(const std::string& path, int wanted_audio, int wanted_subtitles) {
	AVFormatContext* format = nullptr;
	check(avformat_open_input(&format, path.c_str(), nullptr, nullptr) >= 0, "offline file opens without a network input");
	check(avformat_find_stream_info(format, nullptr) >= 0, "offline file has readable media information");
	int video = 0, audio = 0, subtitles = 0;
	bool italian = false, english = false;
	for (unsigned i = 0; i < format->nb_streams; ++i) {
		const auto* st = format->streams[i];
		video += st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
		audio += st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
		subtitles += st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE;
		if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			const auto* lang = av_dict_get(st->metadata, "language", nullptr, 0);
			italian |= lang && std::string(lang->value) == "ita";
			english |= lang && std::string(lang->value) == "eng";
		}
	}
	check(video == 1, "one selected video quality is preserved");
	check(audio == wanted_audio && subtitles == wanted_subtitles, "audio and supported subtitle tracks are preserved");
	check(italian && english, "audio language metadata is preserved");
	AVPacket* packet = av_packet_alloc();
	int packets = 0, result = 0;
	while ((result = av_read_frame(format, packet)) >= 0) { ++packets; av_packet_unref(packet); }
	check(result == AVERROR_EOF && packets > 10, "offline container reads to its end");
	av_packet_free(&packet); avformat_close_input(&format);
}

DownloadTransferRequest make_request(const std::string& root, const std::string& base, const std::string& mode) {
	DownloadTransferRequest request;
	request.work_dir = root + "/" + mode;
	fs::create_directories(request.work_dir);
	request.partial_path = request.work_dir + "/media.part";
	request.checkpoint_path = request.work_dir + "/transfer.json";
	request.stream.kind = StreamKind::Direct;
	request.stream.url = base + "/" + mode + "?providerToken=TOKEN_DO_NOT_LOG";
	request.stream.filename = "fixture.mp4";
	request.stream.request_headers = {"X-Download-Token: TOKEN_DO_NOT_LOG"};
	return request;
}

DownloadTransferResult cancelled_download(const DownloadTransferRequest& request) {
	std::atomic<bool> cancel{false};
	std::thread timer([&] { std::this_thread::sleep_for(65ms); cancel = true; });
	const auto result = download_transfer(request, {}, cancel);
	timer.join();
	check(result.status == DownloadTransferStatus::Cancelled, "active HTTP cancellation returns Cancelled");
	check(result.done > 0 && result.done < result.total, "cancelled HTTP transfer preserves only a partial file");
	return result;
}
}

extern "C" ssize_t __real_write(int fd, const void* data, size_t count);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t count) {
	if (fail_writes && count >= 512) { errno = ENOSPC; return -1; }
	return __real_write(fd, data, count);
}
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
	if (fail_sync) { errno = EIO; return -1; }
	return __real_fsync(fd);
}

// Controlled Engine stub for isolated transport error/resume cases.
// tests/test_torrent_download_integration.cpp also exercises the production
// engine, picker, SHA-1 verification, rolling cache, reader and transfer together.
namespace bt {
Stats Engine::stats(const std::string&) { return {}; }
struct Torrent {};
Engine& Engine::get() { static Engine engine; return engine; }
void Engine::start(const std::string&, const std::vector<std::string>&) {}
bool Engine::wait_metadata(const std::string&, std::vector<FileInfo>& files, const std::atomic<bool>* cancel, double, std::string*) {
	if (cancel && cancel->load()) return false;
	files = {{"episode.s02e03.mp4", int64_t(torrent_bytes.size()), 0}, {"extra.txt", 5, int64_t(torrent_bytes.size())}};
	return true;
}
int Engine::guess_file(const std::vector<FileInfo>&, int season, int episode) { guessed_season = season; guessed_episode = episode; return 0; }
std::shared_ptr<Torrent> Engine::open_reader(const std::string&, int index, int* id, int64_t* size, std::string*, ReaderRole role) {
	torrent_reader_role = role;
	if (index != 0) return {};
	++torrent_opens; *id = torrent_opens; *size = int64_t(torrent_bytes.size()); return std::make_shared<Torrent>();
}
void Engine::close_reader(const std::shared_ptr<Torrent>&, int) { ++torrent_closes; }
int Engine::read(const std::shared_ptr<Torrent>&, int, int, int64_t position, uint8_t* buffer, int wanted, const std::atomic<bool>* abort) {
	if (first_torrent_read < 0) first_torrent_read = position;
	if (torrent_cancel && torrent_cancel_at >= 0 && position >= torrent_cancel_at) {
		*torrent_cancel = true;
		while (!abort->load()) std::this_thread::sleep_for(2ms);
		return -1;
	}
	if (torrent_stopped_at >= 0 && position >= torrent_stopped_at) return -2;
	if (abort->load()) return -1;
	const int n = int(std::min<int64_t>({wanted, 32768, int64_t(torrent_bytes.size()) - position}));
	if (n <= 0) return 0;
	std::memcpy(buffer, torrent_bytes.data() + position, size_t(n)); return n;
}
}

int main(int argc, char** argv) {
	try {
		if (argc == 3 && std::string(argv[1]) == "--offline-probe") {
			media_tracks(std::string(argv[2]) + "/direct/media.part", 2, 1);
			media_tracks(std::string(argv[2]) + "/hls-good.m3u8/media.part", 2, 0);
			media_tracks(std::string(argv[2]) + "/hls-master.m3u8/media.part", 2, 0);
			std::cout << "Offline server-stopped probe: " << checks << " checks passed\n"; return 0;
		}
		if (argc != 4) return 2;
		const std::string root = argv[1], base = argv[2];
		fixture = file_bytes(argv[3]); torrent_bytes = fixture;
		check(fixture.size() > 256u * 1024, "synthetic fixture is long enough to exercise cancellation/resume");
		http_init("");
		std::atomic<bool> cancel{false};
		int progress_events = 0; DownloadTransferProgress last;
		auto report = [&](const DownloadTransferProgress& value) { ++progress_events; last = value; check(value.done >= 0 && (value.total < 0 || value.done <= value.total) && std::isfinite(value.bytes_per_second) && value.bytes_per_second >= 0, "progress counters stay within valid bounds"); };
		auto direct = make_request(root, base, "direct");
		auto result = download_transfer(direct, report, cancel);
		check(result.status == DownloadTransferStatus::Complete && result.extension == ".mp4", "direct HTTP video download completes");
		check(file_bytes(direct.partial_path) == fixture, "direct HTTP file bytes match exactly");
		check(progress_events >= 2 && last.done == int64_t(fixture.size()) && last.total == last.done, "final progress reports the complete file");
		struct stat st{}; ::stat(direct.partial_path.c_str(), &st); check((st.st_mode & 0777) == 0600, "partial files are private");
		::stat(direct.checkpoint_path.c_str(), &st); check((st.st_mode & 0777) == 0600, "resume checkpoints are private");
		check(file_bytes(direct.checkpoint_path).find("TOKEN_DO_NOT_LOG") == std::string::npos && file_bytes(direct.checkpoint_path).find("://") == std::string::npos, "checkpoint does not persist source credentials or URLs");
		result = download_transfer(direct, {}, cancel);
		check(result.status == DownloadTransferStatus::Complete && result.extension == ".mp4", "416 with matching total and validator confirms an existing complete file");
		for (const std::string mode : {"resume", "resume-ignored", "resume-changed", "resume-lastmodified", "resume-novalidator", "resume-bad416"}) {
			auto request = make_request(root, base, mode);
			const auto paused = cancelled_download(request);
			// Uncommitted tail bytes after a crash must be trimmed to the fsynced checkpoint.
			{ std::ofstream extra(request.partial_path, std::ios::binary | std::ios::app); extra << "UNCOMMITTED-TAIL"; }
			result = download_transfer(request, {}, cancel);
			check(result.status == DownloadTransferStatus::Complete, mode + " completes after safe resume/restart: " + result.error);
			const bool changed = mode == "resume-ignored" || mode == "resume-changed";
			check(file_bytes(request.partial_path) == fixture + (changed ? std::string(64, 'B') : ""), mode + " never mixes old/new response bytes");
			check(result.done > paused.done, mode + " advances from the partial state");
		}
		for (const std::string mode : {"resume-wrongstart", "resume-badlength"}) {
			auto request = make_request(root, base, mode); cancelled_download(request);
			result = download_transfer(request, {}, cancel);
			check(result.status == DownloadTransferStatus::Error && result.done < result.total, mode + " rejects an inconsistent Content-Range");
		}
		auto truncated = make_request(root, base, "truncated");
		result = download_transfer(truncated, {}, cancel);
		check(result.status == DownloadTransferStatus::Error && result.done > 0 && result.done < result.total, "truncated response is never marked complete");
		result = download_transfer(truncated, {}, cancel);
		check(result.status == DownloadTransferStatus::Complete && file_bytes(truncated.partial_path) == fixture, "truncated file resumes using its saved validator");
		for (const std::string mode : {"unknown-length", "html-error", "dash.mpd"}) {
			auto request = make_request(root, base, mode);
			result = download_transfer(request, {}, cancel);
			check(result.status == DownloadTransferStatus::Unsupported, mode + " cannot masquerade as an offline video");
			check(result.error.find("TOKEN_DO_NOT_LOG") == std::string::npos && result.error.find(base) == std::string::npos, "download error contains no provider secret");
		}
		auto unavailable = make_request(root, base, "missing");
		check(download_transfer(unavailable, {}, cancel).status == DownloadTransferStatus::Error, "HTTP 404 is a visible download error");
		auto full_disk = make_request(root, base, "disk-full"); fail_writes = true;
		result = download_transfer(full_disk, {}, cancel); fail_writes = false;
		check(result.status == DownloadTransferStatus::Error && result.error.find("storage") != std::string::npos, "ENOSPC cannot publish a partial video");
		auto bad_sync = make_request(root, base, "bad-sync"); fail_sync = true;
		result = download_transfer(bad_sync, {}, cancel); fail_sync = false;
		check(result.status == DownloadTransferStatus::Error, "fsync failure cannot publish a completed video");
		auto bad_path = make_request(root, base, "bad-path"); bad_path.partial_path = root + "/escaped.mp4";
		check(download_transfer(bad_path, {}, cancel).status == DownloadTransferStatus::Error && !fs::exists(bad_path.partial_path), "transport refuses writes outside its flat job directory");
		auto symlink = make_request(root, base, "symlink"); fs::create_symlink(direct.partial_path, symlink.partial_path);
		check(download_transfer(symlink, {}, cancel).status == DownloadTransferStatus::Error && file_bytes(direct.partial_path) == fixture, "transport never follows a partial-file symlink");
		for (const std::string mode : {"hls-good.m3u8", "hls-master.m3u8", "hls-redirect.m3u8", "hls-alias"}) {
			auto request = make_request(root, base, mode);
			result = download_transfer(request, {}, cancel);
			check(result.status == DownloadTransferStatus::Complete && result.extension == ".mkv", mode + " remuxes a finite HLS VOD into MKV: " + result.error);
			check(result.done == int64_t(fs::file_size(request.partial_path)) && result.total == result.done, "HLS result reports actual final file bytes");
			media_tracks(request.partial_path, 2, 0);
		}
		for (const std::string mode : {"hls-live.m3u8", "hls-encrypted.m3u8", "hls-cycle.m3u8", "hls-local.m3u8"}) {
			auto request = make_request(root, base, mode);
			result = download_transfer(request, {}, cancel);
			check(result.status == DownloadTransferStatus::Unsupported, mode + " is rejected explicitly");
		}
		auto broken_hls = make_request(root, base, "hls-missing.m3u8");
		result = download_transfer(broken_hls, {}, cancel);
		check(result.status == DownloadTransferStatus::Error, "missing HLS segment never becomes a false complete file");
		auto slow_hls = make_request(root, base, "hls-slow.m3u8");
		std::thread timer([&] { std::this_thread::sleep_for(90ms); cancel = true; });
		result = download_transfer(slow_hls, {}, cancel); timer.join(); cancel = false;
		check(result.status == DownloadTransferStatus::Cancelled, "HLS segment download remains cancellable");
		result = download_transfer(slow_hls, {}, cancel);
		check(result.status == DownloadTransferStatus::Complete, "cancelled HLS restarts cleanly into a valid container");
		auto torrent = make_request(root, base, "torrent");
		torrent.stream = {}; torrent.stream.kind = StreamKind::Torrent;
		torrent.stream.info_hash = "0123456789abcdef0123456789abcdef01234567"; torrent.season = 2; torrent.episode = 3;
		result = download_transfer(torrent, {}, cancel);
		check(result.status == DownloadTransferStatus::Complete && file_bytes(torrent.partial_path) == fixture, "torrent reader copies the exact complete verified file");
		check(torrent_reader_role == bt::ReaderRole::Download && guessed_season == 2 && guessed_episode == 3, "torrent download preserves playback selection and guesses the exact episode");
		check(torrent_opens == torrent_closes, "completed torrent reader is closed");
		auto torrent_partial = make_request(root, base, "torrent-partial"); torrent_partial.stream = torrent.stream;
		torrent_cancel = &cancel; torrent_cancel_at = 65536; first_torrent_read = -1;
		result = download_transfer(torrent_partial, {}, cancel); torrent_cancel = nullptr; torrent_cancel_at = -1; cancel = false;
		check(result.status == DownloadTransferStatus::Cancelled && result.done >= 65536, "torrent cancellation preserves a checkpoint");
		const int64_t paused_torrent = result.done; first_torrent_read = -1;
		result = download_transfer(torrent_partial, {}, cancel);
		check(result.status == DownloadTransferStatus::Complete && first_torrent_read == paused_torrent && file_bytes(torrent_partial.partial_path) == fixture, "torrent restarts at the persisted verified byte offset");
		check(torrent_opens == torrent_closes, "cancelled and resumed torrent readers are closed");
		auto torrent_bad_index = make_request(root, base, "torrent-bad-index"); torrent_bad_index.stream = torrent.stream; torrent_bad_index.stream.file_idx = 8;
		const int opened_before = torrent_opens;
		check(download_transfer(torrent_bad_index, {}, cancel).status == DownloadTransferStatus::Unsupported && torrent_opens == opened_before, "explicit invalid torrent fileIdx never guesses a different video");
		auto torrent_stopped = make_request(root, base, "torrent-stopped"); torrent_stopped.stream = torrent.stream; torrent_stopped_at = 65536;
		result = download_transfer(torrent_stopped, {}, cancel); torrent_stopped_at = -1;
		check(result.status == DownloadTransferStatus::Error && result.done < result.total, "engine stop cannot publish an incomplete torrent file");
		cancel = true;
		check(download_transfer(direct, {}, cancel).status == DownloadTransferStatus::Cancelled, "already cancelled job performs no transfer");
		std::cout << "Download transport: " << checks << " checks passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "Download transport FAILED after " << checks << " checks: " << error.what() << '\n';
		return 1;
	}
}
