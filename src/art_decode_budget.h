// Weighted admission for artwork decoders. No pixel allocation happens here.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

class ArtDecodeBudget {
public:
    class Lease {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept : budget_(std::exchange(other.budget_, nullptr)), charge_(other.charge_) {}
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) { reset(); budget_ = std::exchange(other.budget_, nullptr); charge_ = other.charge_; }
            return *this;
        }
        ~Lease() { reset(); }
        explicit operator bool() const { return budget_ != nullptr; }
        void reset() {
            if (!budget_) return;
            {
                std::lock_guard<std::mutex> lock(budget_->mutex_);
                budget_->used_ -= charge_;
            }
            budget_->wake_.notify_all();
            budget_ = nullptr;
        }
    private:
        friend class ArtDecodeBudget;
        Lease(ArtDecodeBudget& budget, size_t charge) : budget_(&budget), charge_(charge) {}
        ArtDecodeBudget* budget_ = nullptr;
        size_t charge_ = 0;
    };

    explicit ArtDecodeBudget(size_t limit = 64u << 20) : limit_(std::max<size_t>(1, limit)) {}

    // Requests bigger than the pool consume it exclusively. They are already
    // dimension-bounded before admission, so no valid single image starves.
    Lease acquire(size_t bytes, const std::atomic<bool>* cancel = nullptr) {
        const auto cancelled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
        const size_t charge = std::min(limit_, std::max<size_t>(1, bytes));
        std::unique_lock<std::mutex> lock(mutex_);
        const char ticket = 0;
        waiting_.push_back(&ticket);
        // FIFO admission prevents a waiting large hero from being overtaken
        // forever by a stream of smaller poster decodes.
        while (!cancelled() && (waiting_.front() != &ticket || used_ > limit_ - charge))
            wake_.wait_for(lock, std::chrono::milliseconds(20));
        if (cancelled()) {
            waiting_.erase(std::find(waiting_.begin(), waiting_.end(), &ticket));
            lock.unlock(); wake_.notify_all();
            return {};
        }
        waiting_.pop_front();
        used_ += charge;
        lock.unlock(); wake_.notify_all();
        return Lease(*this, charge);
    }

private:
    const size_t limit_;
    size_t used_ = 0;
    std::deque<const void*> waiting_;
    std::mutex mutex_;
    std::condition_variable wake_;
};
