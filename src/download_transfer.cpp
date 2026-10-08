#include "download_transfer.h"
#include "download_telemetry.h"

#include "http.h"
#include "netstream.h"
#include "torrent/engine.h"
#include "util.h"

#if defined(PLATFORM_PS5_NATIVE) || defined(STREMIO_DOWNLOAD_WRITER_TEST)
#include "download_writer/client.hpp"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}
#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sys/stat.h>
#if defined(PLATFORM_PS5_NATIVE)
#include <sys/mount.h>
extern "C" int download_fstatfs(int, struct statfs*) __asm__("_fstatfs");
#elif defined(__linux__)
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif
#include <thread>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
// A checkpoint always follows a successful media flush. Uncommitted tails are
// discarded on restart; pause, cancellation, errors and completion force a save.
constexpr int64_t kCommitBytes = 32ll << 20;
constexpr auto kCommitInterval = std::chrono::seconds(2);
// Bulk torrents keep a bounded recovery window without repeatedly draining
// filesystem writeback while a peer has more data ready.
constexpr int64_t kTorrentCommitBytes = 256ll << 20;
constexpr auto kTorrentCommitInterval = std::chrono::seconds(30);
constexpr size_t kHeaderLimit = 128u << 10;
constexpr size_t kManifestLimit = 4u << 20;
constexpr size_t kManifestSetLimit = 24u << 20;
constexpr int kStallSeconds = 120;
constexpr size_t kWriteAlignment = 16u << 10;
constexpr size_t kTorrentWriteBytes = 4u << 20;
constexpr uint64_t kStorageHeadroom = 16u << 20;

struct File {
	int fd = -1;
	File() = default;
	explicit File(int value) : fd(value) {}
	~File() { const int saved = errno; if (fd >= 0) ::close(fd); errno = saved; }
	File(const File&) = delete;
	File& operator=(const File&) = delete;
	bool close() { const int value = fd; fd = -1; return value < 0 || ::close(value) == 0; }
};

int file_error() { return errno > 0 ? errno : EIO; }

std::string disk_error(int code, const char* fallback = "Could not write the download to storage") {
	if (code == ENOSPC || code == EDQUOT) return "The download destination could not allocate more storage";
	if (code == EACCES || code == EPERM) return "The download folder does not allow writing";
	return fallback;
}

// Query the destination inode, not the PS5's game-storage capacity. A failed
// or invalid probe is unknown space and must not be treated as a full volume.
bool storage_capacity(int fd, int64_t remaining, const char* mode, const char* phase) {
	const int saved_errno = errno;
	struct stat file{};
	const bool identified = fd >= 0 && ::fstat(fd, &file) == 0;
	struct statfs filesystem{};
#ifdef PLATFORM_PS5_NATIVE
	const int measured = fd < 0 ? -1 : download_fstatfs(fd, &filesystem);
#else
	const int measured = fd < 0 ? -1 : ::fstatfs(fd, &filesystem);
#endif
	const int probe_error = measured ? (fd < 0 ? EBADF : file_error()) : 0;
	const int64_t raw_block_size = int64_t(filesystem.f_bsize);
	const uint64_t block_size = raw_block_size > 0 ? uint64_t(raw_block_size) : 0;
	const uint64_t blocks = uint64_t(filesystem.f_blocks);
	const uint64_t free_blocks = uint64_t(filesystem.f_bfree);
	const int64_t raw_available_blocks = int64_t(filesystem.f_bavail);
	const uint64_t available_blocks = raw_available_blocks >= 0 ? uint64_t(raw_available_blocks) : 0;
	const bool known = measured == 0 && block_size && blocks && blocks <= uint64_t(INT64_MAX) && free_blocks <= blocks &&
		raw_available_blocks >= 0 && available_blocks <= free_blocks && available_blocks <= UINT64_MAX / block_size;
	const uint64_t available = known ? available_blocks * block_size : 0;
	const uint64_t needed = uint64_t(std::max<int64_t>(0, remaining));
	const uint64_t reserve = needed ? kStorageHeadroom : 0;
	const uint64_t required = needed <= UINT64_MAX - reserve ? needed + reserve : UINT64_MAX;
	dlog("download capacity: mode=%s phase=%s known=%d probe_errno=%d available_bytes=%llu required_bytes=%llu remaining_bytes=%lld reserve_bytes=%llu blocks=%lld free_blocks=%lld available_blocks=%lld block_size=%lld free_inodes=%lld file_mode=%04o file_uid=%u file_gid=%u device=%llu identity_known=%d",
	     mode, phase, int(known), probe_error, (unsigned long long)available,
	     (unsigned long long)required, (long long)remaining, (unsigned long long)reserve,
	     (long long)blocks, (long long)free_blocks, (long long)raw_available_blocks,
	     (long long)raw_block_size, (long long)filesystem.f_ffree,
	     unsigned(file.st_mode & 07777), unsigned(file.st_uid), unsigned(file.st_gid),
	     (unsigned long long)file.st_dev, int(identified));
#ifdef PLATFORM_PS5_NATIVE
	if (measured == 0)
		dlog("download capacity filesystem: type=%.*s mount=%.*s process_uid=%u process_gid=%u",
		     int(sizeof(filesystem.f_fstypename)), filesystem.f_fstypename,
		     int(sizeof(filesystem.f_mntonname)), filesystem.f_mntonname, unsigned(::geteuid()), unsigned(::getegid()));
#endif
	errno = saved_errno;
	return !known || available >= required;
}

bool regular_fd(int fd, int64_t* size = nullptr) {
	struct stat st{};
	if (fd < 0 || fstat(fd, &st)) return false;
	if (!S_ISREG(st.st_mode) || st.st_size < 0) { errno = EINVAL; return false; }
	if (size) *size = int64_t(st.st_size);
	return true;
}

bool write_all(int fd, const void* data, size_t count) {
	const auto* at = static_cast<const uint8_t*>(data);
	while (count) {
		const ssize_t n = ::write(fd, at, count);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { if (n == 0) errno = EIO; return false; }
		at += size_t(n); count -= size_t(n);
	}
	return true;
}

struct TorrentWriter {
	int fd;
	std::unique_ptr<uint8_t, decltype(&std::free)> buffer{nullptr, &std::free};
#if defined(PLATFORM_PS5_NATIVE) || defined(STREMIO_DOWNLOAD_WRITER_TEST)
	std::unique_ptr<download_writer::Client> helper;
	bool start_helper(const std::string& directory, int64_t& done, int64_t total) {
		const auto slash = directory.rfind('/');
		if (slash == std::string::npos) { errno = EINVAL; return false; }
#ifdef PLATFORM_PS5_NATIVE
		if (!download_writer::wire::download_directory(std::string_view(directory).substr(0, slash))) { errno = EINVAL; return false; }
#endif
		helper = std::make_unique<download_writer::Client>(download_writer::launch_helper());
		if (!helper->begin(std::string_view(directory).substr(0, slash), std::string_view(directory).substr(slash + 1), done, total)) {
			errno = helper->error(); return false;
		}
		done = helper->offset();
		return true;
	}
#endif

	explicit TorrentWriter(int descriptor) : fd(descriptor) {
		void* memory = nullptr;
		if (::posix_memalign(&memory, kWriteAlignment, kTorrentWriteBytes) == 0)
			buffer.reset(static_cast<uint8_t*>(memory));
	}
	size_t capacity(int64_t remaining) const {
		return size_t(std::min<int64_t>(remaining, kTorrentWriteBytes));
	}
	bool write(size_t count) {
#if defined(PLATFORM_PS5_NATIVE) || defined(STREMIO_DOWNLOAD_WRITER_TEST)
		if (!helper || !helper->write(buffer.get(), count)) {
			errno = helper ? helper->error() : EIO;
			return false;
		}
		return true;
#else
		// Keep one buffered writer so the filesystem can coalesce adjacent
		// ranges without forcing every batch through direct I/O.
		size_t at = 0;
		while (at < count) {
			const ssize_t n = ::write(fd, buffer.get() + at, count - at);
			if (n < 0 && errno == EINTR) continue;
			if (n <= 0) { if (n == 0) errno = EIO; return false; }
			at += size_t(n);
		}
		return true;
#endif
	}
};

#if defined(PLATFORM_PS5_NATIVE) || defined(STREMIO_DOWNLOAD_WRITER_TEST)
int helper_storage_error(const download_writer::Client& helper) {
	using download_writer::wire::Stage;
	switch (helper.error_stage()) {
	case Stage::begin:
		return helper.error() == EINVAL || helper.error() == ENOMEM ? 0 : helper.error();
	case Stage::media_write: case Stage::media_sync: case Stage::state_unlink:
	case Stage::state_open: case Stage::state_check: case Stage::state_write:
	case Stage::state_sync: case Stage::state_close: case Stage::state_rename:
	case Stage::directory_sync: case Stage::close:
		return helper.error();
	default:
		return 0;
	}
}
#endif

bool valid_paths(const DownloadTransferRequest& r) {
	if (r.work_dir.empty() || r.work_dir.front() != '/' || r.work_dir.back() == '/') return false;
	for (const auto& part : split(r.work_dir, '/')) if (part == "." || part == "..") return false;
	return r.partial_path == r.work_dir + "/media.part" && r.checkpoint_path == r.work_dir + "/transfer.json";
}

bool sync_dir(const std::string& dir) {
	File fd(::open(dir.c_str(), O_RDONLY | O_DIRECTORY));
	return fd.fd >= 0 && ::fsync(fd.fd) == 0;
}

