// Controlled transfer I/O instrumentation. HTTP uses real curl over loopback;
// the torrent boundary is a deterministic verified-byte source used only for
// filesystem failure injection. The separate integration suite uses Engine.
#include "download_transfer.h"
#include "http.h"
#include "torrent/engine.h"
#include "util.h"

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
int checks = 0;
std::string media_bytes;
std::atomic<bool>* torrent_cancel = nullptr;
int64_t torrent_cancel_at = -1, torrent_error_at = -1, first_read = -1;
int torrent_read_delay_ms = 0;

struct IoAudit {
	int media_fd = -1;
	int media_syncs = 0, checkpoint_syncs = 0, directory_syncs = 0, sync_failures = 0, writes = 0;
	int64_t media_durable = 0, write_fail_at = -1;
	std::string fail_sync_kind;
	int64_t fail_sync_at = -1;
	bool fail_rename = false, write_failed_once = false, order_valid = true;
	bool reject_direct_flag = false, reject_direct_write = false, direct_write_rejected = false;
	bool fail_direct_write = false, buffer_aligned = true;
	bool force_short_direct = false, short_direct_written = false;
	int direct_rejections = 0, direct_writes = 0, buffered_writes = 0;
	int sync_delay_ms = 0;
	bool fail_media_write = false, force_short_write = false, short_written = false;
	size_t largest_write = 0;
	double sync_ms = 0;
	Clock::time_point start = Clock::now();
	json events = json::array();
} audit;

void check(bool condition, const std::string& message) {
	++checks;
	if (!condition) throw std::runtime_error(message);
}

std::string read_bytes(const std::string& path) {
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input || input.tellg() < 0) return {};
	std::string result(size_t(input.tellg()), '\0'); input.seekg(0);
	input.read(result.data(), std::streamsize(result.size()));
	return input ? result : std::string{};
}

std::string descriptor_path(int fd) {
	std::array<char, 4096> path{};
	const ssize_t n = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), path.data(), path.size() - 1);
	return n > 0 ? std::string(path.data(), size_t(n)) : "";
}

int64_t checkpoint_bytes(const DownloadTransferRequest& request) {
	const json state = json::parse(read_bytes(request.checkpoint_path), nullptr, false);
	return state.is_object() ? state.value("bytes", int64_t(-1)) : -1;
}

DownloadTransferRequest make_request(const fs::path& root, const std::string& mode, const std::string& url = {}) {
	DownloadTransferRequest request;
	request.work_dir = (root / mode).string(); fs::create_directories(request.work_dir);
	request.partial_path = request.work_dir + "/media.part";
	request.checkpoint_path = request.work_dir + "/transfer.json";
	request.stream.filename = "synthetic.mp4";
	if (url.empty()) {
		request.stream.kind = StreamKind::Torrent;
		request.stream.info_hash = "0123456789abcdef0123456789abcdef01234567";
		request.stream.file_idx = 0;
	} else {
		request.stream.kind = StreamKind::Direct;
		request.stream.url = url;
	}
	return request;
}

void restart_exact(const DownloadTransferRequest& request, int64_t expected_start) {
	{ std::ofstream tail(request.partial_path, std::ios::binary | std::ios::app); tail << "unsafe-crash-tail"; }
	audit = {}; first_read = -1;
	std::atomic<bool> cancel{false};
	const auto result = download_transfer(request, {}, cancel);
	check(result.status == DownloadTransferStatus::Complete && first_read == expected_start,
	      "restart begins at the durable checkpoint, including after injected I/O failure");
	check(read_bytes(request.partial_path) == media_bytes && checkpoint_bytes(request) == result.total,
	      "restart truncates the unsafe tail and produces the exact complete media");
	check(audit.order_valid, "restart preserves media-before-checkpoint-before-directory durability ordering");
}

