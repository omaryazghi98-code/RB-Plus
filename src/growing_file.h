// A live, contiguous download prefix. It is never a completed offline file.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>

enum class GrowingFilePhase { Preparing, Downloading, Paused, Waiting, Failed, Complete, Removed };

struct GrowingFileSnapshot {
	int64_t available = 0, total = -1;
	uint64_t generation = 0;
	GrowingFilePhase phase = GrowingFilePhase::Preparing;
};

// Independent shared lifetime: a decoder must never call back into a destroyed
// App or DownloadManager. The manager publishes only bytes actually written.
class GrowingFileState {
public:
	void publish(GrowingFileSnapshot value) {
		{ std::lock_guard lock(mutex_); value_ = value; }
		changed_.notify_all();
	}
	GrowingFileSnapshot snapshot() const { std::lock_guard lock(mutex_); return value_; }
	void wait() const {
		std::unique_lock lock(mutex_);
		changed_.wait_for(lock, std::chrono::milliseconds(50));
	}
private:
	mutable std::mutex mutex_;
	mutable std::condition_variable changed_;
	GrowingFileSnapshot value_;
};

struct GrowingFilePlayback {
	// An already-open regular file, kept valid across the final atomic rename.
	// The cursor belongs to this lease, with serialized seek/read operations.
	int descriptor = -1;
	std::mutex io_mutex;
	std::string path;
	std::shared_ptr<GrowingFileState> state;
	uint64_t generation = 0;
	int64_t total = -1;
	// Optional read-through of bounded container metadata from the same torrent.
	// This never starts another torrent or writes into the offline file.
	std::string torrent_hash;
	int torrent_file_idx = -1;
	~GrowingFilePlayback() { if (descriptor >= 0) ::close(descriptor); }
	GrowingFilePlayback() = default;
	GrowingFilePlayback(const GrowingFilePlayback&) = delete;
	GrowingFilePlayback& operator=(const GrowingFilePlayback&) = delete;
};

inline bool progressive_download_threshold(int64_t available, int64_t total) {
	// ceil(total / 20), without multiplication overflow or float rounding.
	return total > 0 && available >= total / 20 + (total % 20 != 0);
}