bool save_checkpoint(const DownloadTransferRequest& r, const json& state) {
	const std::string temporary = r.checkpoint_path + ".tmp";
	File file(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600));
	if (!regular_fd(file.fd) || ::fchmod(file.fd, 0600)) return false;
	const std::string bytes = state.dump();
	if (!write_all(file.fd, bytes.data(), bytes.size()) || ::fsync(file.fd) || !file.close()) return false;
	if (::rename(temporary.c_str(), r.checkpoint_path.c_str())) return false;
	return sync_dir(r.work_dir);
}

json read_checkpoint(const DownloadTransferRequest& r) {
	File file(::open(r.checkpoint_path.c_str(), O_RDONLY | O_NOFOLLOW));
	int64_t size = 0;
	if (!regular_fd(file.fd, &size) || size <= 0 || size > int64_t(kHeaderLimit)) return json::object();
	std::string bytes(size_t(size), '\0');
	size_t at = 0;
	while (at < bytes.size()) {
		const ssize_t n = ::read(file.fd, bytes.data() + at, bytes.size() - at);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return json::object();
		at += size_t(n);
	}
	json result = json::parse(bytes, nullptr, false);
	return result.is_object() ? result : json::object();
}

int64_t integer(const json& state, const char* key, int64_t fallback = -1) {
	const auto it = state.find(key);
	if (it == state.end() || (!it->is_number_integer() && !it->is_number_unsigned())) return fallback;
	if (it->is_number_unsigned()) {
		const uint64_t value = it->get<uint64_t>();
		return value > uint64_t(INT64_MAX) ? fallback : int64_t(value);
	}
	return it->get<int64_t>();
}

