#pragma once

#include "growing_file.h"

#include <thread>

struct AVIOContext;
namespace bt { struct Torrent; }

// FFmpeg input over an active download. Temporary end-of-prefix is a wait,
// never EOF. Completed files continue through their held descriptor after rename.
class GrowingFileStream {
public:
	~GrowingFileStream();
	static AVIOContext* open_avio(std::shared_ptr<GrowingFilePlayback> source,
		const std::atomic<bool>* abort, const std::atomic<bool>* reposition, std::string* error);
	static GrowingFileStream* of(AVIOContext* pb);
	static void close_avio(AVIOContext** pb);
	int read(uint8_t* buffer, int count);
	int64_t seek(int64_t offset, int whence);
	std::string peek(size_t count);
	const std::string& failure() const { return failure_; }
	void finish_opening();
	// Demux thread only. Refresh against the held descriptor when admitting a
	// seek; ordinary packets reuse the physical prefix validated by read().
	int64_t available_bytes();
	int64_t validated_bytes() const { return validated_bytes_; }
private:
	bool usable(const GrowingFileSnapshot& status);
	int64_t available(const GrowingFileSnapshot& status);
	int auxiliary_read(uint8_t* buffer, int count);
	void stop_auxiliary();
	std::shared_ptr<GrowingFilePlayback> source_;
	const std::atomic<bool>* abort_ = nullptr;
	const std::atomic<bool>* reposition_ = nullptr;
	int64_t position_ = 0;
	int64_t validated_bytes_ = 0;
	AVIOContext* context_ = nullptr;
	bool opening_ = true;
	std::string failure_;
	std::shared_ptr<bt::Torrent> torrent_;
	int reader_ = -1;
	int64_t auxiliary_bytes_ = 0;
	std::atomic<bool> auxiliary_abort_{false}, auxiliary_finished_{false};
	std::atomic<int64_t> auxiliary_deadline_ms_{0};
	std::thread auxiliary_monitor_;
};