json result_json(const DownloadTransferResult& result, double seconds) {
	return {{"status", int(result.status)}, {"bytes", result.done}, {"elapsedSeconds", seconds},
	        {"instrumentedMiBPerSecond", seconds > 0 ? double(result.done) / seconds / 1048576.0 : 0},
	        {"mediaSyncs", audit.media_syncs}, {"checkpointSyncs", audit.checkpoint_syncs},
	        {"directorySyncs", audit.directory_syncs}, {"syncFailures", audit.sync_failures},
	        {"mediaWriteCalls", audit.writes}, {"syncMilliseconds", audit.sync_ms},
	        {"durabilityOrderValid", audit.order_valid}, {"syncEvents", audit.events}};
}
}  // namespace

extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" int __real_fcntl(int, int, ...);
extern "C" int __wrap_fcntl(int fd, int command, ...) {
	va_list args; va_start(args, command);
	int result;
	if (command == F_GETFL || command == F_GETFD) result = __real_fcntl(fd, command, 0);
	else if (command == F_SETFL || command == F_SETFD || command == F_DUPFD || command == F_DUPFD_CLOEXEC) {
		const int value = va_arg(args, int);
		if (fd == audit.media_fd && command == F_SETFL && (value & O_DIRECT) && audit.reject_direct_flag) {
			++audit.direct_rejections; errno = EOPNOTSUPP; result = -1;
		} else result = __real_fcntl(fd, command, value);
	} else result = __real_fcntl(fd, command, va_arg(args, void*));
	va_end(args); return result;
}
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t count) {
	if (fd == audit.media_fd) {
		++audit.writes;
		if (audit.fail_media_write) { errno = EIO; return -1; }
		if (audit.force_short_write && !audit.short_written) { count = std::min<size_t>(count, 4096); audit.short_written = true; }
		audit.largest_write = std::max(audit.largest_write, count);
		audit.buffer_aligned &= reinterpret_cast<uintptr_t>(data) % (16u << 10) == 0;
		if (__real_fcntl(fd, F_GETFL, 0) & O_DIRECT) {
			++audit.direct_writes;
			if (audit.fail_direct_write) { errno = EIO; return -1; }
			if (audit.reject_direct_write) {
				++audit.direct_rejections; audit.direct_write_rejected = true; errno = EINVAL; return -1;
			}
			if (audit.force_short_direct && !audit.short_direct_written) {
				count = std::min<size_t>(count, 4096); audit.short_direct_written = true;
			}
		} else ++audit.buffered_writes;
		if (audit.write_fail_at >= 0) {
			const off_t at = ::lseek(fd, 0, SEEK_CUR);
			if (at >= audit.write_fail_at) { errno = ENOSPC; audit.write_failed_once = true; return -1; }
			count = std::min<size_t>(count, size_t(audit.write_fail_at - at));
		}
	}
	return __real_write(fd, data, count);
}

extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
	const auto begin = Clock::now();
	const std::string path = descriptor_path(fd);
	struct stat st{}; ::fstat(fd, &st);
	std::string kind;
	int64_t bytes = audit.media_durable;
	if (ends_with(path, "/media.part")) {
		kind = "media"; ++audit.media_syncs; audit.media_fd = fd; bytes = st.st_size;
	} else if (ends_with(path, "/transfer.json.tmp")) {
		kind = "checkpoint"; ++audit.checkpoint_syncs;
		const json state = json::parse(read_bytes(path), nullptr, false);
		bytes = state.is_object() ? state.value("bytes", int64_t(-1)) : -1;
		audit.order_valid &= bytes >= 0 && bytes <= audit.media_durable;
	} else if (S_ISDIR(st.st_mode)) { kind = "directory"; ++audit.directory_syncs; }
	const bool fail = !kind.empty() && kind == audit.fail_sync_kind && bytes >= audit.fail_sync_at;
	const auto sync_at = Clock::now();
	if (!kind.empty() && audit.sync_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(audit.sync_delay_ms));
	const int result = fail ? (errno = EIO, -1) : __real_fsync(fd);
	const double sync_ms = std::chrono::duration<double, std::milli>(Clock::now() - sync_at).count();
	const int saved_errno = errno;
	if (!kind.empty()) {
		if (result != 0) ++audit.sync_failures;
		if (kind == "media" && result == 0) audit.media_durable = bytes;
		audit.events.push_back({{"kind", kind}, {"bytes", bytes}, {"ok", result == 0},
		                        {"seconds", std::chrono::duration<double>(begin - audit.start).count()}});
		audit.sync_ms += sync_ms;
	}
	errno = saved_errno;
	return result;
}

extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* from, const char* to) {
	if (audit.fail_rename && ends_with(from, "/transfer.json.tmp") && audit.media_durable >= (256ll << 20)) { errno = EIO; return -1; }
	return __real_rename(from, to);
}

namespace bt {
Stats Engine::stats(const std::string&) { Stats result; result.found = result.has_metadata = true; result.peers = result.seeders = 1; return result; }
struct Torrent {};
Engine& Engine::get() { static Engine engine; return engine; }
void Engine::start(const std::string&, const std::vector<std::string>&) {}
bool Engine::wait_metadata(const std::string&, std::vector<FileInfo>& files, const std::atomic<bool>*, double, std::string*) {
	files = {{"synthetic.mp4", int64_t(media_bytes.size()), 0}}; return true;
}
int Engine::guess_file(const std::vector<FileInfo>&, int, int) { return 0; }
std::shared_ptr<Torrent> Engine::open_reader(const std::string&, int, int* id, int64_t* size, std::string*, ReaderRole) {
	*id = 1; *size = media_bytes.size(); return std::make_shared<Torrent>();
}
void Engine::close_reader(const std::shared_ptr<Torrent>&, int) {}
int Engine::read(const std::shared_ptr<Torrent>&, int, int, int64_t position, uint8_t* buffer, int wanted, const std::atomic<bool>* abort) {
	if (first_read < 0) first_read = position;
	if (torrent_cancel && position >= torrent_cancel_at) {
		*torrent_cancel = true;
		while (!abort->load()) std::this_thread::sleep_for(1ms);
		return -1;
	}
	if (torrent_error_at >= 0 && position >= torrent_error_at) return -2;
	if (abort->load()) return -1;
	if (torrent_read_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(torrent_read_delay_ms));
	const int count = int(std::min<int64_t>(wanted, int64_t(media_bytes.size()) - position));
	if (count > 0) std::memcpy(buffer, media_bytes.data() + position, size_t(count));
	return count;
}
}  // namespace bt

