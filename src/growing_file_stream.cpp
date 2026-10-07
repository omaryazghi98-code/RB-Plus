#include "growing_file_stream.h"
#include "torrent/engine.h"
#include "util.h"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <cerrno>
#include <climits>
#include <cstdio>
#include <sys/stat.h>

namespace {
constexpr int64_t kAuxiliaryLimit = 32ll << 20;
constexpr int64_t kOpeningLocalWait = 1ll << 20;
int64_t milliseconds() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}
int read_callback(void* opaque, uint8_t* buffer, int count) {
	return static_cast<GrowingFileStream*>(opaque)->read(buffer, count);
}
int64_t seek_callback(void* opaque, int64_t offset, int whence) {
	return static_cast<GrowingFileStream*>(opaque)->seek(offset, whence);
}
constexpr const char* kMetadataPending = "This video's playback metadata is not downloaded yet. Let the download continue and try again.";
}

GrowingFileStream::~GrowingFileStream() { stop_auxiliary(); }

bool GrowingFileStream::usable(const GrowingFileSnapshot& status) {
	if (status.generation != source_->generation) failure_ = "The download restarted. Start playback again.";
	else if (status.phase == GrowingFilePhase::Removed) failure_ = "The download was removed.";
	else if (status.total != source_->total) failure_ = "The download source changed. Start playback again.";
	return failure_.empty();
}

