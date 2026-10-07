#pragma once

#include "stremio.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// One selected stream only. All paths are private, pre-created paths owned by
// DownloadManager. The transport never writes outside work_dir and never
// publishes a finished media file: the manager atomically promotes partial_path.
struct DownloadTransferRequest {
	Stream stream;
	std::string work_dir;
	std::string partial_path;     // <work_dir>/media.part
	std::string checkpoint_path;  // <work_dir>/transfer.json, private mode 0600
	bool allow_resume = true;
	int season = -1;
	int episode = -1;
};

struct DownloadTransferProgress {
	// Live contiguous prefix successfully written to media.part. It can exceed
	// the durable checkpoint between commits. The first callback follows resume
	// identity validation, tail truncation and the initial durable checkpoint.
	int64_t done = 0;
	int64_t total = -1;
	double bytes_per_second = 0;
	// Actual resolved file index for native torrent downloads, including an
	// episode selected automatically from multi-file metadata. Otherwise -1.
	int torrent_file_idx = -1;
	int64_t remaining_seconds = -1;
	int connected_peers = -1, connected_seeders = -1;
};

using DownloadProgressCallback = std::function<void(const DownloadTransferProgress&)>;

enum class DownloadTransferStatus { Complete, Cancelled, Unsupported, Error };

struct DownloadTransferResult {
	DownloadTransferStatus status = DownloadTransferStatus::Error;
	// Safe user-facing reason, never a provider URL, header or token.
	std::string error;
	// Container suffix only (for example .mp4, .mkv or .ts). The manager
	// validates this and falls back to .media; it is never treated as a path.
	std::string extension = ".media";
	int64_t done = 0;
	int64_t total = -1;
};

// Blocking worker operation. Progress callbacks may be invoked frequently but
// must finish quickly. Check cancel during network waits and file operations.
// Resume byte identity (ETag / Last-Modified, HLS state) lives in checkpoint_path;
// unsafe or unverifiable partial transfers must restart, never append blindly.
// Complete means partial_path is a closed, playable, fully downloaded file.
// Cancelled preserves safe partial state. No credentials are written to logs.
DownloadTransferResult download_transfer(const DownloadTransferRequest& request,
	const DownloadProgressCallback& progress, const std::atomic<bool>& cancel);
