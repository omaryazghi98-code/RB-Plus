#include "download_manager.h"
#include "download_transfer.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
constexpr size_t kMaxJobs = 128, kMaxManifest = 256 * 1024;
constexpr size_t kMaxPendingBytes = 4 * 1024 * 1024;
constexpr int64_t kMaxArtwork = 32 * 1024 * 1024;
using Clock = std::chrono::steady_clock;

bool storage_failure(std::string* error, const char* operation, const std::string& path, int code) {
	if (error) *error = std::string(operation) + " failed for " + path + " (errno " +
		std::to_string(code) + ": " + std::strerror(code) + ")";
	return false;
}

bool safe_id(const std::string& value) {
	return value.size() == 33 && value[0] == 'd' &&
		std::all_of(value.begin() + 1, value.end(), [](unsigned char c) {
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		});
}

bool safe_extension(const std::string& value) {
	return value.size() >= 2 && value.size() <= 8 && value[0] == '.' &&
		std::all_of(value.begin() + 1, value.end(), [](unsigned char c) {
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
		});
}

// Every parent is checked as well as the last component. This also rejects
// traversal in a supplied data directory and refuses a redirected download root.
bool safe_directory(const std::string& path, std::string* error = nullptr) {
	if (path.empty() || path[0] != '/') return storage_failure(error, "validate directory", path, EINVAL);
	std::string current;
	for (size_t begin = 1; begin < path.size();) {
		const size_t end = path.find('/', begin);
		const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);
		if (part.empty() || part == "." || part == "..") return storage_failure(error, "validate directory", path, EINVAL);
		current += '/' + part;
		struct stat st{};
		if (::lstat(current.c_str(), &st) != 0) return storage_failure(error, "lstat", current, errno);
		if (!S_ISDIR(st.st_mode)) return storage_failure(error, "validate directory", current, ENOTDIR);
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return true;
}

bool create_private_directory(const std::string& path, std::string* error = nullptr) {
	const size_t slash = path.rfind('/');
	if (slash == std::string::npos) return storage_failure(error, "validate directory", path, EINVAL);
	if (!safe_directory(path.substr(0, slash), error)) return false;
	if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return storage_failure(error, "mkdir", path, errno);
	if (!safe_directory(path, error)) return false;
	return ::chmod(path.c_str(), 0700) == 0 || storage_failure(error, "chmod", path, errno);
}

int64_t regular_size(const std::string& path) {
	struct stat st{};
	return ::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? st.st_size : -1;
}

bool write_all(int fd, const void* bytes, size_t length) {
	const auto* data = static_cast<const unsigned char*>(bytes);
	while (length) {
		const ssize_t written = ::write(fd, data, length);
		if (written < 0 && errno == EINTR) continue;
		if (written <= 0) { if (written == 0) errno = EIO; return false; }
		data += written;
		length -= static_cast<size_t>(written);
	}
	return true;
}

bool sync_directory(const std::string& path, std::string* error = nullptr) {
	const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (fd < 0) return storage_failure(error, "open directory", path, errno);
	bool ok = ::fsync(fd) == 0 || storage_failure(error, "fsync directory", path, errno);
	if (::close(fd) != 0 && ok) ok = storage_failure(error, "close directory", path, errno);
	return ok;
}

bool atomic_private_write(const std::string& path, const std::string& bytes, std::string* error = nullptr) {
	if (!safe_directory(path.substr(0, path.rfind('/')), error)) return false;
	const std::string temporary = path + ".tmp";
	const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0) return storage_failure(error, "open", temporary, errno);
	bool ok = ::fchmod(fd, 0600) == 0 || storage_failure(error, "fchmod", temporary, errno);
	if (ok && !write_all(fd, bytes.data(), bytes.size())) ok = storage_failure(error, "write", temporary, errno);
	if (ok && ::fsync(fd) != 0) ok = storage_failure(error, "fsync", temporary, errno);
	if (::close(fd) != 0 && ok) ok = storage_failure(error, "close", temporary, errno);
	if (ok && ::rename(temporary.c_str(), path.c_str()) != 0) ok = storage_failure(error, "rename", path, errno);
	if (ok) ok = sync_directory(path.substr(0, path.rfind('/')), error);
	if (!ok) ::unlink(temporary.c_str());
	return ok;
}

bool probe_download_storage(const std::string& root, std::string* error) {
	// Completed downloads must remain discoverable as well as writable. Some
	// native mounts permit file writes while directory enumeration is denied.
	DIR* directory = ::opendir(root.c_str());
	if (!directory) return storage_failure(error, "opendir", root, errno);
	errno = 0;
	const bool listed = ::readdir(directory) != nullptr || errno == 0;
	const int listing_error = errno;
	const int closed = ::closedir(directory);
	if (!listed) return storage_failure(error, "readdir", root, listing_error);
	if (closed != 0) return storage_failure(error, "closedir", root, errno);
	// Exercise the same private, durable write used by job manifests. A successful
	// mkdir alone does not prove that the native title can store a download.
	const auto path = root + "/.storage-check";
	if (!atomic_private_write(path, "Stremio download storage check\n", error)) return false;
	return ::unlink(path.c_str()) == 0 || storage_failure(error, "unlink", path, errno);
}

bool read_manifest(const std::string& path, json& out) {
	const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
	if (fd < 0) return false;
	struct stat st{};
	bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
		st.st_size <= static_cast<int64_t>(kMaxManifest);
	std::string bytes;
	if (ok) {
		bytes.resize(static_cast<size_t>(st.st_size));
		size_t offset = 0;
		while (offset < bytes.size()) {
			const ssize_t count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
			if (count < 0 && errno == EINTR) continue;
			if (count <= 0) { ok = false; break; }
			offset += static_cast<size_t>(count);
		}
	}
	::close(fd);
	if (!ok) return false;
	// The inventory schema is shallow; reject hostile nesting before the JSON
	// parser can allocate an unbounded stack of arrays from a damaged backup.
	int depth = 0;
	bool quoted = false, escaped = false;
	for (const char c : bytes) {
		if (quoted) {
			if (escaped) escaped = false;
			else if (c == '\\') escaped = true;
			else if (c == '"') quoted = false;
		} else if (c == '"') quoted = true;
		else if (c == '{' || c == '[') { if (++depth > 32) return false; }
		else if (c == '}' || c == ']') --depth;
	}
	out = json::parse(bytes, nullptr, false);
	return out.is_object();
}