int64_t GrowingFileStream::available(const GrowingFileSnapshot& status) {
	struct stat st{};
	if (::fstat(source_->descriptor, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) {
		failure_ = "The downloaded file is unavailable.";
		return -1;
	}
	validated_bytes_ = std::max<int64_t>(0, std::min<int64_t>({status.available, source_->total, st.st_size}));
	return validated_bytes_;
}

int64_t GrowingFileStream::available_bytes() {
	const auto status = source_->state->snapshot();
	return usable(status) ? available(status) : -1;
}

void GrowingFileStream::finish_opening() {
	opening_ = false;
	stop_auxiliary();
	if (!context_) return;
	// A seek satisfied inside AVIO's cache would bypass our byte guard,
	// including metadata fetched past the prefix during opening. Direct
	// seeking always calls the custom seek callback (FFmpeg AVIO contract).
	context_->direct = 1;
	const int64_t bytes = available_bytes();
	const int64_t cursor = context_->pos - (context_->buf_end - context_->buf_ptr);
	if (bytes >= 0 && context_->pos > bytes) {
		const int64_t buffered = std::max<int64_t>(0, bytes - cursor);
		context_->buf_end = context_->buf_ptr + std::min<int64_t>(buffered, context_->buf_end - context_->buf_ptr);
		context_->pos = std::max(cursor, bytes);
		position_ = context_->pos;
	}
}

void GrowingFileStream::stop_auxiliary() {
	auxiliary_finished_.store(true);
	auxiliary_abort_.store(true);
	if (auxiliary_monitor_.joinable()) auxiliary_monitor_.join();
	if (torrent_) bt::Engine::get().close_reader(torrent_, reader_);
	torrent_.reset();
}

int GrowingFileStream::auxiliary_read(uint8_t* buffer, int count) {
	if (source_->torrent_hash.empty() || source_->torrent_file_idx < 0 || auxiliary_bytes_ >= kAuxiliaryLimit) {
		failure_ = kMetadataPending;
		return AVERROR(EAGAIN);
	}
	if (!torrent_) {
		int64_t size = 0;
		std::string error;
		torrent_ = bt::Engine::get().open_reader(source_->torrent_hash, source_->torrent_file_idx,
			&reader_, &size, &error, bt::ReaderRole::Auxiliary);
		if (!torrent_ || size != source_->total) {
			failure_ = kMetadataPending;
			return AVERROR(EIO);
		}
		auxiliary_monitor_ = std::thread([this] {
			while (!auxiliary_finished_.load()) {
				const auto status = source_->state->snapshot();
				const auto deadline = auxiliary_deadline_ms_.load();
				if ((abort_ && abort_->load()) || status.generation != source_->generation ||
					status.phase == GrowingFilePhase::Removed || status.phase == GrowingFilePhase::Failed ||
					status.phase == GrowingFilePhase::Paused || (deadline && milliseconds() >= deadline)) {
					auxiliary_abort_.store(true);
					break;
				}
				source_->state->wait();
			}
		});
		dlog("download playback: reading container metadata from the existing torrent");
	}
	auxiliary_deadline_ms_.store(milliseconds() + 30000);
	const int wanted = int(std::min<int64_t>({count, 64 << 10, kAuxiliaryLimit - auxiliary_bytes_}));
	const int received = bt::Engine::get().read(torrent_, reader_, source_->torrent_file_idx,
		position_, buffer, wanted, &auxiliary_abort_);
	auxiliary_deadline_ms_.store(0);
	if (abort_ && abort_->load()) return AVERROR_EXIT;
	if (!usable(source_->state->snapshot())) return AVERROR(EIO);
	if (received <= 0) { failure_ = kMetadataPending; return AVERROR(EIO); }
	auxiliary_bytes_ += received;
	position_ += received;
	return received;
}

int GrowingFileStream::read(uint8_t* buffer, int count) {
	if (!buffer || count <= 0) return AVERROR(EINVAL);
	int64_t last_available = -1, deadline = milliseconds() + 120000;
	for (;;) {
		if (abort_ && abort_->load()) return AVERROR_EXIT;
		if (reposition_ && reposition_->load()) return AVERROR(EAGAIN);
		const auto status = source_->state->snapshot();
		if (!usable(status)) return AVERROR(EIO);
		if (position_ >= source_->total) return AVERROR_EOF;
		const int64_t bytes = available(status);
		if (bytes < 0) return AVERROR(EIO);
		if (bytes != last_available) { last_available = bytes; deadline = milliseconds() + 120000; }
		if (position_ < bytes) {
			const size_t wanted = size_t(std::min<int64_t>(count, bytes - position_));
			std::lock_guard io_lock(source_->io_mutex);
			const ssize_t got = ::lseek(source_->descriptor, off_t(position_), SEEK_SET) < 0 ? -1 :
				::read(source_->descriptor, buffer, wanted);
			if (got < 0 && errno == EINTR) continue;
			if (got <= 0) { failure_ = "The downloaded file could not be read."; return AVERROR(EIO); }
			// A restart may truncate/replace content concurrently with the disk
			// read. Never return those bytes into the old decoder generation.
			if (!usable(source_->state->snapshot())) return AVERROR(EIO);
			position_ += got;
			return int(got);
		}
		// Only reaching the prefix sequentially may wait for the writer. An
		// unsupported demuxer seek must not block on arbitrary future bytes.
		if (!opening_ && position_ > bytes && status.phase != GrowingFilePhase::Complete)
			return AVERROR(EAGAIN);
		if (status.phase == GrowingFilePhase::Complete) {
			if (position_ >= source_->total && bytes == source_->total) return AVERROR_EOF;
			failure_ = "The downloaded file is incomplete.";
			return AVERROR(EIO);
		}
		if (status.phase == GrowingFilePhase::Paused || status.phase == GrowingFilePhase::Waiting) {
			failure_ = "The downloaded portion has ended. Resume the download to keep watching.";
			return AVERROR(EAGAIN);
		}
		if (status.phase == GrowingFilePhase::Failed) {
			failure_ = "The download stopped before the next part of the video arrived.";
			return AVERROR(EIO);
		}
		if (opening_ && position_ > bytes && position_ - bytes > kOpeningLocalWait && position_ < source_->total)
			return auxiliary_read(buffer, count);
		if (milliseconds() >= deadline) {
			failure_ = "The download has not provided more data. Resume it from Downloads.";
			return AVERROR(ETIMEDOUT);
		}
		source_->state->wait();
	}
}

int64_t GrowingFileStream::seek(int64_t offset, int whence) {
	if (whence & AVSEEK_SIZE) return source_->total;
	whence &= ~AVSEEK_FORCE;
	int64_t base = 0;
	if (whence == SEEK_CUR) base = position_;
	else if (whence == SEEK_END) base = source_->total;
	else if (whence != SEEK_SET) return AVERROR(EINVAL);
	if ((offset > 0 && base > INT64_MAX - offset) || (offset < 0 && offset < -base)) return AVERROR(EINVAL);
	const int64_t target = base + offset;
	if (target < 0 || target > source_->total) return AVERROR(EINVAL);
	const auto status = source_->state->snapshot();
	if (!usable(status)) return AVERROR(EIO);
	const auto bytes = available(status);
	if (bytes < 0) return AVERROR(EIO);
	// Opening may read an index at the tail. During playback, only seek within
	// the downloaded prefix; ordinary sequential reads still wait for new bytes.
	if (!opening_ && target > bytes) return AVERROR(EAGAIN);
	position_ = target;
	return position_;
}

std::string GrowingFileStream::peek(size_t count) {
	count = std::min<size_t>(count, 4096);
	std::string result(count, '\0');
	const int64_t saved = position_;
	const int got = count ? read(reinterpret_cast<uint8_t*>(result.data()), int(count)) : 0;
	position_ = saved;
	result.resize(size_t(std::max(0, got)));
	return result;
}

AVIOContext* GrowingFileStream::open_avio(std::shared_ptr<GrowingFilePlayback> source,
	const std::atomic<bool>* abort, const std::atomic<bool>* reposition, std::string* error) {
	if (!source || !source->state || source->descriptor < 0 || source->total <= 0) {
		if (error) *error = "The downloaded file is unavailable.";
		return nullptr;
	}
	auto stream = std::make_unique<GrowingFileStream>();
	stream->source_ = std::move(source); stream->abort_ = abort; stream->reposition_ = reposition;
	const auto status = stream->source_->state->snapshot();
	if (!stream->usable(status) || stream->available(status) < 0) {
		if (error) *error = stream->failure();
		return nullptr;
	}
	constexpr int buffer_size = 256 << 10;
	auto* buffer = static_cast<uint8_t*>(av_malloc(buffer_size));
	AVIOContext* pb = buffer ? avio_alloc_context(buffer, buffer_size, 0, stream.get(), read_callback, nullptr, seek_callback) : nullptr;
	if (!pb) { av_free(buffer); if (error) *error = "Out of memory."; return nullptr; }
	pb->seekable = AVIO_SEEKABLE_NORMAL;
	stream->context_ = pb;
	stream.release();
	dlog("download playback: local prefix ready=%lld total=%lld generation=%llu",
		(long long)status.available, (long long)status.total, (unsigned long long)status.generation);
	return pb;
}

GrowingFileStream* GrowingFileStream::of(AVIOContext* pb) {
	return pb && pb->read_packet == read_callback ? static_cast<GrowingFileStream*>(pb->opaque) : nullptr;
}

void GrowingFileStream::close_avio(AVIOContext** pb) {
	if (!pb || !*pb) return;
	delete of(*pb);
	av_freep(&(*pb)->buffer);
	avio_context_free(pb);
}
