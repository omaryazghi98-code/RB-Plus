#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// Estimate the remaining wall-clock time from the prefix actually written to
// the destination. Network bursts and resume bytes do not count as newly saved
// media; time spent syncing storage and waiting for peers is part of the rate.
// A monotonic clock is supplied explicitly so the same rules are testable
// without sleeping or replacing the production transfer.
class DownloadRateEstimate {
public:
	void reset(int64_t done, double now) {
		previous_done_ = std::max<int64_t>(0, done);
		started_ = sampled_ = advanced_ = now;
		rate_ = 0; have_sample_ = false;
	}

	void sample(int64_t done, double now) {
		done = std::max<int64_t>(0, done);
		if (!std::isfinite(now) || now < sampled_ || done < previous_done_) { reset(done, now); return; }
		const double elapsed = now - sampled_;
		// Aggregate several callbacks into a useful sample rather than treating a
		// 1-MiB write burst as an instantaneous stable transfer speed.
		if (elapsed < 1.0) return;
		const int64_t gained = done - previous_done_;
		const double current = double(gained) / elapsed;
		if (gained > 0) advanced_ = now;
		if (!have_sample_) { rate_ = current; have_sample_ = gained > 0; }
		else rate_ += (1.0 - std::exp(-elapsed / 8.0)) * (current - rate_);
		if (now - advanced_ >= 15.0 || !std::isfinite(rate_)) { rate_ = 0; have_sample_ = false; }
		previous_done_ = done; sampled_ = now;
	}

	double bytes_per_second() const { return have_sample_ ? std::max(0.0, rate_) : 0; }

	int64_t remaining_seconds(int64_t done, int64_t total) const {
		if (total <= 0) return -1;
		if (done >= total) return 0;
		if (!have_sample_ || sampled_ - started_ < 1.0 || rate_ <= 0) return -1;
		const double remaining = std::ceil(double(total - std::max<int64_t>(0, done)) / rate_);
		if (!std::isfinite(remaining) || remaining >= double(std::numeric_limits<int64_t>::max())) return -1;
		return std::max<int64_t>(1, int64_t(remaining));
	}

private:
	int64_t previous_done_ = 0;
	double started_ = 0, sampled_ = 0, advanced_ = 0, rate_ = 0;
	bool have_sample_ = false;
};