std::string json_bytes(const json& value) {
	return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string text_value(const json& value, const char* name, size_t limit = 16384) {
	const auto it = value.find(name);
	if (it == value.end() || !it->is_string()) return {};
	const auto& result = it->get_ref<const std::string&>();
	return result.size() <= limit ? result : std::string{};
}

int64_t integer_value(const json& value, const char* name, int64_t fallback = -1) {
	const auto it = value.find(name);
	if (it == value.end() || !it->is_number_integer()) return fallback;
	if (it->is_number_unsigned() && it->get<uint64_t>() > uint64_t(INT64_MAX)) return fallback;
	return it->get<int64_t>();
}

bool boolean_value(const json& value, const char* name, bool fallback = false) {
	const auto it = value.find(name);
	return it != value.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

std::vector<std::string> string_list(const json& value, const char* name) {
	std::vector<std::string> result;
	const auto it = value.find(name);
	if (it == value.end() || !it->is_array() || it->size() > 64) return result;
	size_t total = 0;
	for (const auto& item : *it) {
		if (!item.is_string()) return {};
		const auto s = item.get<std::string>();
		if (s.size() > 16384 || (total += s.size()) > 65536) return {};
		result.push_back(s);
	}
	return result;
}

json stream_json(const Stream& s) {
	return {{"name", s.name}, {"description", s.description}, {"addon", s.addon},
		{"url", s.url}, {"info_hash", s.info_hash}, {"file_idx", s.file_idx},
		{"sources", s.sources}, {"filename", s.filename}, {"request_headers", s.request_headers},
		{"kind", static_cast<int>(s.kind)}, {"video_size", s.has_video_size ? s.video_size : 0}};
}

Stream parse_stream(const json& value) {
	Stream s;
	if (!value.is_object()) return s;
	s.name = text_value(value, "name"); s.description = text_value(value, "description");
	s.addon = text_value(value, "addon"); s.url = text_value(value, "url");
	s.info_hash = text_value(value, "info_hash", 64); s.filename = text_value(value, "filename");
	const int64_t index = integer_value(value, "file_idx");
	s.file_idx = index >= -1 && index <= INT32_MAX ? static_cast<int>(index) : -1;
	s.sources = string_list(value, "sources"); s.request_headers = string_list(value, "request_headers");
	const auto kind = integer_value(value, "kind", static_cast<int>(StreamKind::Unsupported));
	if (kind >= 0 && kind <= static_cast<int>(StreamKind::Unsupported)) s.kind = static_cast<StreamKind>(kind);
	const int64_t size = integer_value(value, "video_size", 0);
	s.video_size = size > 0 ? static_cast<uint64_t>(size) : 0; s.has_video_size = s.video_size > 0;
	return s;
}

std::string digest(const std::string& value) {
	uint64_t a = 14695981039346656037ULL, b = 1099511628211ULL;
	for (unsigned char c : value) { a = (a ^ c) * 1099511628211ULL; b = (b ^ c) * 14029467366897019727ULL; }
	char output[33];
	std::snprintf(output, sizeof(output), "%016llx%016llx", static_cast<unsigned long long>(a),
		static_cast<unsigned long long>(b));
	return output;
}

bool torrent_stream(const Stream& s) { return !s.info_hash.empty(); }

Stream download_stream(const Stream& source) {
	Stream result;
	result.name = source.name; result.description = source.description; result.addon = source.addon;
	result.url = source.url; result.info_hash = source.info_hash; result.file_idx = source.file_idx;
	result.sources = source.sources; result.filename = source.filename; result.request_headers = source.request_headers;
	result.kind = source.kind; result.video_size = source.video_size; result.has_video_size = source.has_video_size;
	return result;
}

bool valid_request(const DownloadRequest& r, std::string& error) {
	if (r.live) { error = "Live streams cannot be downloaded for offline viewing."; return false; }
	if (r.media_id.empty() || r.type.empty() || r.title.empty() || r.media_id.size() > 2048 || r.poster_url.size() > 16384 ||
		r.poster_path.size() > 16384 || r.background_path.size() > 16384 || r.logo_path.size() > 16384 ||
		r.type.size() > 64 || r.video_id.size() > 2048 || r.title.size() > 4096 || r.subtitle.size() > 8192) {
		error = "The video metadata is not valid for downloading."; return false;
	}
	const auto& s = r.stream;
	if (!s.playable() || s.url.size() > 16384 || s.info_hash.size() > 64 ||
		(!torrent_stream(s) && s.url.rfind("http://", 0) != 0 && s.url.rfind("https://", 0) != 0)) {
		error = "This source does not support local downloads."; return false;
	}
	const auto valid_list = [](const std::vector<std::string>& values) {
		if (values.size() > 64) return false;
		size_t total = 0;
		for (const auto& value : values) if (value.size() > 16384 || (total += value.size()) > 65536) return false;
		return true;
	};
	if (!valid_list(s.sources) || !valid_list(s.request_headers) || s.name.size() > 16384 ||
		s.description.size() > 16384 || s.addon.size() > 16384 || s.filename.size() > 16384 ||
		json_bytes(stream_json(s)).size() > 192 * 1024) {
		error = "The source configuration is too large."; return false;
	}
	error.clear();
	return true;
}

std::string safe_error(std::string error) {
	if (error.empty()) return "Download failed. You can retry.";
	if (error.size() > 512 || error.find("://") != std::string::npos ||
		error.find("Bearer ") != std::string::npos || error.find("magnet:") != std::string::npos)
		return "Download failed. You can retry.";
	return error;
}

void update_fraction(DownloadEntry& entry) {
	entry.done = std::max<int64_t>(0, entry.done);
	entry.progress = entry.state == DownloadState::Complete ? 1.0 :
		(entry.total > 0 ? std::clamp(double(entry.done) / double(entry.total), 0.0, 1.0) : -1.0);
}

bool copy_artwork(const std::string& source, const std::string& destination) {
	if (source.empty()) return false;
	const int input = ::open(source.c_str(), O_RDONLY | O_NOFOLLOW);
	if (input < 0) return false;
	struct stat st{};
	std::array<unsigned char, 12> header{};
	bool ok = ::fstat(input, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 12 &&
		st.st_size <= kMaxArtwork && ::read(input, header.data(), header.size()) == 12;
	uint32_t width = 0, height = 0;
	if (ok) {
		std::memcpy(&width, header.data() + 4, 4); std::memcpy(&height, header.data() + 8, 4);
		ok = std::memcmp(header.data(), "RGBA", 4) == 0 && width > 0 && height > 0 &&
			width <= 4096 && height <= 4096 && 12 + int64_t(width) * height * 4 == st.st_size;
	}
	int output = -1;
	const auto temporary = destination + ".tmp";
	if (ok) output = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	ok = ok && output >= 0;
	if (ok) ok = ::fchmod(output, 0600) == 0 && write_all(output, header.data(), header.size());
	std::array<unsigned char, 65536> buffer{};
	while (ok) {
		const ssize_t count = ::read(input, buffer.data(), buffer.size());
		if (count < 0 && errno == EINTR) continue;
		if (count == 0) break;
		if (count < 0 || !write_all(output, buffer.data(), static_cast<size_t>(count))) ok = false;
	}
	::close(input);
	if (output >= 0) { if (ok) ok = ::fsync(output) == 0; if (::close(output) != 0) ok = false; }
	if (ok) ok = ::rename(temporary.c_str(), destination.c_str()) == 0;
	if (!ok) ::unlink(temporary.c_str());
	return ok;
}

// Job storage is deliberately flat. Never descend into a directory supplied by
// a manifest or follow a symlink. A symlink itself can be unlinked safely.
bool remove_flat_directory(const std::string& root, const std::string& id) {
	if (!safe_id(id) || !safe_directory(root)) return false;
	const auto path = root + '/' + id;
	struct stat original{};
	if (::lstat(path.c_str(), &original) != 0) return errno == ENOENT;
	if (!S_ISDIR(original.st_mode)) return false;
	DIR* directory = ::opendir(path.c_str());
	if (!directory) return false;
	std::vector<std::string> files;
	bool ok = true;
	while (const auto* entry = ::readdir(directory)) {
		const std::string name = entry->d_name;
		if (name == "." || name == "..") continue;
		struct stat st{};
		if (name.find('/') != std::string::npos || files.size() >= 512 ||
			::lstat((path + '/' + name).c_str(), &st) != 0 || S_ISDIR(st.st_mode)) { ok = false; break; }
		files.push_back(name);
	}
	::closedir(directory);
	if (!ok) return false;
	for (const auto& name : files) {
		struct stat current{};
		if (!safe_directory(root) || ::lstat(path.c_str(), &current) != 0 ||
			!S_ISDIR(current.st_mode) || current.st_ino != original.st_ino || current.st_dev != original.st_dev)
			return false;
		if (::unlink((path + '/' + name).c_str()) != 0 && errno != ENOENT) return false;
	}
	return ::rmdir(path.c_str()) == 0 || errno == ENOENT;
}
} // namespace

struct DownloadManager::Impl {
	struct Job {
		DownloadRequest request;
		DownloadEntry entry;
		std::string identity, directory, media_name;
		bool removing = false, artwork_pending = true, artwork_busy = false;
		bool artwork_manifest_dirty = false;
		uint64_t artwork_generation = 0;
		int64_t created_ms = 0;
		Clock::time_point last_progress{};
		Clock::time_point artwork_retry_at{};
		std::shared_ptr<GrowingFileState> playback = std::make_shared<GrowingFileState>();
		uint64_t playback_generation = 0;
		bool progress_ready = false;
		int torrent_file_idx = -1;
	};
	mutable std::mutex mutex;
	// New-job storage has its own lifecycle gate. Committing a waiting job must
	// not hold the live progress mutex or stall the active transfer's callback.
	std::mutex lifecycle_mutex;
	// Storage still uses mutex to serialize durable state transitions. UI reads
	// only this separately published inventory: a slow fsync must never become
	// a frame-long wait in snapshot() or find(). Nothing holding this mutex does
	// filesystem or torrent work.
	mutable std::mutex published_mutex;
	std::vector<DownloadEntry> published;
	struct ArtworkUpdate { std::string id, path, url; };
	std::mutex artwork_updates_mutex;
	std::vector<ArtworkUpdate> artwork_updates;
	std::atomic<bool> artwork_updates_pending{false}, accept_artwork{false};
	std::condition_variable changed;
	std::thread worker, artwork_worker;
	struct EnqueueWork { DownloadRequest request; EnqueueCallback done; size_t bytes; uint64_t generation; };
	std::mutex enqueue_mutex;
	std::condition_variable enqueue_changed;
	std::deque<EnqueueWork> enqueue_work;
	std::thread enqueue_worker;
	size_t pending_bytes = 0;
	bool enqueue_closed = true;
	std::atomic<uint64_t> suspend_generation{0};
	std::vector<std::shared_ptr<Job>> jobs;
	std::string root, storage_parent, storage_error, active_id;
	std::atomic<bool> cancel{false};
	std::atomic<uint64_t> revision{1};
	bool ready = false, stopping = false, enabled = false;
	bool foreground_torrent = false, active_torrent = false;
	bool torrent_preempted = false;
	std::shared_ptr<std::atomic<unsigned>> foreground_resolvers = std::make_shared<std::atomic<unsigned>>(0);
	uint64_t sequence = 0;
	bool torrent_reserved() const { return foreground_torrent || foreground_resolvers->load() != 0; }
	bool artwork_running() const {
		return std::any_of(jobs.begin(), jobs.end(), [](const auto& job) { return job->artwork_busy; });
	}
	bool artwork_due(const Job& job) const {
		return (job.artwork_pending || job.artwork_manifest_dirty) && Clock::now() >= job.artwork_retry_at;
	}
	bool runnable_media() const {
		if (!enabled) return false;
		return std::any_of(jobs.begin(), jobs.end(), [&](const auto& job) {
			return !job->removing && (job->entry.state == DownloadState::Queued || job->entry.state == DownloadState::Waiting) &&
				(!torrent_stream(job->request.stream) || !torrent_reserved());
		});
	}

	void touch() {
		for (const auto& job : jobs) {
			auto& e = job->entry;
			if (e.state != DownloadState::Downloading) {
				e.bytes_per_second = 0; e.remaining_seconds = e.state == DownloadState::Complete ? 0 : -1;
				e.connected_peers = e.connected_seeders = -1;
			}
			e.playable_while_downloading = !job->removing && job->progress_ready &&
				e.state == DownloadState::Downloading && progressive_download_threshold(e.done, e.total);
			GrowingFilePhase phase = GrowingFilePhase::Preparing;
			if (job->removing || stopping) phase = GrowingFilePhase::Removed;
			else if (e.state == DownloadState::Complete) phase = GrowingFilePhase::Complete;
			else if (e.state == DownloadState::Failed) phase = GrowingFilePhase::Failed;
			else if (e.state == DownloadState::Paused) phase = GrowingFilePhase::Paused;
			else if (e.state == DownloadState::Waiting) phase = GrowingFilePhase::Waiting;
			else if (e.state == DownloadState::Downloading && job->progress_ready) phase = GrowingFilePhase::Downloading;
			job->playback->publish({e.done, e.total, job->playback_generation, phase});
		}
		std::vector<DownloadEntry> next; next.reserve(jobs.size());
		for (const auto& job : jobs) if (!job->removing) next.push_back(job->entry);
		{
			std::lock_guard lock(published_mutex);
			published.swap(next);
		}
		revision.fetch_add(1, std::memory_order_relaxed);
	}
	bool start_storage() {
		if (ready) return true;
		storage_error.clear();
		if (storage_parent.empty()) {
			storage_error = "Download storage has not been initialized.";
			return false;
		}
		const auto candidate = storage_parent + "/downloads";
		if (!safe_directory(storage_parent, &storage_error) ||
			!create_private_directory(candidate, &storage_error) ||
			!probe_download_storage(candidate, &storage_error)) return false;
		root = candidate; stopping = false; cancel.store(false); load();
		worker = std::thread([this] { run(); });
		artwork_worker = std::thread([this] { run_artwork(); }); ready = true; accept_artwork = true; touch();
		return true;
	}
	std::shared_ptr<Job> lookup(const std::string& id) const {
		for (const auto& job : jobs) if (job->entry.id == id) return job;
		return {};
	}
	bool save(const Job& job) const {
		const auto& e = job.entry;
		json manifest{{"version", 1}, {"id", e.id}, {"identity", job.identity}, {"created_ms", job.created_ms},
			{"media_id", e.media_id}, {"type", e.type}, {"video_id", e.video_id},
			{"title", e.title}, {"subtitle", e.subtitle}, {"season", e.season}, {"episode", e.episode},
			{"state", static_cast<int>(e.state)}, {"done", e.done}, {"total", e.total},
			{"error", e.error}, {"media_name", job.media_name}, {"removing", job.removing},
			{"artwork_pending", job.artwork_pending},
			{"poster_url", job.request.poster_url},
			{"poster_source", job.request.poster_path}, {"background_source", job.request.background_path},
			{"logo_source", job.request.logo_path}, {"stream", stream_json(job.request.stream)}};
		const auto bytes = json_bytes(manifest);
		return bytes.size() <= kMaxManifest && safe_directory(root) &&
			atomic_private_write(job.directory + "/manifest.json", bytes);
	}
	void load() {
		DIR* dir = ::opendir(root.c_str());
		if (!dir) return;
		std::vector<std::string> names;
		while (const auto* ent = ::readdir(dir)) if (safe_id(ent->d_name)) {
			if (names.size() >= 1024) break;
			names.emplace_back(ent->d_name);
		}
		::closedir(dir);
		std::sort(names.begin(), names.end());
		for (const auto& id : names) {
			if (jobs.size() >= kMaxJobs) break;
			const auto path = root + '/' + id;
			if (!safe_directory(path)) continue;
			json value;
			if (!read_manifest(path + "/manifest.json", value) || integer_value(value, "version") != 1 ||
				text_value(value, "id") != id) continue;
			auto job = std::make_shared<Job>();
			job->directory = path;
			auto& e = job->entry; auto& r = job->request;
			e.id = id; e.media_id = r.media_id = text_value(value, "media_id", 2048);
			e.type = r.type = text_value(value, "type", 64); e.video_id = r.video_id = text_value(value, "video_id", 2048);
			e.title = r.title = text_value(value, "title", 4096); e.subtitle = r.subtitle = text_value(value, "subtitle", 8192);
			e.poster_url = r.poster_url = text_value(value, "poster_url");
			if (e.media_id.empty() || e.type.empty() || e.title.empty()) continue;
			e.season = r.season = static_cast<int>(std::clamp<int64_t>(integer_value(value, "season"), -1, 99999));
			e.episode = r.episode = static_cast<int>(std::clamp<int64_t>(integer_value(value, "episode"), -1, 999999));
			e.done = std::max<int64_t>(0, integer_value(value, "done", 0)); e.total = integer_value(value, "total");
			e.error = safe_error(text_value(value, "error", 512));
			if (value.contains("stream")) r.stream = parse_stream(value["stream"]);
			job->identity = text_value(value, "identity", 32);
			job->created_ms = std::max<int64_t>(0, integer_value(value, "created_ms", 0));
			job->media_name = text_value(value, "media_name", 16);
			job->removing = boolean_value(value, "removing");
			job->artwork_pending = boolean_value(value, "artwork_pending");
			if (job->artwork_pending) {
				r.poster_path = text_value(value, "poster_source"); r.background_path = text_value(value, "background_source");
				r.logo_path = text_value(value, "logo_source");
			}
			if (regular_size(path + "/poster.rgba") >= 12) e.poster_path = path + "/poster.rgba";
			else if (regular_size(r.poster_path) >= 12) e.poster_path = r.poster_path;
			if (regular_size(path + "/background.rgba") >= 12) e.background_path = path + "/background.rgba";
			else if (regular_size(r.background_path) >= 12) e.background_path = r.background_path;
			if (regular_size(path + "/logo.rgba") >= 12) e.logo_path = path + "/logo.rgba";
			else if (regular_size(r.logo_path) >= 12) e.logo_path = r.logo_path;
			const int64_t state = integer_value(value, "state");
			if (state == static_cast<int>(DownloadState::Complete)) {
				const bool name_ok = job->media_name.rfind("media.", 0) == 0 &&
					safe_extension(job->media_name.substr(5)) && job->media_name != "media.part";
				const int64_t size = name_ok ? regular_size(path + '/' + job->media_name) : -1;
				if (size > 0 && size == e.done) {
					e.state = DownloadState::Complete; e.local_path = path + '/' + job->media_name; e.total = size; e.error.clear();
				} else { e.state = DownloadState::Failed; e.error = "The downloaded file is missing or incomplete. Download it again."; }
			} else {
				e.state = state == static_cast<int>(DownloadState::Failed) ? DownloadState::Failed : DownloadState::Paused;
				if (e.state == DownloadState::Paused) e.error.clear();
				const int64_t size = regular_size(path + "/media.part");
				if (size >= 0) e.done = size;
			}
			update_fraction(e); jobs.push_back(job);
			// Restored manifests retain private permissions even after a copied backup.
			::chmod((path + "/manifest.json").c_str(), 0600);
		}
		std::stable_sort(jobs.begin(), jobs.end(), [](const auto& a, const auto& b) {
			return a->created_ms < b->created_ms;
		});
	}
	void prepare_artwork(const std::shared_ptr<Job>& job) {
		const auto started = Clock::now();
		DownloadRequest request;
		uint64_t generation = 0;
		bool pending = false;
		{ std::lock_guard lock(mutex); request = job->request; generation = job->artwork_generation; pending = job->artwork_pending; }
		const bool poster = pending && !request.poster_path.empty() && safe_directory(job->directory) &&
			copy_artwork(request.poster_path, job->directory + "/poster.rgba");
		const bool background = pending && !request.background_path.empty() && safe_directory(job->directory) &&
			copy_artwork(request.background_path, job->directory + "/background.rgba");
		const bool logo = pending && !request.logo_path.empty() && safe_directory(job->directory) &&
			copy_artwork(request.logo_path, job->directory + "/logo.rgba");
		std::lock_guard lock(mutex);
		job->artwork_busy = false;
		if (!job->removing && generation == job->artwork_generation) {
			if (poster) job->entry.poster_path = job->directory + "/poster.rgba";
			if (background) job->entry.background_path = job->directory + "/background.rgba";
			if (logo) job->entry.logo_path = job->directory + "/logo.rgba";
			job->request.poster_path.clear(); job->request.background_path.clear(); job->request.logo_path.clear();
			job->artwork_pending = false;
			job->artwork_manifest_dirty = !save(*job);
			job->artwork_retry_at = job->artwork_manifest_dirty ? Clock::now() + std::chrono::seconds(5) : Clock::time_point{};
		}
		dlog("download queue: artwork_ms=%.1f copied=%d media_active=%d",
			std::chrono::duration<double, std::milli>(Clock::now() - started).count(),
			int(poster) + int(background) + int(logo), !active_id.empty());
		touch(); changed.notify_all();
	}
	bool queue_artwork(const std::string& id, const std::string& path, const std::string& url) {
		if (!accept_artwork.load() || !safe_id(id)) return false;
		{
			std::lock_guard lock(published_mutex);
			if (std::none_of(published.begin(), published.end(), [&](const auto& e) { return e.id == id; })) return false;
		}
		{
			std::lock_guard lock(artwork_updates_mutex);
			if (!accept_artwork.load()) return false;
			auto update = std::find_if(artwork_updates.begin(), artwork_updates.end(), [&](const auto& item) { return item.id == id; });
			if (update == artwork_updates.end()) {
				if (artwork_updates.size() >= kMaxJobs) return false;
				artwork_updates.push_back({id, path, url});
			} else {
				if (!path.empty()) update->path = path;
				if (!url.empty()) update->url = url;
			}
			artwork_updates_pending = true;
		}
		changed.notify_all(); return true;
	}
	void apply_artwork_updates() {
		std::vector<ArtworkUpdate> pending;
		{
			std::lock_guard lock(artwork_updates_mutex);
			pending.swap(artwork_updates); artwork_updates_pending = false;
		}
		for (const auto& update : pending) {
			// All file validation and persistence happen on this worker. Callback
			// callers can continue drawing even while a previous fsync is blocked.
			const bool image_valid = !update.path.empty() && regular_size(update.path) >= 12;
			std::lock_guard lock(mutex);
			const auto job = lookup(update.id);
			if (!job || job->removing) continue;
			bool modified = false;
			if (!update.url.empty() && job->request.poster_url != update.url) {
				job->entry.poster_url = job->request.poster_url = update.url; modified = true;
			}
			const bool own_poster = job->entry.poster_path == job->directory + "/poster.rgba";
			if (image_valid && !own_poster && !(job->request.poster_path == update.path && job->artwork_pending)) {
				job->entry.poster_path = job->request.poster_path = update.path;
				++job->artwork_generation; job->artwork_pending = true; modified = true;
			}
			// Cache paths are immediately usable by the UI. The source URL and
			// private artwork are persisted when the media writer is idle, so a
			// late cover cannot flush another job's filesystem under its writer.
			if (modified) { job->artwork_manifest_dirty = true; touch(); }
		}
		changed.notify_all();
	}
	void run_artwork() {
		for (;;) {
			std::shared_ptr<Job> selected;
			{
				std::unique_lock lock(mutex);
				// Callback producers intentionally never take the storage mutex.
				// A bounded wait also covers a notify racing the predicate-to-wait
				// transition of that independent, coalesced queue.
				changed.wait_for(lock, std::chrono::milliseconds(100), [&] {
					if (stopping) return true;
					if (artwork_updates_pending.load()) return true;
					if (!active_id.empty() || torrent_reserved() || artwork_running() || runnable_media()) return false;
					for (const auto& job : jobs)
						if (artwork_due(*job) && !job->removing) return true;
					return false;
				});
				if (stopping) break;
				if (artwork_updates_pending.load()) {
					lock.unlock(); apply_artwork_updates(); continue;
				}
				if (!active_id.empty() || torrent_reserved() || artwork_running() || runnable_media()) continue;
				for (const auto& job : jobs) if (artwork_due(*job) && !job->removing) {
					selected = job; job->artwork_busy = true; break;
				}
			}
			if (selected) prepare_artwork(selected);
		}
	}
	void run() {
		for (;;) {
			std::shared_ptr<Job> selected;
			bool deleting = false, preparing_artwork = false;
			{
				std::unique_lock lock(mutex);
				changed.wait_for(lock, std::chrono::milliseconds(100), [&] {
					if (stopping) return true;
					for (const auto& j : jobs) if ((j->removing && !j->artwork_busy) ||
						(enabled && !artwork_running() && !j->removing && j->entry.state == DownloadState::Queued)) return true;
					return false;
				});
				if (stopping) break;
				for (const auto& j : jobs) if (j->removing && !j->artwork_busy) { selected = j; deleting = true; break; }
				if (!selected && enabled && !artwork_running()) for (const auto& j : jobs) {
					if (j->removing) continue;
					if (j->entry.state != DownloadState::Queued && j->entry.state != DownloadState::Waiting) continue;
					if (torrent_stream(j->request.stream) && torrent_reserved()) {
						if (j->entry.state != DownloadState::Waiting) { j->entry.state = DownloadState::Waiting; touch(); }
						continue;
					}
					if (artwork_due(*j)) {
						selected = j; j->artwork_busy = true; preparing_artwork = true; break;
					}
					selected = j; active_id = j->entry.id; active_torrent = torrent_stream(j->request.stream);
					torrent_preempted = false;
					cancel.store(false); j->entry.state = DownloadState::Downloading; j->entry.error.clear();
					j->entry.bytes_per_second = 0; j->entry.remaining_seconds = -1;
					j->entry.connected_peers = j->entry.connected_seeders = -1;
					++j->playback_generation; j->progress_ready = false; j->torrent_file_idx = -1;
					j->last_progress = Clock::now();
					if (!save(*j)) {
						j->entry.state = DownloadState::Failed; j->entry.error = "The download state could not be saved.";
						active_id.clear(); active_torrent = false; selected.reset(); changed.notify_all();
					}
					touch(); break;
				}
			}
			if (!selected) continue;
			if (preparing_artwork) { prepare_artwork(selected); continue; }
			if (deleting) {
				const bool removed = remove_flat_directory(root, selected->entry.id);
				std::lock_guard lock(mutex);
				if (removed) jobs.erase(std::remove(jobs.begin(), jobs.end(), selected), jobs.end());
				else {
					selected->removing = false; selected->entry.state = DownloadState::Failed;
					selected->entry.error = "The download folder could not be deleted."; save(*selected);
				}
				touch(); continue;
			}
			DownloadTransferRequest transfer;
			{
				std::lock_guard lock(mutex);
				transfer.stream = selected->request.stream; transfer.work_dir = selected->directory;
				transfer.partial_path = selected->directory + "/media.part";
				transfer.checkpoint_path = selected->directory + "/transfer.json";
				transfer.season = selected->request.season; transfer.episode = selected->request.episode;
			}
			DownloadTransferResult result;
			bool progress_metadata_failed = false;
			if (safe_directory(root) && safe_directory(selected->directory)) {
				try {
					result = download_transfer(transfer, [this, selected, &progress_metadata_failed](const DownloadTransferProgress& p) {
						std::lock_guard lock(mutex);
						if (selected->removing || selected->entry.state != DownloadState::Downloading) return;
						const auto now = Clock::now();
						if (selected->progress_ready && now - selected->last_progress < std::chrono::milliseconds(200) &&
							p.done != p.total && p.done >= selected->entry.done && p.total == selected->entry.total) return;
						if (selected->progress_ready && (p.done < selected->entry.done ||
							(p.total > 0 && p.total != selected->entry.total))) ++selected->playback_generation;
						const int64_t total = p.total > 0 ? p.total : -1;
						const bool metadata_changed = total != selected->entry.total;
						selected->progress_ready = true; selected->torrent_file_idx = p.torrent_file_idx;
						selected->last_progress = now; selected->entry.done = std::max<int64_t>(0, p.done);
						selected->entry.total = total;
						selected->entry.bytes_per_second = std::isfinite(p.bytes_per_second) ? std::max(0.0, p.bytes_per_second) : 0;
						selected->entry.remaining_seconds = p.remaining_seconds >= 0 ? p.remaining_seconds : -1;
						selected->entry.connected_peers = std::max(-1, p.connected_peers);
						selected->entry.connected_seeders = p.connected_seeders >= 0 && p.connected_peers >= 0 ?
							std::min(p.connected_seeders, p.connected_peers) : -1;
						update_fraction(selected->entry); touch();
						// The transfer owns durable byte checkpoints. Persist a newly
						// resolved total once; load() recovers current bytes from the
						// partial file without flushing the manifest on every update.
						if (metadata_changed && !save(*selected)) {
							progress_metadata_failed = true; cancel.store(true);
						}
					}, cancel);
				} catch (...) { result.error = "An internal error interrupted the download. You can retry."; }
			} else result.error = "The download folder is not available.";
			{
				std::lock_guard lock(mutex);
				active_id.clear(); active_torrent = false; changed.notify_all();
				auto& e = selected->entry; e.bytes_per_second = 0; e.remaining_seconds = -1;
				e.connected_peers = e.connected_seeders = -1;
				if (selected->removing) { touch(); continue; }
				if (progress_metadata_failed) {
					e.state = DownloadState::Failed; e.error = "The download metadata could not be saved.";
				} else if (result.status == DownloadTransferStatus::Complete) {
					const int64_t size = regular_size(transfer.partial_path);
					std::string extension = safe_extension(result.extension) && result.extension != ".part" ? result.extension : ".media";
					const std::string filename = "media" + extension;
					if (safe_directory(root) && safe_directory(selected->directory) && size > 0 &&
						(result.done <= 0 || result.done == size) && (result.total <= 0 || result.total == size) &&
						::rename(transfer.partial_path.c_str(), (selected->directory + '/' + filename).c_str()) == 0) {
						selected->media_name = filename; e.local_path = selected->directory + '/' + filename;
						e.done = e.total = size; e.state = DownloadState::Complete; e.error.clear();
						e.remaining_seconds = 0;
					} else { e.state = DownloadState::Failed; e.error = "The downloaded file is incomplete or cannot be saved."; }
				} else if (result.status == DownloadTransferStatus::Cancelled || cancel.load()) {
					const int64_t size = regular_size(transfer.partial_path);
					if (torrent_stream(transfer.stream)) {
						// A disconnected writer can leave bytes beyond its last reply.
						// File length alone must not expose that tail to playback.
						const int64_t acknowledged = std::max<int64_t>(0, result.done);
						const int64_t prefix = std::min(acknowledged, std::max<int64_t>(0, size));
						if (prefix < e.done) ++selected->playback_generation;
						e.done = e.total > 0 ? std::min(prefix, e.total) : prefix;
					} else if (size >= 0) e.done = size;
					if (!stopping && enabled && torrent_preempted && e.state == DownloadState::Downloading)
						e.state = torrent_reserved() ? DownloadState::Waiting : DownloadState::Queued;
					else if (e.state == DownloadState::Downloading) e.state = DownloadState::Paused;
					e.error.clear();
				} else { e.state = DownloadState::Failed; e.error = safe_error(result.error); }
				update_fraction(e);
				if (!save(*selected) && e.state == DownloadState::Complete) {
					e.state = DownloadState::Failed; e.local_path.clear(); e.error = "The video was written, but its completion state could not be saved. Download it again.";
				}
				touch();
			}
		}
	}
};

DownloadManager::DownloadManager() : impl_(std::make_unique<Impl>()) {}
DownloadManager::~DownloadManager() { shutdown(); }

bool DownloadManager::init(const std::string& data_dir, std::string* error) {
	shutdown();
	auto& p = *impl_;
	std::lock_guard lifecycle_lock(p.lifecycle_mutex);
	{
		std::lock_guard lock(p.enqueue_mutex);
		p.enqueue_closed = false;
	}
	std::lock_guard lock(p.mutex);
	p.storage_parent = data_dir;
	while (p.storage_parent.size() > 1 && p.storage_parent.back() == '/') p.storage_parent.pop_back();
	p.foreground_torrent = p.active_torrent = false; p.active_id.clear();
	const bool ready = p.start_storage();
	if (error) *error = p.storage_error;
	return ready;
}

void DownloadManager::shutdown() {
	auto& p = *impl_;
	{
		std::lock_guard lock(p.enqueue_mutex);
		p.enqueue_closed = true;
	}
	p.enqueue_changed.notify_all();
	if (p.enqueue_worker.joinable()) p.enqueue_worker.join();
	std::lock_guard lifecycle_lock(p.lifecycle_mutex);
	p.accept_artwork = false;
	{
		std::lock_guard lock(p.mutex);
		p.stopping = true; p.enabled = false; p.cancel.store(true); p.changed.notify_all();
		for (const auto& j : p.jobs) if (j->entry.state == DownloadState::Queued ||
			j->entry.state == DownloadState::Downloading || j->entry.state == DownloadState::Waiting) {
			j->entry.state = DownloadState::Paused; j->entry.bytes_per_second = 0;
		}
		p.touch();
	}
	if (p.worker.joinable()) p.worker.join();
	if (p.artwork_worker.joinable()) p.artwork_worker.join();
	// Preserve late source/path updates without copying queued images during
	// shutdown. They remain visible from the persistent cache on the next boot
	// and are made private before that video's transfer starts.
	p.apply_artwork_updates();
	{
		std::lock_guard lock(p.artwork_updates_mutex);
		p.artwork_updates.clear(); p.artwork_updates_pending = false;
	}
	std::lock_guard lock(p.mutex);
	for (const auto& j : p.jobs) p.save(*j);
	p.ready = false; p.active_torrent = false; p.active_id.clear(); p.jobs.clear();
	p.storage_parent.clear(); p.root.clear(); p.storage_error.clear(); p.touch(); p.changed.notify_all();
}

std::string DownloadManager::enqueue(const DownloadRequest& request, std::string& error) {
	return enqueue_at_generation(request, error, impl_->suspend_generation.load());
}

std::string DownloadManager::enqueue_at_generation(const DownloadRequest& request, std::string& error, uint64_t generation) {
	const auto started = Clock::now();
	error.clear();
	if (!valid_request(request, error)) return {};
	const std::string identity = digest(json_bytes(json{{"media_id", request.media_id}, {"type", request.type},
		{"video_id", request.video_id}, {"url", request.stream.url}, {"info_hash", request.stream.info_hash},
		{"file_idx", request.stream.file_idx}, {"season", request.season}, {"episode", request.episode},
		{"headers", request.stream.request_headers}}));
	auto& p = *impl_;
	std::lock_guard lifecycle_lock(p.lifecycle_mutex);
	std::unique_lock lock(p.mutex);
	// Retry a failed startup only on an explicit download action. A filesystem
	// grant or temporarily unavailable mount must not disable downloads until exit.
	if (!p.ready && !p.start_storage()) { error = "Download storage is not available."; return {}; }
	for (const auto& j : p.jobs) if (!j->removing && j->identity == identity) return j->entry.id;
	if (p.jobs.size() >= kMaxJobs) { error = "The limit of 128 downloads has been reached. Delete a video to add another."; return {}; }
	auto job = std::make_shared<Impl::Job>();
	job->request.media_id = request.media_id; job->request.type = request.type; job->request.video_id = request.video_id;
	job->request.title = request.title; job->request.subtitle = request.subtitle;
	job->request.poster_path = request.poster_path; job->request.background_path = request.background_path; job->request.logo_path = request.logo_path;
	job->request.poster_url = request.poster_url; job->request.season = request.season; job->request.episode = request.episode;
	job->request.stream = download_stream(request.stream); job->identity = identity;
	job->artwork_pending = !request.poster_path.empty() || !request.background_path.empty() || !request.logo_path.empty();
	auto& e = job->entry;
	e.media_id = request.media_id; e.type = request.type; e.video_id = request.video_id;
	e.title = request.title; e.subtitle = request.subtitle; e.season = request.season; e.episode = request.episode;
	e.poster_url = request.poster_url;
	const std::string root = p.root;
	const auto sequence = ++p.sequence;
	// This job is still private and cannot be selected, removed or observed.
	// The lifecycle gate serializes duplicate enqueues and excludes shutdown,
	// while active progress and UI snapshots continue without a storage wait.
	lock.unlock();
	if (regular_size(request.poster_path) >= 12) e.poster_path = request.poster_path;
	if (regular_size(request.background_path) >= 12) e.background_path = request.background_path;
	if (regular_size(request.logo_path) >= 12) e.logo_path = request.logo_path;
	const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
	job->created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	for (int attempt = 0; attempt < 8; ++attempt) {
		e.id = 'd' + digest(identity + std::to_string(stamp) + ':' + std::to_string(sequence) + ':' + std::to_string(attempt));
		job->directory = root + '/' + e.id;
		struct stat st{};
		if (::lstat(job->directory.c_str(), &st) != 0 && errno == ENOENT) break;
		e.id.clear();
	}
	if (e.id.empty() || !create_private_directory(job->directory) || !p.save(*job) || !sync_directory(root)) {
		error = "The download could not be created. Check the available storage space.";
		if (!e.id.empty()) remove_flat_directory(root, e.id);
		dlog("download queue: enqueue_ms=%.1f committed=0", std::chrono::duration<double, std::milli>(Clock::now() - started).count());
		return {};
	}
	lock.lock();
	if (generation != p.suspend_generation.load()) {
		// A submission may be private while logout pauses the published list.
		// Persist its suspended state before exposing it, even if another login
		// has already enabled new work in the meantime.
		e.state = DownloadState::Paused;
		lock.unlock();
		if (!p.save(*job)) {
			error = "The download could not be created. Check the available storage space.";
			remove_flat_directory(root, e.id); return {};
		}
		lock.lock();
	}
	p.jobs.push_back(job); p.touch(); p.changed.notify_all();
	const auto queued = p.jobs.size();
	const bool active = !p.active_id.empty();
	const bool artwork_deferred = active && job->artwork_pending;
	lock.unlock();
	dlog("download queue: enqueue_ms=%.1f committed=1 inventory=%zu media_active=%d artwork_deferred=%d",
		std::chrono::duration<double, std::milli>(Clock::now() - started).count(), queued, active, artwork_deferred);
	return e.id;
}

bool DownloadManager::enqueue_async(DownloadRequest request, EnqueueCallback done) {
	auto& p = *impl_;
	const auto generation = p.suspend_generation.load();
	std::string error;
	if (!valid_request(request, error)) return false;
	request.stream = download_stream(request.stream);
	const size_t bytes = json_bytes(stream_json(request.stream)).size() + request.media_id.size() + request.type.size() +
		request.video_id.size() + request.title.size() + request.subtitle.size() + request.poster_url.size() +
		request.poster_path.size() + request.background_path.size() + request.logo_path.size() + 1024;
	std::lock_guard lock(p.enqueue_mutex);
	if (p.enqueue_closed || p.enqueue_work.size() >= kMaxJobs || bytes > kMaxPendingBytes - p.pending_bytes) return false;
	p.enqueue_work.push_back({std::move(request), std::move(done), bytes, generation}); p.pending_bytes += bytes;
	try {
	if (!p.enqueue_worker.joinable()) p.enqueue_worker = std::thread([this] {
		auto& state = *impl_;
		for (;;) {
			Impl::EnqueueWork work;
			bool cancelled = false;
			{
				std::unique_lock lock(state.enqueue_mutex);
				state.enqueue_changed.wait(lock, [&] { return state.enqueue_closed || !state.enqueue_work.empty(); });
				if (state.enqueue_closed && state.enqueue_work.empty()) break;
				cancelled = state.enqueue_closed;
				work = std::move(state.enqueue_work.front()); state.enqueue_work.pop_front();
				state.pending_bytes -= work.bytes;
			}
			std::string id, error;
			try {
				if (cancelled) error = "Download storage is not available.";
				else id = enqueue_at_generation(work.request, error, work.generation);
			}
			catch (...) { error = "The download could not be created. Check the available storage space."; }
			try { if (work.done) work.done(std::move(id), std::move(error)); }
			catch (...) { dlog("download queue: completion callback failed"); }
		}
	});
	} catch (...) { p.pending_bytes -= p.enqueue_work.back().bytes; p.enqueue_work.pop_back(); return false; }
	p.enqueue_changed.notify_one();
	return true;
}

std::vector<DownloadEntry> DownloadManager::snapshot() const {
	auto& p = *impl_; std::lock_guard lock(p.published_mutex);
	return p.published;
}

std::optional<DownloadEntry> DownloadManager::find(const std::string& id) const {
	auto& p = *impl_; std::lock_guard lock(p.published_mutex);
	for (const auto& entry : p.published) if (entry.id == id) return entry;
	return std::nullopt;
}

bool DownloadManager::update_poster(const std::string& id, const std::string& cached_path) {
	if (cached_path.empty() || cached_path.size() > 16384) return false;
	return impl_->queue_artwork(id, cached_path, {});
}

bool DownloadManager::update_poster_source(const std::string& id, const std::string& url) {
	if (url.empty() || url.size() > 16384) return false;
	return impl_->queue_artwork(id, {}, url);
}

std::shared_ptr<GrowingFilePlayback> DownloadManager::open_progressive(const std::string& id, std::string& error) {
	error.clear();
	auto& p = *impl_; std::lock_guard lock(p.mutex);
	const auto job = p.lookup(id);
	if (!job || job->removing || !job->entry.playable_while_downloading) {
		error = "Play while downloading is available from 5% of an active download.";
		return {};
	}
	if (!safe_directory(p.root) || !safe_directory(job->directory)) {
		error = "The downloaded file is unavailable.";
		return {};
	}
	auto source = std::make_shared<GrowingFilePlayback>();
	source->path = job->directory + "/media.part";
	source->descriptor = ::open(source->path.c_str(), O_RDONLY | O_NOFOLLOW);
	struct stat st{};
	if (source->descriptor < 0 || ::fstat(source->descriptor, &st) || !S_ISREG(st.st_mode) ||
		!progressive_download_threshold(std::min<int64_t>(st.st_size, job->entry.done), job->entry.total)) {
		error = "The downloaded file is not ready for playback yet.";
		return {};
	}
	source->state = job->playback; source->generation = job->playback_generation;
	source->total = job->entry.total;
	if (job->torrent_file_idx >= 0) {
		source->torrent_hash = job->request.stream.info_hash;
		source->torrent_file_idx = job->torrent_file_idx;
	}
	return source;
}

bool DownloadManager::pause(const std::string& id) {
	auto& p = *impl_; std::lock_guard lock(p.mutex); const auto j = p.lookup(id);
	if (!j || j->removing || j->entry.state == DownloadState::Complete) return false;
	j->entry.state = DownloadState::Paused; j->entry.bytes_per_second = 0;
	if (p.active_id == id) p.cancel.store(true);
	p.save(*j); p.touch(); p.changed.notify_all(); return true;
}

bool DownloadManager::resume(const std::string& id) {
	auto& p = *impl_; std::lock_guard lock(p.mutex); const auto j = p.lookup(id);
	if (!j || j->removing || j->entry.state == DownloadState::Complete || p.active_id == id) return false;
	std::string error;
	if (!valid_request(j->request, error)) { j->entry.state = DownloadState::Failed; j->entry.error = error; p.touch(); return false; }
	j->entry.state = torrent_stream(j->request.stream) && p.torrent_reserved() ? DownloadState::Waiting : DownloadState::Queued;
	j->entry.error.clear(); p.save(*j); p.touch(); p.changed.notify_all(); return true;
}

bool DownloadManager::remove(const std::string& id, std::string& error) {
	error.clear(); auto& p = *impl_; std::lock_guard lock(p.mutex); const auto j = p.lookup(id);
	if (!safe_id(id) || !j || j->removing || !safe_directory(p.root) || !safe_directory(j->directory)) {
		error = "This download cannot be deleted."; return false;
	}
	j->removing = true;
	if (!p.save(*j)) { j->removing = false; error = "The delete request could not be saved."; return false; }
	if (p.active_id == id) p.cancel.store(true);
	p.touch(); p.changed.notify_all(); return true;
}

uint64_t DownloadManager::revision() const { return impl_->revision.load(std::memory_order_relaxed); }

bool DownloadManager::storage_available() const {
	auto& p = *impl_; std::lock_guard lock(p.mutex); return p.ready;
}

std::string DownloadManager::storage_error() const {
	auto& p = *impl_; std::lock_guard lock(p.mutex); return p.storage_error;
}

void DownloadManager::set_enabled(bool enabled) {
	auto& p = *impl_; std::lock_guard lock(p.mutex);
	p.enabled = enabled;
	if (!enabled) p.suspend_generation.fetch_add(1);
	if (!enabled && !p.active_id.empty()) {
		p.cancel.store(true); const auto j = p.lookup(p.active_id);
		if (j) { j->entry.state = DownloadState::Paused; j->entry.bytes_per_second = 0; p.save(*j); }
	}
	p.touch(); p.changed.notify_all();
}

void DownloadManager::set_torrent_playback_active(bool active) {
	auto& p = *impl_; std::lock_guard lock(p.mutex);
	p.foreground_torrent = active;
	if (active && p.active_torrent) { p.torrent_preempted = true; p.cancel.store(true); }
	for (const auto& j : p.jobs) if (torrent_stream(j->request.stream) && !j->removing) {
		if (active && j->entry.state == DownloadState::Queued) j->entry.state = DownloadState::Waiting;
		else if (!p.torrent_reserved() && j->entry.state == DownloadState::Waiting) j->entry.state = DownloadState::Queued;
	}
	p.touch(); p.changed.notify_all();
}

bool DownloadManager::wait_for_torrent_idle(const std::atomic<bool>& cancel) {
	auto& p = *impl_; std::unique_lock lock(p.mutex);
	while (p.active_torrent && !cancel.load()) p.changed.wait_for(lock, std::chrono::milliseconds(50));
	return !p.active_torrent && !cancel.load();
}

std::shared_ptr<void> DownloadManager::acquire_torrent_resolution() {
	auto& p = *impl_;
	std::lock_guard lock(p.mutex);
	const auto counter = p.foreground_resolvers;
	auto* token = new unsigned char(0);
	counter->fetch_add(1);
	if (p.active_torrent) { p.torrent_preempted = true; p.cancel.store(true); }
	p.changed.notify_all();
	// The counter has independent shared lifetime: a cancelled task may be
	// destroyed after the manager. Its release must never dereference the App.
	return std::shared_ptr<void>(token, [counter](void* token) {
		delete static_cast<unsigned char*>(token);
		counter->fetch_sub(1);
	});
}