int main(int argc, char** argv) {
	try {
		if (argc != 6) return 2;
		const std::string mode = argv[1], base = argv[3];
		const fs::path root = fs::absolute(argv[2]);
		media_bytes = read_bytes(argv[4]);
		check(media_bytes.size() >= (300u << 20), "controlled media is large enough for repeated durable batches");
		http_init(""); std::atomic<bool> cancel{false};
		json report;
		if (mode == "--benchmark") {
			auto request = make_request(root, "torrent-benchmark");
			audit = {}; audit.sync_delay_ms = 25; const auto start = Clock::now();
			const auto result = download_transfer(request, {}, cancel);
			const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
			check(result.status == DownloadTransferStatus::Complete && read_bytes(request.partial_path) == media_bytes,
			      "instrumented torrent transfer saves the exact media");
			check(audit.order_valid && checkpoint_bytes(request) == result.total,
			      "instrumented transfer durably commits the complete file in correct order");
			report = result_json(result, seconds);
		} else if (mode == "--durability") {
			{
				auto request = make_request(root, "buffered"); audit = {};
				const auto result = download_transfer(request, {}, cancel);
				check(result.status == DownloadTransferStatus::Complete && read_bytes(request.partial_path) == media_bytes,
				      "buffered sequential writes preserve every verified byte");
				check(audit.order_valid && checkpoint_bytes(request) == result.total,
				      "bulk transfers retain atomic durable resume checkpoints");
				check(audit.buffer_aligned && audit.largest_write == (4u << 20),
				      "torrent writes use a bounded 4 MiB page-aligned buffer");
				check(audit.direct_writes == 0 && audit.buffered_writes > 0,
				      "the filesystem can coalesce writes without per-batch direct I/O");
				check(audit.media_syncs == 3 && audit.checkpoint_syncs == 3 && audit.directory_syncs == 3,
				      "a 320 MiB transfer checkpoints at startup, 256 MiB and completion");
				for (const auto& event : audit.events) if (event["kind"] == "checkpoint" && event["ok"] == true) {
					const int64_t bytes = event["bytes"];
					check(bytes == 0 || bytes == (256ll << 20) || bytes == result.total,
					      "normal progress does not trigger frequent filesystem flushes");
				}
				report["buffered"] = result_json(result, std::chrono::duration<double>(Clock::now() - audit.start).count());
			}
			{
				auto request = make_request(root, "short-buffered-write"); audit = {}; audit.force_short_write = true;
				const auto result = download_transfer(request, {}, cancel);
				check(result.status == DownloadTransferStatus::Complete && read_bytes(request.partial_path) == media_bytes,
				      "a short write preserves the current offset and exact complete file");
				check(audit.short_written && audit.direct_writes == 0,
				      "a short write completes its remaining bytes without changing I/O mode");
			}
			{
				const size_t original_size = media_bytes.size();
				media_bytes.append("\0\0\0\rfreetails", 13);
				auto request = make_request(root, "unaligned-tail"); audit = {};
				const auto result = download_transfer(request, {}, cancel);
				check(result.status == DownloadTransferStatus::Complete && read_bytes(request.partial_path) == media_bytes,
				      "an unaligned final tail is saved without truncation or padding");
				check(audit.direct_writes == 0 && audit.buffered_writes > 0,
				      "the final unaligned bytes do not switch the file into direct I/O");
				media_bytes.resize(original_size);
			}
			{
				auto request = make_request(root, "unaligned-resume"); audit = {};
				const int64_t prefix = 12345;
				std::ofstream(request.partial_path, std::ios::binary).write(media_bytes.data(), prefix);
				std::ofstream(request.checkpoint_path) << json{{"version", 1}, {"kind", "torrent"},
					{"source", request.stream.info_hash + ":0"}, {"bytes", prefix},
					{"total", media_bytes.size()}, {"extension", ".mp4"}}.dump();
				first_read = -1;
				const auto result = download_transfer(request, {}, cancel);
				check(result.status == DownloadTransferStatus::Complete && first_read == prefix && read_bytes(request.partial_path) == media_bytes,
				      "an unaligned resume preserves the prefix and exact final bytes");
				check(audit.buffered_writes > 0 && audit.direct_writes == 0,
				      "an unaligned resume remains on the sequential buffered path");
			}
			{
				auto request = make_request(root, "write-io-error"); audit = {}; audit.fail_media_write = true;
				const auto result = download_transfer(request, {}, cancel);
				check(result.status == DownloadTransferStatus::Error && result.done == 0 && audit.writes == 1,
				      "a fatal write I/O error is never retried on the same descriptor");
			}
			auto request = make_request(root, "cancel");
			audit = {}; torrent_cancel = &cancel; torrent_cancel_at = 291ll << 20;
			auto result = download_transfer(request, {}, cancel);
			torrent_cancel = nullptr; torrent_cancel_at = -1; cancel = false;
			check(result.status == DownloadTransferStatus::Cancelled && result.done >= (291ll << 20) && result.done < result.total,
			      "cancel interrupts an advancing transfer after at least one durable batch");
			check(checkpoint_bytes(request) == result.done && audit.order_valid,
			      "cancel forces the entire written prefix into a durable checkpoint");
			check(audit.media_syncs == 3, "cancellation saves the current tail after one normal bulk checkpoint");
			report["cancel"] = result_json(result, std::chrono::duration<double>(Clock::now() - audit.start).count());
			restart_exact(request, result.done);

			request = make_request(root, "peer-stop"); audit = {}; torrent_error_at = 291ll << 20;
			result = download_transfer(request, {}, cancel); torrent_error_at = -1;
			check(result.status == DownloadTransferStatus::Error && result.done < result.total && checkpoint_bytes(request) == result.done,
			      "remote read failure still commits the verified prefix for safe resume");
			restart_exact(request, result.done);

			for (const std::string phase : {"media", "checkpoint", "directory", "rename"}) {
				request = make_request(root, "fail-" + phase); audit = {};
				audit.fail_sync_kind = phase; audit.fail_sync_at = 256ll << 20; audit.fail_rename = phase == "rename";
				result = download_transfer(request, {}, cancel);
				check(result.status == DownloadTransferStatus::Error && result.done < result.total,
				      phase + " failure cannot publish a completed video");
				const int64_t durable = checkpoint_bytes(request);
				check(durable >= 0 && durable <= audit.media_durable && audit.order_valid,
				      phase + " failure cannot advance checkpoint beyond the durable media prefix");
				if (phase != "directory") check(durable == 0, phase + " failure preserves the previous atomic checkpoint");
				report["failure-" + phase] = result_json(result, std::chrono::duration<double>(Clock::now() - audit.start).count());
				restart_exact(request, durable);
			}

			request = make_request(root, "partial-write"); audit = {}; audit.write_fail_at = (291ll << 20) + 16384;
			result = download_transfer(request, {}, cancel);
			check(result.status == DownloadTransferStatus::Error && audit.write_failed_once && result.done < int64_t(fs::file_size(request.partial_path)),
			      "short write followed by ENOSPC leaves only an explicitly uncommitted tail");
			check(checkpoint_bytes(request) == result.done && audit.order_valid,
			      "partial write error checkpoints only complete copied chunks");
			restart_exact(request, result.done);

			{
				std::string full_media = std::move(media_bytes);
				media_bytes = read_bytes(std::string(argv[4]) + ".slow");
				check(media_bytes.size() == (128u << 20), "slow torrent fixture stays below the byte checkpoint threshold");
				request = make_request(root, "slow-torrent"); audit = {}; torrent_read_delay_ms = 1100;
				result = download_transfer(request, {}, cancel); torrent_read_delay_ms = 0;
				check(result.status == DownloadTransferStatus::Complete && read_bytes(request.partial_path) == media_bytes,
				      "a slow torrent still finishes with exact playable media");
				int intermediate = 0;
				for (const auto& event : audit.events) if (event["kind"] == "checkpoint" && event["ok"] == true) {
					const int64_t bytes = event["bytes"]; const double at = event["seconds"];
					if (bytes > 0 && bytes < result.total) {
						++intermediate;
						check(at >= 29.9 && at < 34.0,
						      "the time limit checkpoints advancing slow torrent progress after 30 seconds");
					}
				}
				check(intermediate == 1 && audit.order_valid && checkpoint_bytes(request) == result.total,
				      "slow torrents avoid repeated short-interval flushes and retain a durable final checkpoint");
				report["slow-torrent"] = result_json(result, std::chrono::duration<double>(Clock::now() - audit.start).count());
				media_bytes = std::move(full_media);
			}

			request = make_request(root, "slow-http", base + "/slow"); audit = {};
			result = download_transfer(request, {}, cancel);
			check(result.status == DownloadTransferStatus::Complete && result.total < (32ll << 20), "slow HTTP fixture completes below the byte threshold");
			int intermediate = 0; int64_t previous = 0; double previous_at = 0;
			for (const auto& event : audit.events) if (event["kind"] == "checkpoint" && event["ok"] == true) {
				const int64_t bytes = event["bytes"]; const double at = event["seconds"];
				if (bytes > 0 && bytes < result.total) {
					++intermediate;
					check(bytes > previous && at - previous_at >= 1.9 && at - previous_at <= 2.5,
					      "advancing slow transfer saves a timed checkpoint about every two seconds");
				}
				previous = bytes; previous_at = at;
			}
			check(intermediate >= 2 && audit.order_valid && checkpoint_bytes(request) == result.total,
			      "timed flushes preserve slow download progress before final completion");
			report["slow"] = result_json(result, std::chrono::duration<double>(Clock::now() - audit.start).count());
		} else return 2;
		report["checks"] = checks;
		std::ofstream output(argv[5]); output << report.dump(2) << '\n';
		check(output.good(), "I/O evidence report is written");
		std::cout << "Download I/O " << mode << ": " << checks << " assertions passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "Download I/O FAILED after " << checks << " assertions: " << error.what() << '\n'; return 1;
	}
}
