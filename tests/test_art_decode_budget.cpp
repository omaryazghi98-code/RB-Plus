#include "art_decode_budget.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace {
int checks = 0;
void expect(bool value, const char* description) {
    ++checks;
    if (!value) throw std::runtime_error(description);
}
void weighted_admission() {
    // Units stand in for MiB; no large buffers are allocated in these checks.
    ArtDecodeBudget budget(64);
    auto first = budget.acquire(48);
    auto second = budget.acquire(16);
    expect(bool(first) && bool(second), "ordinary jobs may share the exact 64-unit budget");
    auto entered = std::async(std::launch::async, [&] { return budget.acquire(1); });
    const bool blocked = entered.wait_for(40ms) == std::future_status::timeout;
    second.reset();
    const bool admitted = entered.wait_for(500ms) == std::future_status::ready;
    auto third = entered.get();
    expect(blocked, "another allocation waits instead of exceeding the budget");
    expect(admitted && bool(third), "released weight admits waiting work");
    auto moved = std::move(first);
    expect(!first && bool(moved), "moving a lease keeps exactly one budget owner");
}
void oversized_exclusive() {
    ArtDecodeBudget budget(64);
    auto small = budget.acquire(1);
    auto big_future = std::async(std::launch::async, [&] { return budget.acquire(136); });
    const bool big_waited = big_future.wait_for(40ms) == std::future_status::timeout;
    small.reset();
    auto big = big_future.get();
    auto next = std::async(std::launch::async, [&] { return budget.acquire(1); });
    const bool next_waited = next.wait_for(40ms) == std::future_status::timeout;
    big.reset();
    auto after = next.get();
    expect(big_waited, "an oversized image waits for all ordinary decodes to finish");
    expect(next_waited && bool(after), "an oversized image runs exclusively, then releases the full budget");
}
void cancellation_and_fifo() {
    ArtDecodeBudget budget(64);
    auto full = budget.acquire(64);
    std::atomic<bool> cancel = false;
    auto cancelled = std::async(std::launch::async, [&] { return budget.acquire(136, &cancel); });
    const bool waited = cancelled.wait_for(40ms) == std::future_status::timeout;
    auto behind = std::async(std::launch::async, [&] { return budget.acquire(2); });
    cancel = true;
    const bool returned = cancelled.wait_for(500ms) == std::future_status::ready;
    auto empty = cancelled.get();
    expect(waited && returned && !empty, "cancellation interrupts admission while another decode still holds all memory");
    full.reset();
    expect(bool(behind.get()), "cancelled front waiter does not strand later requests");
    cancel = true;
    expect(!budget.acquire(1, &cancel), "already-cancelled work allocates no budget");

    auto initial = budget.acquire(1);
    std::promise<void> large_started;
    auto started = large_started.get_future();
    auto large_future = std::async(std::launch::async, [&] { large_started.set_value(); return budget.acquire(80); });
    started.wait();
    const bool large_blocked = large_future.wait_for(40ms) == std::future_status::timeout;
    auto tiny_future = std::async(std::launch::async, [&] { return budget.acquire(1); });
    const bool tiny_blocked = tiny_future.wait_for(40ms) == std::future_status::timeout;
    initial.reset();
    auto large = large_future.get();
    const bool still_blocked = tiny_future.wait_for(40ms) == std::future_status::timeout;
    large.reset();
    auto tiny = tiny_future.get();
    expect(large_blocked && tiny_blocked && still_blocked && bool(tiny), "queued large image is not starved by later small work");
}
} // namespace

int main() {
    try {
        weighted_admission(); oversized_exclusive(); cancellation_and_fifo();
        std::cout << "PASS " << checks << " decode admission checks (64-unit budget; no large allocations).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
