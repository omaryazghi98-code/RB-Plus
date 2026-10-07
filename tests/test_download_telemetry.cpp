#include "download_telemetry.h"

#include <iostream>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool value, const char* message) {
	++checks; if (!value) throw std::runtime_error(message);
}
}

int main() {
	try {
		constexpr int64_t MiB = 1ll << 20;
		DownloadRateEstimate estimate;
		int64_t done = 80 * MiB;
		estimate.reset(done, 100);
		check(estimate.bytes_per_second() == 0 && estimate.remaining_seconds(done, 100 * MiB) == -1,
			"resume has no estimate until fresh byte/time evidence exists");
		estimate.sample(done + MiB, 100.05);
		check(estimate.remaining_seconds(done + MiB, 100 * MiB) == -1, "a short initial write burst cannot invent an ETA");
		for (int n = 1; n <= 5; ++n) { done += MiB; estimate.sample(done, 100 + n); }
		check(estimate.bytes_per_second() == MiB && estimate.remaining_seconds(done, 100 * MiB) == 15,
			"steady new bytes estimate remaining wall time without counting the resumed 80 MiB");
		done += 10 * MiB; estimate.sample(done, 106);
		check(estimate.bytes_per_second() > MiB && estimate.bytes_per_second() < 3 * MiB,
			"one cached burst cannot turn a stable rate into a tenfold speed estimate");
		check(estimate.remaining_seconds(done, -1) == -1 && estimate.remaining_seconds(done, 0) == -1,
			"unknown totals never produce invented completion times");
		const auto before_stall = estimate.bytes_per_second();
		for (int n = 107; n < 115; ++n) estimate.sample(done, n);
		check(estimate.bytes_per_second() > 0 && estimate.bytes_per_second() < before_stall,
			"time blocked on peers or storage reduces the useful rate smoothly");
		estimate.sample(done, 121);
		check(estimate.bytes_per_second() == 0 && estimate.remaining_seconds(done, 200 * MiB) == -1,
			"a prolonged stall clears stale speed and ETA rather than freezing them on screen");
		done += MiB; estimate.sample(done, 122);
		check(estimate.bytes_per_second() == MiB && estimate.remaining_seconds(done, done + 10 * MiB) == 10,
			"a resumed advancing transfer obtains a fresh useful estimate");
		check(estimate.remaining_seconds(done, done) == 0, "complete bytes always have zero remaining time");
		estimate.sample(0, 123);
		check(estimate.bytes_per_second() == 0 && estimate.remaining_seconds(0, 100 * MiB) == -1,
			"identity reconciliation which truncates a prefix resets the old rate");
		std::cout << "Download telemetry: " << checks << " assertions passed\n";
		return 0;
	} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