std::string source_identity(const Stream& s) {
	AVSHA* sha = av_sha_alloc();
	if (!sha) return {};
	if (av_sha_init(sha, 256) < 0) { av_free(sha); return {}; }
	auto add = [&](const std::string& value) {
		const std::string count = std::to_string(value.size()) + ":";
		av_sha_update(sha, reinterpret_cast<const uint8_t*>(count.data()), count.size());
		av_sha_update(sha, reinterpret_cast<const uint8_t*>(value.data()), value.size());
	};
	add(s.url); add(lower(s.info_hash)); add(std::to_string(s.file_idx));
	for (const auto& header : s.request_headers) add(header);
	std::array<uint8_t, 32> bytes{}; av_sha_final(sha, bytes.data()); av_free(sha);
	constexpr char hex[] = "0123456789abcdef";
	std::string result; result.reserve(64);
	for (const auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
	return result;
}

std::string suffix(const std::string& name, const std::string& mime = {}) {
	const std::string path = lower(name.substr(0, name.find_first_of("?#")));
	const auto dot = path.find_last_of('.');
	const std::string ext = dot == std::string::npos ? "" : path.substr(dot);
	for (const char* allowed : {".mkv", ".mp4", ".m4v", ".mov", ".avi", ".webm", ".ts", ".m2ts", ".mpg", ".mpeg", ".ogv", ".flv"})
		if (ext == allowed) return ext;
	if (mime == "video/mp4" || mime == "application/mp4") return ".mp4";
	if (mime == "video/x-matroska") return ".mkv";
	if (mime == "video/webm") return ".webm";
	if (mime == "video/mp2t") return ".ts";
	return ".media";
}

bool hls_url(const std::string& value) {
	return ends_with(lower(value.substr(0, value.find_first_of("?#"))), ".m3u8");
}
bool hls_mime(const std::string& mime) {
	return mime == "application/vnd.apple.mpegurl" || mime == "application/x-mpegurl" || mime == "audio/mpegurl" || mime == "audio/x-mpegurl";
}
bool bad_media_mime(const std::string& mime) {
	return starts_with(mime, "text/") || mime == "application/json" || mime == "application/xml" || mime == "application/dash+xml";
}

struct Reporter {
	const DownloadProgressCallback& callback;
	std::mutex mutex;
	Clock::time_point last = Clock::now(), connections_at{};
	DownloadRateEstimate rate;
	int64_t written = 0, total_bytes = -1;
	int torrent_file_idx = -1;
	int connected_peers = -1, connected_seeders = -1;
	bool ready = false;
	std::string torrent_hash;
	static double seconds(Clock::time_point at) { return std::chrono::duration<double>(at.time_since_epoch()).count(); }
	explicit Reporter(const DownloadProgressCallback& value) : callback(value) { rate.reset(0, seconds(last)); }
	void reset(int64_t bytes) {
		std::lock_guard lock(mutex); last = Clock::now(); written = bytes; ready = false;
		rate.reset(bytes, seconds(last));
	}
	void torrent(const std::string& hash, int index) {
		std::lock_guard lock(mutex); torrent_hash = hash; torrent_file_idx = index;
	}
	void publish(bool force) {
		const auto now = Clock::now();
		if (!force && now - last < std::chrono::milliseconds(200)) return;
		last = now; rate.sample(written, seconds(now));
		if (!torrent_hash.empty() && (connections_at == Clock::time_point{} || now - connections_at >= std::chrono::seconds(1))) {
			const auto peers = bt::Engine::get().stats(torrent_hash);
			connected_peers = peers.found ? peers.peers : -1;
			connected_seeders = peers.found && peers.has_metadata ? peers.seeders : -1;
			connections_at = now;
		}
		if (callback) callback({written, total_bytes, rate.bytes_per_second(), torrent_file_idx,
			rate.remaining_seconds(written, total_bytes), connected_peers, connected_seeders});
	}
	void emit(int64_t done, int64_t total, bool force = false) {
		std::lock_guard lock(mutex);
		written = std::max<int64_t>(0, done); total_bytes = total; ready = true;
		publish(force);
	}
	// The deadline worker refreshes rate and peers while Engine::read or fsync
	// waits. No unvalidated resume prefix is exposed before the first emit().
	void heartbeat() { std::lock_guard lock(mutex); if (ready) publish(false); }
};

// Per-window timings let console logs separate waiting for verified pieces
// from writing them and making the resume point durable. No source identifier,
// title, URL, filesystem path or credential is included in these events.
struct TransferTimings {
	const char* kind;
	Clock::time_point window = Clock::now();
	int64_t bytes = 0;
	uint64_t reads = 0, writes = 0, commits = 0;
	double read_ms = 0, write_ms = 0, commit_ms = 0;
	double media_sync_ms = 0, state_sync_ms = 0, helper_write_ms = 0;
	static double millis(Clock::time_point start) {
		return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
	}
	void emit(int64_t done, int64_t durable, bool force = false) {
		const auto now = Clock::now();
		const double elapsed = std::chrono::duration<double>(now - window).count();
		if (!force && elapsed < 10) return;
		if (bytes || reads || writes || commits) {
			dlog("download io: kind=%s window_s=%.3f done=%lld durable=%lld useful_MiBs=%.2f read_wait_ms=%.1f write_ms=%.1f commit_ms=%.1f reads=%llu writes=%llu commits=%llu media_sync_ms=%.1f state_sync_ms=%.1f helper_write_ms=%.1f",
			     kind, elapsed, (long long)done, (long long)durable, elapsed > 0 ? double(bytes) / elapsed / 1048576.0 : 0,
			     read_ms, write_ms, commit_ms, (unsigned long long)reads, (unsigned long long)writes, (unsigned long long)commits, media_sync_ms, state_sync_ms, helper_write_ms);
		}
		window = now; bytes = 0; reads = writes = commits = 0; read_ms = write_ms = commit_ms = 0;
		media_sync_ms = state_sync_ms = helper_write_ms = 0;
	}
};

struct CurlHandle {
	CURL* handle = curl_easy_init();
	curl_slist* headers = nullptr;
	~CurlHandle() { if (handle) curl_easy_cleanup(handle); if (headers) curl_slist_free_all(headers); }
	bool add(const std::string& value) {
		curl_slist* next = curl_slist_append(headers, value.c_str());
		if (!next) return false;
		headers = next; return true;
	}
};

bool request_headers(CurlHandle& curl, const Stream& stream) {
	for (const auto& header : stream.request_headers) {
		const auto colon = header.find(':');
		if (colon == std::string::npos || colon == 0 || header.find_first_of("\r\n") != std::string::npos || header.find('\0') != std::string::npos) return false;
		const std::string key = lower(trim(header.substr(0, colon)));
		if (key == "range" || key == "if-range" || key == "accept-encoding" || key == "content-length" || key == "transfer-encoding") continue;
		if (!curl.add(header)) return false;
	}
	return true;
}

int cancel_curl(void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	return static_cast<const std::atomic<bool>*>(data)->load() ? 1 : 0;
}

bool setup_curl(CurlHandle& curl, const std::string& url, const Stream& stream, const std::atomic<bool>& cancel) {
	if (!curl.handle || !http_valid_url(url) || !request_headers(curl, stream)) return false;
	http_setup_handle(curl.handle);
	curl_easy_setopt(curl.handle, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl.handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl.handle, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(curl.handle, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl.handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl.handle, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl.handle, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(curl.handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl.handle, CURLOPT_LOW_SPEED_TIME, long(kStallSeconds));
	curl_easy_setopt(curl.handle, CURLOPT_USERAGENT, kUserAgent);
	curl_easy_setopt(curl.handle, CURLOPT_ACCEPT_ENCODING, "identity");
	curl_easy_setopt(curl.handle, CURLOPT_HTTP_CONTENT_DECODING, 0L);
	curl_easy_setopt(curl.handle, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
	curl_easy_setopt(curl.handle, CURLOPT_BUFFERSIZE, 256L << 10);
	curl_easy_setopt(curl.handle, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl.handle, CURLOPT_XFERINFOFUNCTION, cancel_curl);
	curl_easy_setopt(curl.handle, CURLOPT_XFERINFODATA, &cancel);
	if (!http_ca_bundle().empty()) curl_easy_setopt(curl.handle, CURLOPT_CAINFO, http_ca_bundle().c_str());
	return true;
}

bool positive_integer(const std::string& text, int64_t& number) {
	if (text.empty()) return false;
	number = 0;
	for (const char ch : text) {
		if (ch < '0' || ch > '9' || number > (INT64_MAX - (ch - '0')) / 10) return false;
		number = number * 10 + ch - '0';
	}
	return true;
}

struct Range {
	int64_t start = -1, end = -1, total = -1;
	bool unsatisfied = false;
};

bool parse_range(const std::string& value, Range& range) {
	if (!starts_with(value, "bytes ")) return false;
	const auto slash = value.find('/');
	if (slash == std::string::npos || !positive_integer(value.substr(slash + 1), range.total) || range.total <= 0) return false;
	if (value.substr(6, slash - 6) == "*") { range.unsatisfied = true; return true; }
	const auto dash = value.find('-', 6);
	return dash != std::string::npos && dash < slash && positive_integer(value.substr(6, dash - 6), range.start) &&
	       positive_integer(value.substr(dash + 1, slash - dash - 1), range.end) && range.end >= range.start && range.end < range.total;
}

bool strong_etag(const std::string& value) {
	return value.size() >= 2 && value.front() == '"' && value.back() == '"' && value.find_first_of("\r\n") == std::string::npos;
}
bool valid_date_validator(const std::string& value) {
	return !value.empty() && value.size() <= 128 && value.find_first_of("\r\n") == std::string::npos && curl_getdate(value.c_str(), nullptr) >= 0;
}

struct ResponseHeaders {
	long status = 0;
	int64_t length = -1;
	std::string etag, modified, mime, content_range, encoding;
	size_t bytes = 0;
	bool invalid = false;
	bool consume(const char* data, size_t count, bool& complete) {
		complete = false;
		if (count > kHeaderLimit - std::min(bytes, kHeaderLimit)) return false;
		bytes += count;
		std::string line(data, count);
		if (starts_with(line, "HTTP/")) {
			const auto space = line.find(' ');
			status = space == std::string::npos ? 0 : std::strtol(line.c_str() + space + 1, nullptr, 10);
			length = -1; etag.clear(); modified.clear(); mime.clear(); content_range.clear(); encoding.clear(); invalid = false;
		} else if (line == "\r\n" || line == "\n") complete = true;
		else {
			const auto colon = line.find(':');
			if (colon == std::string::npos) return true;
			const std::string key = lower(trim(line.substr(0, colon))), value = trim(line.substr(colon + 1));
			if (key == "content-length") {
				int64_t parsed = -1;
				if (!positive_integer(value, parsed) || (length >= 0 && length != parsed)) invalid = true;
				else length = parsed;
			} else if (key == "content-range") content_range = value;
			else if (key == "etag") etag = value;
			else if (key == "last-modified") modified = value;
			else if (key == "content-type") mime = lower(trim(value.substr(0, value.find(';'))));
			else if (key == "content-encoding") encoding = lower(value);
		}
		return true;
	}
};

bool validator_matches(const json& saved, const ResponseHeaders& response, bool require_returned = false) {
	const std::string etag = jstr(saved, "etag"), modified = jstr(saved, "modified");
	if (strong_etag(etag)) return response.etag.empty() ? !require_returned : response.etag == etag;
	if (valid_date_validator(modified)) return response.modified.empty() ? !require_returned : response.modified == modified;
	return false;
}

bool probe_media(const std::string& path, const std::atomic<bool>& cancel) {
	if (cancel.load()) return false;
	AVFormatContext* context = avformat_alloc_context();
	if (!context) return false;
	context->interrupt_callback = {[](void* value) { return static_cast<const std::atomic<bool>*>(value)->load() ? 1 : 0; }, const_cast<std::atomic<bool>*>(&cancel)};
	context->probesize = 2 << 20;
	context->max_analyze_duration = 2 * AV_TIME_BASE;
	AVDictionary* options = nullptr;
	av_dict_set(&options, "protocol_whitelist", "file", 0);
	const int opened = avformat_open_input(&context, path.c_str(), nullptr, &options);
	av_dict_free(&options);
	if (opened < 0) return false;
	avformat_find_stream_info(context, nullptr);
	bool media = false;
	for (unsigned i = 0; i < context->nb_streams; ++i) {
		const auto* p = context->streams[i]->codecpar;
		if ((p->codec_type == AVMEDIA_TYPE_VIDEO || p->codec_type == AVMEDIA_TYPE_AUDIO) && p->codec_id != AV_CODEC_ID_NONE) media = true;
	}
	avformat_close_input(&context);
	return media && !cancel.load();
}

struct HttpDownload {
	const DownloadTransferRequest& request;
	const std::atomic<bool>& cancel;
	Reporter reporter;
	File file;
	ResponseHeaders headers;
	json saved = json::object();
	std::string identity, error, extension = ".media";
	int64_t done = 0, total = -1, requested = 0, response_bytes = 0, response_expected = -1, committed = 0;
	Clock::time_point checkpoint_at = Clock::now();
	TransferTimings timings{"http"};
	bool ready = false, hls = false, restart = false, verified_existing = false;
	DownloadTransferStatus failure = DownloadTransferStatus::Error;
	int storage_error = 0;
	bool fail_storage(int code, const char* phase, const char* fallback = "Could not write the download to storage") {
		if (!storage_error) {
			storage_error = code;
			error = disk_error(code, fallback);
			dlog("download http: storage failed stage=%s errno=%d offset=%lld durable=%lld total=%lld",
			     phase, code, (long long)done, (long long)committed, (long long)total);
			(void)storage_capacity(file.fd, total > 0 ? total - done : -1, "http", phase);
		}
		return false;
	}

	bool checkpoint(bool force = false) {
		if (storage_error) return false;
		if (!force && done == committed) return true;
		const auto start = Clock::now();
		const bool media_saved = ::fsync(file.fd) == 0;
		const int media_error = media_saved ? 0 : file_error();
		timings.media_sync_ms += TransferTimings::millis(start);
		if (!media_saved) { timings.commit_ms += TransferTimings::millis(start); return fail_storage(media_error, "media_sync"); }
		json state{{"version", 1}, {"kind", "http"}, {"source", identity}, {"bytes", done}, {"total", total}, {"etag", headers.etag}, {"modified", headers.modified}, {"extension", extension}};
		const auto state_at = Clock::now();
		const bool saved_ok = save_checkpoint(request, state);
		const int state_error = saved_ok ? 0 : file_error();
		timings.state_sync_ms += TransferTimings::millis(state_at);
		timings.commit_ms += TransferTimings::millis(start);
		if (!saved_ok) return fail_storage(state_error, "state_save", "Could not save the download checkpoint");
		++timings.commits; committed = done; checkpoint_at = Clock::now(); return true;
	}
	~HttpDownload() { timings.emit(done, committed, true); }
	bool prepare() {
		if (headers.status < 200 || (headers.status >= 300 && headers.status < 400)) return true;
		if (headers.invalid) { error = "The source returned an invalid file length"; return false; }
		if (headers.status == 416 && requested > 0) {
			Range range;
			if (parse_range(headers.content_range, range) && range.unsatisfied && range.total == requested && integer(saved, "total") == requested && validator_matches(saved, headers, true)) {
				verified_existing = true; total = done = requested; return false;
			}
			restart = true; return false;
		}
		if (headers.status != 200 && headers.status != 206) { error = "Download source returned HTTP " + std::to_string(headers.status); return false; }
		if (hls_mime(headers.mime)) { hls = true; return false; }
		if (bad_media_mime(headers.mime)) { failure = DownloadTransferStatus::Unsupported; error = "This source does not provide a downloadable media file"; return false; }
		if (!headers.encoding.empty() && headers.encoding != "identity") { error = "The source returned compressed bytes that cannot be safely resumed"; return false; }
		if (headers.status == 206) {
			Range range;
			if (!parse_range(headers.content_range, range) || range.unsatisfied || range.start != requested || (headers.length >= 0 && headers.length != range.end - range.start + 1)) {
				error = "The source returned an incorrect download byte range"; return false;
			}
			if (requested > 0 && (integer(saved, "total") != range.total || !validator_matches(saved, headers))) {
				restart = true; return false;
			}
			total = range.total; response_expected = range.end - range.start + 1;
			if (headers.etag.empty()) headers.etag = jstr(saved, "etag");
			if (headers.modified.empty()) headers.modified = jstr(saved, "modified");
		} else {
			// A failed If-Range or a server ignoring Range is a new entity. Reuse
			// this complete 200 response only after discarding the old partial.
			if (headers.length <= 0) { failure = DownloadTransferStatus::Unsupported; error = "Offline download requires a finite file length; live streams are not supported"; return false; }
			total = response_expected = headers.length;
			if (requested > 0) { if (::ftruncate(file.fd, 0) || ::lseek(file.fd, 0, SEEK_SET) < 0) return fail_storage(file_error(), "restart"); done = 0; }
		}
		if (::lseek(file.fd, done, SEEK_SET) < 0) return fail_storage(file_error(), "media_seek");
		if (!storage_capacity(file.fd, total - done, "http", "preflight")) {
			storage_error = ENOSPC; error = "Insufficient storage for this download"; return false;
		}
		extension = suffix(request.stream.filename.empty() ? request.stream.url : request.stream.filename, headers.mime);
		reporter.reset(done);
		ready = checkpoint(true);
		if (ready) reporter.emit(done, total, true);
		return ready;
	}
	static size_t header_cb(char* data, size_t size, size_t n, void* opaque) {
		auto& self = *static_cast<HttpDownload*>(opaque);
		if (size && n > SIZE_MAX / size) return 0;
		bool complete = false;
		if (!self.headers.consume(data, size * n, complete)) { self.error = "Download response headers are too large"; return 0; }
		if (complete && !self.ready && !self.prepare()) return 0;
		return size * n;
	}
	static int progress_cb(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
		auto& self = *static_cast<HttpDownload*>(opaque);
		if (self.cancel.load()) return 1;
		if (self.ready) self.reporter.heartbeat();
		return 0;
	}
	static size_t write_cb(char* data, size_t size, size_t n, void* opaque) {
		auto& self = *static_cast<HttpDownload*>(opaque);
		if (size && n > SIZE_MAX / size) return 0;
		const size_t bytes = size * n;
		if (self.cancel.load()) return 0;
		if (!self.ready) return self.headers.status >= 300 && self.headers.status < 400 ? bytes : 0;
		if (self.done == 0 && bytes >= 7 && (std::memcmp(data, "#EXTM3U", 7) == 0 || (bytes >= 10 && std::memcmp(data, "\xEF\xBB\xBF#EXTM3U", 10) == 0))) { self.hls = true; return 0; }
		if (bytes > uint64_t(self.total - self.done) || (self.response_expected >= 0 && bytes > uint64_t(self.response_expected - self.response_bytes))) { self.error = "The source sent more bytes than its declared file length"; return 0; }
		const auto start = Clock::now();
		const bool written = write_all(self.file.fd, data, bytes);
		const int write_error = written ? 0 : file_error();
		self.timings.write_ms += TransferTimings::millis(start); ++self.timings.writes;
		if (!written) { self.fail_storage(write_error, "media_write"); return 0; }
		self.done += int64_t(bytes); self.response_bytes += int64_t(bytes);
		self.timings.bytes += int64_t(bytes);
		if ((self.done - self.committed >= kCommitBytes || Clock::now() - self.checkpoint_at >= kCommitInterval) && !self.checkpoint()) return 0;
		self.reporter.emit(self.done, self.total);
		self.timings.emit(self.done, self.committed);
		return bytes;
	}
};

DownloadTransferResult transfer_hls(const DownloadTransferRequest&, const DownloadProgressCallback&, const std::atomic<bool>&);

DownloadTransferResult transfer_http(const DownloadTransferRequest& request, const DownloadProgressCallback& progress, const std::atomic<bool>& cancel) {
	if (!http_valid_url(request.stream.url)) return {DownloadTransferStatus::Unsupported, "This source has no direct HTTP or HTTPS media URL"};
	if (hls_url(request.stream.url)) return transfer_hls(request, progress, cancel);
	HttpDownload run{request, cancel, Reporter{progress}};
	run.identity = source_identity(request.stream);
	if (run.identity.empty()) return {DownloadTransferStatus::Error, "Could not identify the download source"};
	run.file.fd = ::open(request.partial_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
	int64_t file_size = 0;
	if (!regular_fd(run.file.fd, &file_size) || ::fchmod(run.file.fd, 0600)) {
		const int code = file_error();
		return {DownloadTransferStatus::Error, disk_error(code), run.extension, run.done, run.total, code};
	}
	run.saved = read_checkpoint(request);
	run.extension = suffix(jstr(run.saved, "extension"));
	const int64_t saved_bytes = integer(run.saved, "bytes");
	if (request.allow_resume && jstr(run.saved, "kind") == "http" && jstr(run.saved, "source") == run.identity && saved_bytes > 0 && saved_bytes <= file_size && integer(run.saved, "total") >= saved_bytes && (strong_etag(jstr(run.saved, "etag")) || valid_date_validator(jstr(run.saved, "modified")))) {
		run.done = saved_bytes; run.total = integer(run.saved, "total");
	}
	if (::ftruncate(run.file.fd, run.done)) {
		const int code = file_error();
		return {DownloadTransferStatus::Error, disk_error(code), run.extension, run.done, run.total, code};
	}
	for (int attempt = 0; attempt < 128; ++attempt) {
		if (cancel.load()) return {DownloadTransferStatus::Cancelled, {}, run.extension, run.done, run.total};
		CurlHandle curl;
		if (!setup_curl(curl, request.stream.url, request.stream, cancel)) return {DownloadTransferStatus::Error, "Could not initialize the download connection"};
		run.requested = run.done; run.response_bytes = 0; run.response_expected = -1; run.headers = {}; run.ready = false; run.restart = false; run.verified_existing = false;
		std::string range;
		if (run.requested > 0) {
			range = std::to_string(run.requested) + "-";
			curl_easy_setopt(curl.handle, CURLOPT_RANGE, range.c_str());
			const std::string validator = strong_etag(jstr(run.saved, "etag")) ? jstr(run.saved, "etag") : jstr(run.saved, "modified");
			if (!curl.add("If-Range: " + validator)) return {DownloadTransferStatus::Error, "Could not initialize download headers"};
		}
		curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, curl.headers);
		curl_easy_setopt(curl.handle, CURLOPT_HEADERFUNCTION, HttpDownload::header_cb);
		curl_easy_setopt(curl.handle, CURLOPT_HEADERDATA, &run);
		curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, HttpDownload::write_cb);
		curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &run);
		curl_easy_setopt(curl.handle, CURLOPT_XFERINFOFUNCTION, HttpDownload::progress_cb);
		curl_easy_setopt(curl.handle, CURLOPT_XFERINFODATA, &run);
		const CURLcode result = curl_easy_perform(curl.handle);
		if (run.hls) { run.file.close(); return transfer_hls(request, progress, cancel); }
		if (run.storage_error || (run.ready && !run.checkpoint()))
			return {DownloadTransferStatus::Error, run.error, run.extension, run.done, run.total, run.storage_error};
		if (cancel.load()) return {DownloadTransferStatus::Cancelled, {}, run.extension, run.done, run.total};
		if (run.restart) {
			if (run.requested == 0) return {DownloadTransferStatus::Error, "The download source cannot resume this file safely"};
			if (::ftruncate(run.file.fd, 0)) {
				const int code = file_error();
				return {DownloadTransferStatus::Error, disk_error(code), run.extension, run.done, run.total, code};
			}
			run.done = run.committed = 0; run.saved = json::object(); run.error.clear(); continue;
		}
		if (!run.verified_existing && (!run.error.empty() || result != CURLE_OK || !run.ready || run.response_bytes != run.response_expected)) {
			const std::string error = !run.error.empty() ? run.error : result == CURLE_OPERATION_TIMEDOUT ? "The download source stopped responding" : "The file download was interrupted before all bytes arrived";
			return {run.failure, error, run.extension, run.done, run.total};
		}
		if (run.done < run.total) {
			run.saved = read_checkpoint(request);
			if (!strong_etag(jstr(run.saved, "etag")) && !valid_date_validator(jstr(run.saved, "modified")))
				return {DownloadTransferStatus::Error, "The source split the file into ranges without a safe resume validator", run.extension, run.done, run.total};
			continue;
		}
		if (!regular_fd(run.file.fd, &file_size) || ::fsync(run.file.fd)) {
			const int code = file_error();
			return {DownloadTransferStatus::Error, disk_error(code, "Could not finalize the complete downloaded file"), run.extension, run.done, run.total, code};
		}
		if (run.done != run.total || file_size != run.total) return {DownloadTransferStatus::Error, "Could not finalize the complete downloaded file", run.extension, run.done, run.total};
		if (!run.file.close()) {
			const int code = file_error();
			return {DownloadTransferStatus::Error, disk_error(code, "Could not finalize the complete downloaded file"), run.extension, run.done, run.total, code};
		}
		if (!probe_media(request.partial_path, cancel)) return {cancel.load() ? DownloadTransferStatus::Cancelled : DownloadTransferStatus::Unsupported, "The downloaded file does not contain playable audio or video", run.extension, run.done, run.total};
		run.reporter.emit(run.done, run.total, true);
		return {DownloadTransferStatus::Complete, {}, run.extension, run.done, run.total};
	}
	return {DownloadTransferStatus::Error, "The source returned too many incomplete byte ranges", run.extension, run.done, run.total};
}

// Engine::read waits for verified pieces. This guard supplies cancellation and
// a bounded no-progress deadline even when the swarm has stopped answering.
struct TorrentDeadline {
	std::atomic<bool> abort{false}, expired{false};
	std::mutex mutex;
	std::condition_variable wake;
	bool finished = false, waiting_for_peers = true;
	Clock::time_point last = Clock::now();
	std::thread thread;
	explicit TorrentDeadline(const std::atomic<bool>& cancel, Reporter& reporter) : thread([this, &cancel, &reporter] {
		std::unique_lock<std::mutex> lock(mutex);
		auto reported = Clock::now();
		while (!finished) {
			if (cancel.load()) { abort = true; break; }
			if (waiting_for_peers && Clock::now() - last > std::chrono::seconds(kStallSeconds)) { expired = true; abort = true; break; }
			if (Clock::now() - reported >= std::chrono::seconds(1)) {
				lock.unlock(); reporter.heartbeat(); lock.lock(); reported = Clock::now();
			}
			wake.wait_for(lock, std::chrono::milliseconds(100));
		}
	}) {}
	void touch() { std::lock_guard<std::mutex> lock(mutex); last = Clock::now(); }
	void waiting(bool value) {
		std::lock_guard<std::mutex> lock(mutex);
		waiting_for_peers = value;
		if (value) last = Clock::now();
	}
	~TorrentDeadline() { { std::lock_guard<std::mutex> lock(mutex); finished = true; } wake.notify_all(); if (thread.joinable()) thread.join(); }
};

DownloadTransferResult transfer_torrent(const DownloadTransferRequest& request, const DownloadProgressCallback& progress, const std::atomic<bool>& cancel) {
	const std::string hash = lower(request.stream.info_hash);
	if (hash.size() != 40 || hash.find_first_not_of("0123456789abcdef") != std::string::npos) return {DownloadTransferStatus::Unsupported, "This source does not provide a valid torrent hash"};
	if (cancel.load()) return {DownloadTransferStatus::Cancelled};
	Reporter reporter{progress};
	TorrentDeadline deadline(cancel, reporter);
	auto& engine = bt::Engine::get();
	engine.start(hash, request.stream.sources);
	std::vector<bt::FileInfo> files;
	std::string engine_error;
	if (!engine.wait_metadata(hash, files, &deadline.abort, 90, &engine_error)) return {cancel.load() ? DownloadTransferStatus::Cancelled : DownloadTransferStatus::Error, "Could not obtain the torrent file list"};
	// Metadata discovery has its own timeout. Give the first verified pieces
	// the full no-progress interval after that discovery and storage setup.
	deadline.waiting(false);
	const int index = request.stream.file_idx >= 0 ? request.stream.file_idx : bt::Engine::guess_file(files, request.season, request.episode);
	if (index < 0 || size_t(index) >= files.size() || files[size_t(index)].size <= 0) return {DownloadTransferStatus::Unsupported, "The selected file is not available in this torrent"};
	struct Reader {
		std::shared_ptr<bt::Torrent> torrent;
		int id = 0;
		~Reader() { if (torrent) bt::Engine::get().close_reader(torrent, id); }
	} reader;
	int64_t total = 0;
	reader.torrent = engine.open_reader(hash, index, &reader.id, &total, &engine_error, bt::ReaderRole::Download);
	if (!reader.torrent || total <= 0 || total != files[size_t(index)].size) return {DownloadTransferStatus::Error, "Could not open the selected torrent file"};
	const json saved = read_checkpoint(request);
	const std::string identity = hash + ":" + std::to_string(index);
	const std::string extension = suffix(files[size_t(index)].path);
	int64_t done = 0;
	const int64_t saved_bytes = integer(saved, "bytes");
	if (request.allow_resume && jstr(saved, "kind") == "torrent" && jstr(saved, "source") == identity && integer(saved, "total") == total && saved_bytes >= 0 && saved_bytes <= total) done = saved_bytes;
	File file;
	int64_t size = 0;
#if defined(PLATFORM_PS5_NATIVE) || defined(STREMIO_DOWNLOAD_WRITER_TEST)
	TorrentWriter writer(-1);
	if (!writer.buffer) return {DownloadTransferStatus::Error, "Could not allocate the download write buffer"};
	if (!writer.start_helper(request.work_dir, done, total)) {
		const int code = writer.helper ? writer.helper->error() : file_error();
		const int storage_error = writer.helper ? helper_storage_error(*writer.helper) : 0;
		dlog("download writer: launch failed error=%d stage=%s", code,
		     writer.helper ? download_writer::wire::stage_name(writer.helper->error_stage()) : "transport");
		return {DownloadTransferStatus::Error, storage_error ? disk_error(storage_error) : "Could not start the PS5 download writer", extension, done, total, storage_error};
	}
	// This descriptor observes the same destination inode throughout the
	// transfer, including after the helper reports a terminal write error.
	file.fd = ::open(request.partial_path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	if (file.fd < 0) {
		const int code = file_error();
		dlog("download capacity: destination descriptor unavailable errno=%d", code);
	}
	dlog("download writer: ready protocol=%u resume=%lld total=%lld",
	     unsigned(download_writer::wire::version), (long long)done, (long long)total);
	const char* storage_mode = "payload_writer";
#else
	file.fd = ::open(request.partial_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
	if (!regular_fd(file.fd, &size) || ::fchmod(file.fd, 0600)) {
		const int code = file_error();
		return {DownloadTransferStatus::Error, disk_error(code), extension, done, total, code};
	}
	if (done > size) done = 0;
	if (::ftruncate(file.fd, done) || ::lseek(file.fd, done, SEEK_SET) < 0) {
		const int code = file_error();
		return {DownloadTransferStatus::Error, disk_error(code), extension, done, total, code};
	}
	TorrentWriter writer(file.fd);
	if (!writer.buffer) return {DownloadTransferStatus::Error, "Could not allocate the download write buffer"};
	const char* storage_mode = "buffered";
#endif
	if (!storage_capacity(file.fd, total - done, storage_mode, "preflight"))
		return {DownloadTransferStatus::Error, "Insufficient storage for this download", extension, done, total, ENOSPC};
	reporter.torrent(hash, index); reporter.reset(done);
	int64_t committed = done;
	int storage_error = 0;
	bool output_failed = false;
	std::string error;
	auto checkpoint_at = Clock::now();
	TransferTimings timings{"torrent"};
	auto checkpoint = [&](bool force = false) {
		if (!force && done == committed) return true;
		const auto start = Clock::now();
		const json state{{"version", 1}, {"kind", "torrent"}, {"source", identity}, {"bytes", done}, {"total", total}, {"extension", extension}};
#if defined(PLATFORM_PS5_NATIVE) || defined(STREMIO_DOWNLOAD_WRITER_TEST)
		download_writer::CommitTimes commit_times;
		const bool saved_ok = writer.helper->checkpoint(state.dump(), commit_times);
		const int code = saved_ok ? 0 : writer.helper->error();
		timings.media_sync_ms += commit_times.media_ms;
		timings.state_sync_ms += commit_times.state_ms;
		if (!saved_ok) {
			storage_error = helper_storage_error(*writer.helper);
			error = storage_error ? disk_error(storage_error, "Could not save the torrent download checkpoint") : "Could not save the torrent download checkpoint";
			dlog("download checkpoint: failed stage=%s errno=%d offset=%lld total=%lld initial=%d",
			     download_writer::wire::stage_name(writer.helper->error_stage()), code,
			     (long long)done, (long long)total, int(force));
		}
#else
		const bool media_saved = ::fsync(file.fd) == 0;
		const int media_error = media_saved ? 0 : file_error();
		timings.media_sync_ms += TransferTimings::millis(start);
		const auto state_at = Clock::now();
		const bool saved_ok = media_saved && save_checkpoint(request, state);
		const int code = saved_ok ? 0 : media_error ? media_error : file_error();
		timings.state_sync_ms += TransferTimings::millis(state_at);
		if (!saved_ok) {
			storage_error = code;
			error = disk_error(code, "Could not save the torrent download checkpoint");
			dlog("download checkpoint: failed stage=%s errno=%d offset=%lld total=%lld initial=%d",
			     media_saved ? "state_save" : "media_sync", code, (long long)done, (long long)total, int(force));
		}
#endif
		timings.commit_ms += TransferTimings::millis(start);
		if (!saved_ok) {
			output_failed = true;
			(void)storage_capacity(file.fd, total - done, storage_mode, "checkpoint_failure");
			return false;
		}
		++timings.commits; committed = done; checkpoint_at = Clock::now(); return true;
	};
	if (!checkpoint(true)) return {DownloadTransferStatus::Error, error, extension, done, total, storage_error};
	deadline.touch();
	reporter.emit(done, total, true);
	dlog("download torrent: start file=%d total=%lld resume=%lld", index, (long long)total, (long long)done);
#if defined(PLATFORM_PS5_NATIVE) || defined(STREMIO_DOWNLOAD_WRITER_TEST)
	dlog("download storage: mode=payload_writer protocol=%u write_buffer=%zu alignment=%zu checkpoint_bytes=%lld checkpoint_seconds=%lld",
	     unsigned(download_writer::wire::version), kTorrentWriteBytes, kWriteAlignment,
	     (long long)kTorrentCommitBytes, (long long)kTorrentCommitInterval.count());
#else
	dlog("download storage: mode=buffered write_buffer=%zu alignment=%zu checkpoint_bytes=%lld checkpoint_seconds=%lld",
	     kTorrentWriteBytes, kWriteAlignment, (long long)kTorrentCommitBytes, (long long)kTorrentCommitInterval.count());
	struct stat storage{};
	if (::fstat(file.fd, &storage) == 0)
		dlog("download storage: device=%llu block_size=%lld", (unsigned long long)storage.st_dev, (long long)storage.st_blksize);
#endif
	while (done < total && !deadline.abort.load()) {
		const size_t capacity = writer.capacity(std::min(total - done, kTorrentCommitBytes - (done - committed)));
		size_t buffered = 0;
		const auto batch_at = Clock::now();
		while (buffered < capacity && !deadline.abort.load()) {
			int wanted = int(capacity - buffered);
			if (buffered && Clock::now() - batch_at >= std::chrono::milliseconds(200)) {
				const size_t tail = buffered % kWriteAlignment;
				if (!tail) break;
				wanted = std::min(wanted, int(kWriteAlignment - tail));
			}
			const auto read_at = Clock::now();
			deadline.waiting(true);
			const int count = engine.read(reader.torrent, reader.id, index, done + int64_t(buffered), writer.buffer.get() + buffered, wanted, &deadline.abort);
			deadline.waiting(false);
			timings.read_ms += TransferTimings::millis(read_at); ++timings.reads;
			if (count <= 0 || count > wanted) { error = "The torrent download stopped before the file was complete"; break; }
			buffered += size_t(count);
		}
		if (!buffered) break;
		const auto write_at = Clock::now();
		const bool written = writer.write(buffered);
		const int write_error = written ? 0 : file_error();
		timings.write_ms += TransferTimings::millis(write_at); ++timings.writes;
#if defined(PLATFORM_PS5_NATIVE) || defined(STREMIO_DOWNLOAD_WRITER_TEST)
		timings.helper_write_ms += writer.helper->last_write_ms();
#endif
		if (!written) {
			output_failed = true;
#if defined(PLATFORM_PS5_NATIVE) || defined(STREMIO_DOWNLOAD_WRITER_TEST)
			storage_error = helper_storage_error(*writer.helper);
			error = storage_error ? disk_error(storage_error) : "The PS5 download writer stopped before confirming the data";
			dlog("download writer: write failed error=%d stage=%s offset=%lld durable=%lld total=%lld",
			     write_error, download_writer::wire::stage_name(writer.helper->error_stage()),
			     (long long)done, (long long)committed, (long long)total);
#else
			storage_error = write_error;
			error = disk_error(write_error);
			dlog("download writer: write failed error=%d stage=media_write offset=%lld durable=%lld total=%lld",
			     write_error, (long long)done, (long long)committed, (long long)total);
#endif
			(void)storage_capacity(file.fd, total - done, storage_mode, "write_failure");
			break;
		}
		done += int64_t(buffered); timings.bytes += int64_t(buffered); reporter.emit(done, total);
		if (done - committed >= kTorrentCommitBytes || Clock::now() - checkpoint_at >= kTorrentCommitInterval) {
			if (!checkpoint()) break;
			// Time spent syncing local storage is not a stalled remote peer.
			deadline.touch();
		}
		timings.emit(done, committed);
		if (!error.empty()) break;
	}
	// A failed media write has already ended the helper session. Preserve the
	// preceding durable checkpoint and its original error instead of attempting
	// another checkpoint and misreporting the write failure as a save failure.
	bool final_saved = !output_failed && checkpoint();
#if defined(PLATFORM_PS5_NATIVE) || defined(STREMIO_DOWNLOAD_WRITER_TEST)
	// A lost reply never permits native writes or an unconfirmed file promotion.
	// The next helper must acquire the same directory lock before resuming.
	if (final_saved && !writer.helper->close()) {
		const int code = writer.helper->error();
		storage_error = helper_storage_error(*writer.helper);
		error = storage_error ? disk_error(storage_error, "Could not finalize the torrent download") : "Could not finalize the torrent download";
		dlog("download writer: close failed error=%d offset=%lld", code, (long long)done);
		(void)storage_capacity(file.fd, total - done, storage_mode, "close_failure");
		final_saved = false;
	}
#endif
	timings.emit(done, committed, true);
	if (!final_saved) return {DownloadTransferStatus::Error, error, extension, done, total, storage_error};
	if (cancel.load()) return {DownloadTransferStatus::Cancelled, {}, extension, done, total};
	if (deadline.expired.load()) {
		dlog("download torrent: no-progress timeout file=%d saved=%lld total=%lld timeout=%ds",
		     index, (long long)done, (long long)total, kStallSeconds);
		return {DownloadTransferStatus::Error, "The torrent peers stopped sending data", extension, done, total};
	}
	if (!error.empty() || done != total) return {DownloadTransferStatus::Error, error.empty() ? "The torrent download is incomplete" : error, extension, done, total};
#if defined(PLATFORM_PS5_NATIVE) || defined(STREMIO_DOWNLOAD_WRITER_TEST)
	if (file.fd < 0) file.fd = ::open(request.partial_path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
#endif
	if (!regular_fd(file.fd, &size)) {
		const int code = file_error();
		return {DownloadTransferStatus::Error, disk_error(code, "Could not finalize the torrent download"), extension, done, total, code};
	}
	if (size != total) return {DownloadTransferStatus::Error, "Could not finalize the torrent download", extension, done, total};
	if (!file.close()) {
		const int code = file_error();
		return {DownloadTransferStatus::Error, disk_error(code, "Could not finalize the torrent download"), extension, done, total, code};
	}
	if (!probe_media(request.partial_path, cancel)) return {cancel.load() ? DownloadTransferStatus::Cancelled : DownloadTransferStatus::Unsupported, "The selected torrent file does not contain playable audio or video", extension, done, total};
	reporter.emit(done, total, true);
	return {DownloadTransferStatus::Complete, {}, extension, done, total};
}

// HLS is saved as one self-contained Matroska container. Frozen ENDLIST
// playlists prevent a changing live manifest from turning into an endless job.
struct ManifestFetch {
	std::string body;
	ResponseHeaders headers;
	bool too_large = false;
	static size_t write(char* data, size_t size, size_t count, void* opaque) {
		auto& self = *static_cast<ManifestFetch*>(opaque);
		if ((size && count > SIZE_MAX / size) || size * count > kManifestLimit - std::min(kManifestLimit, self.body.size())) { self.too_large = true; return 0; }
		self.body.append(data, size * count); return size * count;
	}
	static size_t header(char* data, size_t size, size_t count, void* opaque) {
		auto& self = *static_cast<ManifestFetch*>(opaque);
		if (size && count > SIZE_MAX / size) return 0;
		bool complete = false;
		if (count * size >= 5 && std::memcmp(data, "HTTP/", 5) == 0) self.body.clear();
		return self.headers.consume(data, size * count, complete) ? size * count : 0;
	}
};

std::string resolve_url(const std::string& base, const std::string& relative) {
	CURLU* url = curl_url();
	if (!url) return {};
	char* resolved = nullptr;
	std::string result;
	if (!curl_url_set(url, CURLUPART_URL, base.c_str(), 0) && !curl_url_set(url, CURLUPART_URL, relative.c_str(), 0) && !curl_url_get(url, CURLUPART_URL, &resolved, 0) && resolved) result = resolved;
	curl_free(resolved); curl_url_cleanup(url);
	return http_valid_url(result) ? result : std::string{};
}

std::map<std::string, std::string> attributes(const std::string& line) {
	std::map<std::string, std::string> values;
	const auto colon = line.find(':');
	if (colon == std::string::npos) return values;
	size_t at = colon + 1;
	while (at < line.size()) {
		const auto equal = line.find('=', at);
		if (equal == std::string::npos) break;
		const std::string key = trim(line.substr(at, equal - at));
		at = equal + 1;
		std::string value;
		if (at < line.size() && line[at] == '"') {
			const auto end = line.find('"', ++at);
			if (end == std::string::npos) return {};
			value = line.substr(at, end - at); at = end + 1;
		} else {
			const auto end = line.find(',', at);
			value = trim(line.substr(at, end == std::string::npos ? end : end - at));
			at = end == std::string::npos ? line.size() : end;
		}
		values[key] = value;
		if (at < line.size() && line[at] == ',') ++at;
	}
	return values;
}

struct HlsSet {
	std::map<std::string, std::shared_ptr<const std::string>> snapshots;
	std::set<std::string> active;
	std::string root_url, error;
	size_t bytes = 0;
	double max_duration = 0;
	DownloadTransferStatus status = DownloadTransferStatus::Unsupported;
	bool collect(const std::string& url, const Stream& stream, const std::atomic<bool>& cancel, int depth = 0) {
		if (cancel.load()) { status = DownloadTransferStatus::Cancelled; return false; }
		if (active.count(url)) { error = "The HLS playlist contains a recursive reference"; return false; }
		if (snapshots.count(url)) return true;
		if (depth > 4 || snapshots.size() >= 96 || bytes >= kManifestSetLimit) { error = "This HLS playlist is too complex for an offline download"; return false; }
		CurlHandle curl;
		if (!setup_curl(curl, url, stream, cancel)) { error = "Could not open the HLS playlist"; status = DownloadTransferStatus::Error; return false; }
		ManifestFetch fetch;
		curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, curl.headers);
		curl_easy_setopt(curl.handle, CURLOPT_TIMEOUT, 30L);
		curl_easy_setopt(curl.handle, CURLOPT_HEADERFUNCTION, ManifestFetch::header);
		curl_easy_setopt(curl.handle, CURLOPT_HEADERDATA, &fetch);
		curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, ManifestFetch::write);
		curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &fetch);
		const CURLcode code = curl_easy_perform(curl.handle);
		if (cancel.load()) { status = DownloadTransferStatus::Cancelled; return false; }
		if (code != CURLE_OK || fetch.headers.status != 200 || fetch.too_large) { error = "Could not retrieve the complete HLS playlist"; status = DownloadTransferStatus::Error; return false; }
		char* effective = nullptr; curl_easy_getinfo(curl.handle, CURLINFO_EFFECTIVE_URL, &effective);
		const std::string base = effective && http_valid_url(effective) ? effective : url;
		if (depth == 0) root_url = base;
		std::string body = fetch.body;
		if (starts_with(body, "\xEF\xBB\xBF")) body.erase(0, 3);
		if (!starts_with(trim(body), "#EXTM3U")) { error = "This source did not return an HLS playlist"; return false; }
		bytes += body.size();
		if (bytes > kManifestSetLimit) { error = "The HLS playlists exceed the offline manifest limit"; return false; }
		auto frozen = std::make_shared<const std::string>(std::move(body));
		snapshots[url] = frozen; snapshots[base] = frozen;
		active.insert(url); active.insert(base);
		bool master = false, expect_playlist = false, expect_segment = false, ended = false;
		int segments = 0;
		double duration = 0;
		std::vector<std::string> children;
		for (const auto& untrimmed : split(*frozen, '\n')) {
			const std::string line = trim(untrimmed);
			if (line.empty()) continue;
			if (starts_with(line, "#EXT-X-KEY:") || starts_with(line, "#EXT-X-SESSION-KEY:")) {
				const auto values = attributes(line);
				const auto method = values.find("METHOD");
				if (method == values.end() || method->second != "NONE") { error = "Encrypted HLS and DRM sources are not supported for offline downloads"; return false; }
			} else if (line == "#EXT-X-ENDLIST") ended = true;
			else if (starts_with(line, "#EXT-X-STREAM-INF:")) { master = true; expect_playlist = true; }
			else if (starts_with(line, "#EXT-X-MEDIA:")) {
				const auto values = attributes(line); const auto uri = values.find("URI");
				if (uri != values.end()) { master = true; const std::string child = resolve_url(base, uri->second); if (child.empty()) { error = "The HLS playlist uses an unsupported media URL"; return false; } children.push_back(child); }
			} else if (starts_with(line, "#EXT-X-MAP:")) {
				const auto values = attributes(line); const auto uri = values.find("URI");
				if (uri == values.end() || resolve_url(base, uri->second).empty()) { error = "The HLS initialization segment has an unsupported URL"; return false; }
			} else if (starts_with(line, "#EXTINF:")) {
				if (expect_segment) { error = "The HLS playlist is missing a media segment"; return false; }
				const std::string value = line.substr(8, line.find(',') == std::string::npos ? std::string::npos : line.find(',') - 8);
				char* end = nullptr; errno = 0; const double seconds = std::strtod(value.c_str(), &end);
				if (errno || !end || *end || !std::isfinite(seconds) || seconds <= 0 || seconds > 86400 || duration > 1e9 - seconds) { error = "The HLS playlist contains an invalid segment duration"; return false; }
				duration += seconds; expect_segment = true;
			} else if (line.front() != '#') {
				const std::string child = resolve_url(base, line);
				if (child.empty()) { error = "The HLS playlist uses an unsupported segment URL"; return false; }
				if (expect_playlist) { children.push_back(child); expect_playlist = false; }
				else {
					if (!expect_segment) { error = "The HLS playlist has a segment without a finite duration"; return false; }
					++segments; expect_segment = false;
				}
			}
		}
		if (expect_playlist || expect_segment || (master && children.empty())) { error = "The HLS master playlist is incomplete"; return false; }
		if (!master && (!ended || segments <= 0 || duration <= 0)) { error = "Live HLS streams cannot be downloaded for offline viewing"; return false; }
		max_duration = std::max(max_duration, duration);
		for (const auto& child : children) if (!collect(child, stream, cancel, depth + 1)) return false;
		active.erase(url); active.erase(base);
		return true;
	}
};

struct MemoryInput {
	std::shared_ptr<const std::string> bytes;
	int64_t position = 0;
	static int read(void* opaque, uint8_t* buffer, int count) {
		auto& self = *static_cast<MemoryInput*>(opaque);
		if (self.position >= int64_t(self.bytes->size())) return AVERROR_EOF;
		const int size = int(std::min<int64_t>(count, int64_t(self.bytes->size()) - self.position));
		std::memcpy(buffer, self.bytes->data() + self.position, size_t(size)); self.position += size; return size;
	}
	static int64_t seek(void* opaque, int64_t offset, int whence) {
		auto& self = *static_cast<MemoryInput*>(opaque);
		if (whence == AVSEEK_SIZE) return int64_t(self.bytes->size());
		whence &= ~AVSEEK_FORCE;
		const int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? self.position : whence == SEEK_END ? int64_t(self.bytes->size()) : -1;
		if (base < 0 || (offset > 0 && base > INT64_MAX - offset) || (offset < 0 && offset < -base)) return AVERROR(EINVAL);
		const int64_t next = base + offset;
		if (next < 0 || next > int64_t(self.bytes->size())) return AVERROR(EINVAL);
		self.position = next; return next;
	}
	static AVIOContext* open(std::shared_ptr<const std::string> bytes) {
		auto input = std::make_unique<MemoryInput>(); input->bytes = std::move(bytes);
		auto* buffer = static_cast<uint8_t*>(av_malloc(32u << 10));
		if (!buffer) return nullptr;
		AVIOContext* context = avio_alloc_context(buffer, 32u << 10, 0, input.get(), read, nullptr, seek);
		if (!context) { av_free(buffer); return nullptr; }
		input.release(); return context;
	}
	static bool owns(AVIOContext* context) { return context && context->read_packet == read; }
	static void close(AVIOContext** context) {
		if (!context || !*context) return;
		delete static_cast<MemoryInput*>((*context)->opaque);
		av_freep(&(*context)->buffer); avio_context_free(context);
	}
};

struct HlsHooks : NetStream::IoHooks {
	const HlsSet* manifests = nullptr;
	decltype(AVFormatContext::io_open) network_open = nullptr;
	decltype(AVFormatContext::io_close2) network_close = nullptr;
	bool failed = false;
	static HlsHooks& of(AVFormatContext* context) { return *static_cast<HlsHooks*>(static_cast<NetStream::IoHooks*>(context->opaque)); }
	static int open(AVFormatContext* context, AVIOContext** pb, const char* url, int flags, AVDictionary** options) {
		auto& self = of(context);
		if ((flags & AVIO_FLAG_WRITE) || !url || !http_valid_url(url)) { self.failed = true; return AVERROR(EACCES); }
		const auto found = self.manifests->snapshots.find(url);
		if (found != self.manifests->snapshots.end()) { *pb = MemoryInput::open(found->second); if (!*pb) self.failed = true; return *pb ? 0 : AVERROR(ENOMEM); }
		const int result = self.network_open(context, pb, url, flags, options);
		if (result < 0) self.failed = true;
		return result;
	}
	static int close(AVFormatContext* context, AVIOContext* pb) {
		auto& self = of(context);
		if (MemoryInput::owns(pb)) { MemoryInput::close(&pb); return 0; }
		if (NetStream::is_ours(pb)) {
			const std::string failure = NetStream::of(pb)->failure();
			if (!failure.empty() || (pb->error < 0 && pb->error != AVERROR_EOF && pb->error != AVERROR_EXIT)) self.failed = true;
		}
		return self.network_close(context, pb);
	}
};

struct OutputIo {
	int fd = -1;
	const std::atomic<bool>* cancel = nullptr;
	int64_t length = 0;
	bool failed = false;
	int storage_error = 0;
	int fail_storage(int code) {
		failed = true;
		if (!storage_error) {
			storage_error = code;
			dlog("download hls: storage failed errno=%d length=%lld", code, (long long)length);
			(void)storage_capacity(fd, -1, "hls", "write_failure");
		}
		return AVERROR(storage_error);
	}
	static int write(void* opaque,
#if LIBAVFORMAT_VERSION_MAJOR >= 61
	                 const uint8_t* data,
#else
	                 uint8_t* data,
#endif
	                 int count) {
		auto& self = *static_cast<OutputIo*>(opaque);
		if (self.cancel->load()) return AVERROR_EXIT;
		if (count < 0) { self.failed = true; return AVERROR(EINVAL); }
		if (!write_all(self.fd, data, size_t(count))) return self.fail_storage(file_error());
		const auto at = ::lseek(self.fd, 0, SEEK_CUR);
		if (at < 0) return self.fail_storage(file_error());
		self.length = std::max(self.length, int64_t(at)); return count;
	}
	static int64_t seek(void* opaque, int64_t offset, int whence) {
		auto& self = *static_cast<OutputIo*>(opaque);
		if (whence == AVSEEK_SIZE) return self.length;
		const auto next = ::lseek(self.fd, offset, whence & ~AVSEEK_FORCE);
		if (next < 0) return self.fail_storage(file_error());
		return int64_t(next);
	}
};

DownloadTransferResult transfer_hls(const DownloadTransferRequest& request, const DownloadProgressCallback& progress, const std::atomic<bool>& cancel) {
	HlsSet manifests;
	Reporter reporter{progress}; reporter.emit(0, -1, true);
	if (!manifests.collect(request.stream.url, request.stream, cancel)) return {manifests.status, manifests.error, ".mkv"};
	if (cancel.load()) return {DownloadTransferStatus::Cancelled, {}, ".mkv"};
	// Segment timestamps and muxer indexes cannot be resumed by appending bytes.
	// Restart a paused HLS job from its beginning; direct files and torrents have
	// a separate, identity-checked byte resume path.
	File file(::open(request.partial_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600));
	if (!regular_fd(file.fd) || ::fchmod(file.fd, 0600) || ::fsync(file.fd)) {
		const int code = file_error();
		return {DownloadTransferStatus::Error, disk_error(code), ".mkv", 0, -1, code};
	}
	if (!save_checkpoint(request, json{{"version", 1}, {"kind", "hls"}, {"bytes", 0}, {"total", -1}})) {
		const int code = file_error();
		return {DownloadTransferStatus::Error, disk_error(code, "Could not save the HLS download checkpoint"), ".mkv", 0, -1, code};
	}
	HlsHooks hooks; hooks.manifests = &manifests; hooks.headers = request.stream.request_headers; hooks.abort = &cancel;
	AVFormatContext* input = avformat_alloc_context();
	AVFormatContext* output = nullptr;
	AVIOContext* main_io = nullptr;
	AVIOContext* output_io = nullptr;
	AVPacket* packet = nullptr;
	OutputIo writer{file.fd, &cancel};
	auto cleanup = [&] {
		av_packet_free(&packet);
		if (input) avformat_close_input(&input);
		MemoryInput::close(&main_io);
		if (output) { output->pb = nullptr; avformat_free_context(output); output = nullptr; }
		if (output_io) { av_freep(&output_io->buffer); avio_context_free(&output_io); }
	};
	auto fail = [&](DownloadTransferStatus status, const std::string& message) {
		cleanup();
		if (!writer.storage_error && ::fsync(file.fd) != 0) writer.fail_storage(file_error());
		if (writer.storage_error)
			return DownloadTransferResult{DownloadTransferStatus::Error, disk_error(writer.storage_error), ".mkv", writer.length, -1, writer.storage_error};
		return DownloadTransferResult{cancel.load() ? DownloadTransferStatus::Cancelled : status, cancel.load() ? "" : message, ".mkv", writer.length, -1};
	};
	if (!input) return fail(DownloadTransferStatus::Error, "Could not allocate the HLS reader");
	input->interrupt_callback = {[](void* value) { return static_cast<const std::atomic<bool>*>(value)->load() ? 1 : 0; }, const_cast<std::atomic<bool>*>(&cancel)};
	input->probesize = 5 << 20; input->max_analyze_duration = 5 * AV_TIME_BASE;
	NetStream::install(input, &hooks);
	hooks.network_open = input->io_open; hooks.network_close = input->io_close2;
	input->io_open = HlsHooks::open; input->io_close2 = HlsHooks::close;
	main_io = MemoryInput::open(manifests.snapshots.at(manifests.root_url));
	if (!main_io) return fail(DownloadTransferStatus::Error, "Could not allocate the HLS playlist reader");
	input->pb = main_io; input->flags |= AVFMT_FLAG_CUSTOM_IO;
	AVDictionary* options = nullptr;
	av_dict_set(&options, "http_persistent", "0", 0); av_dict_set(&options, "http_multiple", "0", 0);
	av_dict_set(&options, "allowed_extensions", "ALL", 0);
	av_dict_set(&options, "protocol_whitelist", "http,https,tcp,tls", 0);
	const AVInputFormat* format = av_find_input_format("hls");
	int result = format ? avformat_open_input(&input, manifests.root_url.c_str(), format, &options) : AVERROR_DEMUXER_NOT_FOUND;
	av_dict_free(&options);
	if (result < 0 || !input) return fail(DownloadTransferStatus::Error, "Could not open this HLS video");
	if (avformat_find_stream_info(input, nullptr) < 0 || hooks.failed || cancel.load()) return fail(DownloadTransferStatus::Error, "Could not read all HLS media tracks");
	int best_video = -1;
	int64_t best_area = -1;
	for (unsigned i = 0; i < input->nb_streams; ++i) {
		const auto* par = input->streams[i]->codecpar;
		const int64_t area = int64_t(std::max(0, par->width)) * std::max(0, par->height);
		if (par->codec_type == AVMEDIA_TYPE_VIDEO && area > best_area) { best_area = area; best_video = int(i); }
	}
	std::set<unsigned> selected;
	if (best_video >= 0) {
		for (unsigned p = 0; p < input->nb_programs && selected.empty(); ++p) {
			const auto* program = input->programs[p];
			bool found = false;
			for (unsigned i = 0; i < program->nb_stream_indexes; ++i) if (int(program->stream_index[i]) == best_video) found = true;
			if (found) for (unsigned i = 0; i < program->nb_stream_indexes; ++i) selected.insert(program->stream_index[i]);
		}
	}
	if (selected.empty()) for (unsigned i = 0; i < input->nb_streams; ++i) selected.insert(i);
	if (avformat_alloc_output_context2(&output, nullptr, "matroska", nullptr) < 0 || !output) return fail(DownloadTransferStatus::Unsupported, "The Matroska offline container is unavailable");
	std::vector<int> mapping(input->nb_streams, -1);
	bool media = false;
	for (unsigned i = 0; i < input->nb_streams; ++i) {
		AVStream* from = input->streams[i];
		const AVMediaType type = from->codecpar->codec_type;
		const bool wanted = selected.count(i) && ((type == AVMEDIA_TYPE_VIDEO && int(i) == best_video) || type == AVMEDIA_TYPE_AUDIO || type == AVMEDIA_TYPE_SUBTITLE);
		if (!wanted) { from->discard = AVDISCARD_ALL; continue; }
		if (avformat_query_codec(output->oformat, from->codecpar->codec_id, FF_COMPLIANCE_NORMAL) == 0) return fail(DownloadTransferStatus::Unsupported, "An HLS audio or subtitle track cannot be preserved in the offline container");
		AVStream* to = avformat_new_stream(output, nullptr);
		if (!to || avcodec_parameters_copy(to->codecpar, from->codecpar) < 0) return fail(DownloadTransferStatus::Error, "Could not copy the HLS media tracks");
		to->codecpar->codec_tag = 0; to->time_base = from->time_base; to->disposition = from->disposition;
		av_dict_copy(&to->metadata, from->metadata, 0);
		mapping[i] = to->index; media |= type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO;
	}
	if (!media) return fail(DownloadTransferStatus::Unsupported, "This HLS source contains no downloadable audio or video");
	av_dict_copy(&output->metadata, input->metadata, 0);
	output->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;
	output->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_AUTO_BSF;
	auto* buffer = static_cast<uint8_t*>(av_malloc(64u << 10));
	if (!buffer) return fail(DownloadTransferStatus::Error, "Could not allocate the offline output buffer");
	output_io = avio_alloc_context(buffer, 64u << 10, 1, &writer, nullptr, OutputIo::write, OutputIo::seek);
	if (!output_io) { av_free(buffer); return fail(DownloadTransferStatus::Error, "Could not allocate the offline output writer"); }
	output->pb = output_io;
	if (avformat_write_header(output, nullptr) < 0) return fail(DownloadTransferStatus::Unsupported, "This HLS stream cannot be saved without changing its media tracks");
	packet = av_packet_alloc();
	if (!packet) return fail(DownloadTransferStatus::Error, "Could not allocate the HLS packet buffer");
	double first_time = std::numeric_limits<double>::infinity(), last_time = -std::numeric_limits<double>::infinity();
	int64_t packets = 0;
	while (!cancel.load() && (result = av_read_frame(input, packet)) >= 0) {
		const int stream = packet->stream_index;
		if (stream < 0 || size_t(stream) >= mapping.size() || mapping[size_t(stream)] < 0) { av_packet_unref(packet); continue; }
		const AVStream* from = input->streams[stream];
		const AVStream* to = output->streams[mapping[size_t(stream)]];
		if (stream == best_video || best_video < 0) {
			const int64_t timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
			if (timestamp != AV_NOPTS_VALUE) {
				const double when = double(timestamp) * av_q2d(from->time_base);
				first_time = std::min(first_time, when); last_time = std::max(last_time, when + std::max<int64_t>(0, packet->duration) * av_q2d(from->time_base));
			}
		}
		av_packet_rescale_ts(packet, from->time_base, to->time_base);
		packet->stream_index = mapping[size_t(stream)]; packet->pos = -1;
		if (av_interleaved_write_frame(output, packet) < 0) return fail(DownloadTransferStatus::Error, "Could not write every HLS packet to the offline video");
		++packets; reporter.emit(writer.length, -1);
		if (hooks.failed) return fail(DownloadTransferStatus::Error, "An HLS segment could not be downloaded completely");
	}
	if (cancel.load()) return fail(DownloadTransferStatus::Cancelled, {});
	if (result != AVERROR_EOF || packets == 0 || hooks.failed || writer.failed) return fail(DownloadTransferStatus::Error, "The HLS download ended before every segment was saved");
	// Closing the input also checks the final segment's transport result. HLS
	// demuxers may otherwise skip a missing segment and still report ordinary EOF.
	avformat_close_input(&input);
	if (hooks.failed) return fail(DownloadTransferStatus::Error, "An HLS segment was missing or incomplete");
	const double tolerance = std::max(.5, std::min(2.0, manifests.max_duration * .03));
	if (!std::isfinite(first_time) || !std::isfinite(last_time) || last_time - first_time + tolerance < manifests.max_duration) return fail(DownloadTransferStatus::Error, "The saved HLS video is shorter than the complete playlist");
	if (av_write_trailer(output) < 0) return fail(DownloadTransferStatus::Error, "Could not finalize the offline video container");
	avio_flush(output_io);
	if (output_io->error < 0 || writer.failed) return fail(DownloadTransferStatus::Error, "Could not finalize the offline video container");
	if (::fsync(file.fd)) {
		writer.fail_storage(file_error());
		return fail(DownloadTransferStatus::Error, "Could not finalize the offline video container");
	}
	const int64_t bytes = writer.length;
	cleanup();
	int64_t final_size = 0;
	if (!regular_fd(file.fd, &final_size)) {
		const int code = file_error();
		return {DownloadTransferStatus::Error, disk_error(code, "Could not finalize the complete offline video"), ".mkv", bytes, -1, code};
	}
	if (bytes <= 0 || final_size != bytes) return {DownloadTransferStatus::Error, "Could not finalize the complete offline video", ".mkv", bytes, -1};
	if (!file.close()) {
		const int code = file_error();
		return {DownloadTransferStatus::Error, disk_error(code, "Could not finalize the complete offline video"), ".mkv", bytes, -1, code};
	}
	if (!probe_media(request.partial_path, cancel)) return {cancel.load() ? DownloadTransferStatus::Cancelled : DownloadTransferStatus::Error, "The downloaded HLS container could not be opened", ".mkv", bytes, -1};
	reporter.emit(bytes, bytes, true);
	return {DownloadTransferStatus::Complete, {}, ".mkv", bytes, bytes};
}

} // namespace

DownloadTransferResult download_transfer(const DownloadTransferRequest& request, const DownloadProgressCallback& progress, const std::atomic<bool>& cancel) {
	if (!valid_paths(request)) return {DownloadTransferStatus::Error, "The download storage path is invalid"};
	if (cancel.load()) return {DownloadTransferStatus::Cancelled};
	try {
		if (request.stream.kind == StreamKind::Torrent || (request.stream.url.empty() && !request.stream.info_hash.empty())) return transfer_torrent(request, progress, cancel);
		if (request.stream.kind == StreamKind::Direct || !request.stream.url.empty()) return transfer_http(request, progress, cancel);
		return {DownloadTransferStatus::Unsupported, "This stream type cannot be downloaded for offline viewing"};
	} catch (const std::bad_alloc&) {
		return {DownloadTransferStatus::Error, "Not enough memory to continue this download"};
	} catch (...) {
		return {cancel.load() ? DownloadTransferStatus::Cancelled : DownloadTransferStatus::Error, "The download could not be completed"};
	}
}
