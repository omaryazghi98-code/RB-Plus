// Stremio - Artwork-derived controller light, with bounded background sampling.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "controller_ambient.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {
constexpr int kSampleSide = 64;
constexpr int kMaxDimension = 8192;
constexpr std::uint64_t kMaxPixels = 16u * 1024u * 1024u;
constexpr std::size_t kPaletteCacheEntries = 96;
constexpr float kSelectionDelay = 0.25f;
constexpr float kApplyPeriod = 0.05f;

bool valid_dimensions(int width, int height) {
    return width > 0 && height > 0 && width <= kMaxDimension && height <= kMaxDimension &&
           std::uint64_t(width) * std::uint64_t(height) <= kMaxPixels;
}

std::uint32_t little_u32(const unsigned char* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

struct Hsv { float h, s, v; };

Hsv to_hsv(float r, float g, float b) {
    const float hi = std::max({r, g, b}), lo = std::min({r, g, b}), delta = hi - lo;
    float h = 0;
    if (delta > 0.0001f) {
        if (hi == r) h = (g - b) / delta;
        else if (hi == g) h = (b - r) / delta + 2;
        else h = (r - g) / delta + 4;
        h /= 6;
        if (h < 0) h += 1;
    }
    return {h, hi > 0.0001f ? delta / hi : 0, hi};
}

ControllerRgb from_hsv(Hsv hsv) {
    const float h = hsv.h * 6, c = hsv.v * hsv.s, m = hsv.v - c;
    const float x = c * (1 - std::abs(std::fmod(h, 2.0f) - 1));
    float r = 0, g = 0, b = 0;
    if (h < 1) { r = c; g = x; }
    else if (h < 2) { r = x; g = c; }
    else if (h < 3) { g = c; b = x; }
    else if (h < 4) { g = x; b = c; }
    else if (h < 5) { r = x; b = c; }
    else { r = c; b = x; }
    const auto channel = [m](float value) {
        return static_cast<std::uint8_t>(std::clamp(std::lround((value + m) * 255), 0l, 255l));
    };
    return {channel(r), channel(g), channel(b)};
}

// Hue voting does not average opposing colors into mud. Neighbor weighting
// avoids unstable choices when a gradient straddles a 15-degree bin boundary.
class Palette {
public:
    void add(const unsigned char* rgba) {
        if (rgba[3] < 96) return;
        const float alpha = float(rgba[3]) / 255;
        const float r = float(rgba[0]) / 255, g = float(rgba[1]) / 255, b = float(rgba[2]) / 255;
        const auto hsv = to_hsv(r, g, b);
        opaque_ += alpha;
        gray_sum_ += (0.2126f * r + 0.7152f * g + 0.0722f * b) * alpha;
        // White lettering and black mattes should not erase a poster's palette.
        if (hsv.v < 0.14f || hsv.s < 0.18f) {
            if (hsv.v >= 0.14f && hsv.v <= 0.90f) neutral_ += alpha * 0.6f;
            return;
        }
        const float weight = alpha * (0.5f + 0.5f * hsv.s) * (0.75f + 0.25f * hsv.v);
        auto& bin = bins_[std::min(23, int(hsv.h * 24))];
        bin.weight += weight;
        bin.r += r * weight; bin.g += g * weight; bin.b += b * weight;
    }

    bool finish(ControllerRgb& color) const {
        if (opaque_ <= 0) return false;
        int best = 0;
        float score = 0;
        for (int i = 0; i < 24; ++i) {
            const float candidate = bins_[i].weight + 0.25f *
                (bins_[(i + 23) % 24].weight + bins_[(i + 1) % 24].weight);
            if (candidate > score) { best = i; score = candidate; }
        }
        if (score <= 0 || score < neutral_ * 0.5f) {
            const auto gray = static_cast<std::uint8_t>(std::clamp(
                std::lround(gray_sum_ / opaque_ * 255 * 0.65f), 64l, 150l));
            color = {gray, gray, gray};
            return true;
        }
        const auto& center = bins_[best];
        const auto& left = bins_[(best + 23) % 24];
        const auto& right = bins_[(best + 1) % 24];
        const float weight = center.weight + 0.25f * (left.weight + right.weight);
        Hsv hsv = to_hsv((center.r + 0.25f * (left.r + right.r)) / weight,
                        (center.g + 0.25f * (left.g + right.g)) / weight,
                        (center.b + 0.25f * (left.b + right.b)) / weight);
        hsv.s = std::clamp(hsv.s, 0.22f, 0.85f);
        hsv.v = std::clamp(hsv.v * 0.75f, 0.32f, 0.72f);
        color = from_hsv(hsv);
        return true;
    }

private:
    struct Bin { float r = 0, g = 0, b = 0, weight = 0; };
    std::array<Bin, 24> bins_{};
    float opaque_ = 0, gray_sum_ = 0, neutral_ = 0;
};

int sample_position(int sample, int count, int extent) {
    return int((std::uint64_t(sample * 2 + 1) * std::uint64_t(extent)) / std::uint64_t(count * 2));
}

bool read_cached_palette(const std::string& path, ControllerRgb& color,
                         const std::atomic<std::uint64_t>* generation, std::uint64_t expected) {
    if (path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos ||
        !path.ends_with(".rgba")) return false;
    const auto canceled = [&] { return generation && generation->load() != expected; };
    if (canceled()) return false;
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "rb"), &std::fclose);
    if (!file) return false;
    unsigned char header[12];
    if (std::fread(header, 1, sizeof(header), file.get()) != sizeof(header) ||
        std::memcmp(header, "RGBA", 4) != 0) return false;
    const auto raw_w = little_u32(header + 4), raw_h = little_u32(header + 8);
    if (raw_w > kMaxDimension || raw_h > kMaxDimension) return false;
    const int width = int(raw_w), height = int(raw_h);
    if (!valid_dimensions(width, height)) return false;
    const long expected_size = long(12 + std::uint64_t(width) * std::uint64_t(height) * 4);
    if (std::fseek(file.get(), 0, SEEK_END) != 0 || std::ftell(file.get()) != expected_size) return false;
    Palette palette;
    // A 32 KiB maximum row replaces a full 64 MiB decode/copy. At most 64
    // rows are read, and at most 4096 pixel samples enter the histogram.
    std::vector<unsigned char> row(std::size_t(width) * 4);
    const int nx = std::min(width, kSampleSide), ny = std::min(height, kSampleSide);
    for (int y = 0; y < ny; ++y) {
        if (canceled()) return false;
        const long offset = long(12 + std::uint64_t(sample_position(y, ny, height)) * std::uint64_t(width) * 4);
        if (std::fseek(file.get(), offset, SEEK_SET) != 0 ||
            std::fread(row.data(), 1, row.size(), file.get()) != row.size()) return false;
        for (int x = 0; x < nx; ++x) palette.add(row.data() + sample_position(x, nx, width) * 4);
    }
    return !canceled() && palette.finish(color);
}
} // namespace

bool dominant_controller_color(std::span<const unsigned char> rgba, int width, int height,
                               ControllerRgb& color) {
    if (!valid_dimensions(width, height) ||
        rgba.size() < std::size_t(width) * std::size_t(height) * 4) return false;
    Palette palette;
    const int nx = std::min(width, kSampleSide), ny = std::min(height, kSampleSide);
    for (int y = 0; y < ny; ++y) {
        const auto offset = std::size_t(sample_position(y, ny, height)) * std::size_t(width) * 4;
        for (int x = 0; x < nx; ++x)
            palette.add(rgba.data() + offset + sample_position(x, nx, width) * 4);
    }
    return palette.finish(color);
}

bool dominant_controller_color_file(const std::string& path, ControllerRgb& color) {
    return read_cached_palette(path, color, nullptr, 0);
}

ControllerAmbientLight::~ControllerAmbientLight() { stop(); }

void ControllerAmbientLight::cancel_job() {
    ++generation_;
    requested_generation_ = 0;
    std::lock_guard lock(mutex_);
    job_.reset(); result_.reset();
}

void ControllerAmbientLight::reset() {
    if (title_.empty() && path_.empty() && target_ == kControllerDefaultColor) return;
    title_.clear(); path_.clear();
    cancel_job();
    target_ = kControllerDefaultColor;
    selected_for_ = 0;
    reset_pending_ = true;
    apply_after_ = 0;
}

void ControllerAmbientLight::select(const std::string& title, const std::string& path) {
    if (title.empty() || title.size() > 4096) { reset(); return; }
    const bool new_title = title != title_;
    // Metadata/launch views may have no cached path for one frame. Keep the
    // selected title's palette; never retain a different title's pending job.
    if (!new_title && (path.empty() || path == path_)) return;
    title_ = title;
    path_ = path.size() <= 4096 ? path : std::string{};
    cancel_job();
    selected_for_ = 0;
    reset_pending_ = false;
    apply_after_ = 0;
    if (new_title) target_ = kControllerDefaultColor;
    if (enabled_) {
        const auto found = cache_.find(path_);
        if (found != cache_.end() && found->second.ok) {
            found->second.used = ++cache_order_;
            target_ = found->second.color;
        }
    }
}

void ControllerAmbientLight::remember(const Result& result) {
    Cached cached;
    cached.color = result.color; cached.ok = result.ok;
    cached.retry_at = elapsed_ + 5;
    cached.used = ++cache_order_;
    cache_[result.job.path] = cached;
    while (cache_.size() > kPaletteCacheEntries) {
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it)
            if (it->second.used < oldest->second.used) oldest = it;
        cache_.erase(oldest);
    }
}

void ControllerAmbientLight::receive_color() {
    std::optional<Result> ready;
    {
        std::lock_guard lock(mutex_);
        ready.swap(result_);
    }
    if (!ready || ready->job.generation != generation_.load() || ready->job.path != path_) return;
    requested_generation_ = 0;
    remember(*ready);
    if (enabled_ && ready->ok) {
        target_ = ready->color;
        apply_after_ = 0;
    }
}

void ControllerAmbientLight::request_color() {
    if (path_.empty() || selected_for_ < kSelectionDelay || requested_generation_ == generation_.load()) return;
    const auto found = cache_.find(path_);
    if (found != cache_.end()) {
        found->second.used = ++cache_order_;
        if (found->second.ok) {
            if (target_ != found->second.color) apply_after_ = 0;
            target_ = found->second.color;
            return;
        }
        if (elapsed_ < found->second.retry_at) return;
    }
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        job_ = Job{path_, generation_.load()};
        requested_generation_ = job_->generation;
    }
    if (!worker_.joinable()) worker_ = std::thread([this] { worker(); });
    cv_.notify_one();
}

void ControllerAmbientLight::update(float dt, bool enabled, const Apply& apply) {
    if (!std::isfinite(dt) || dt < 0) dt = 0;
    dt = std::min(dt, 0.25f);
    elapsed_ += dt;
    selected_for_ += dt;
    apply_after_ -= dt;
    if (enabled != enabled_) {
        enabled_ = enabled;
        cancel_job();
        target_ = kControllerDefaultColor;
        reset_pending_ = !enabled;
        apply_after_ = 0;
        if (enabled) selected_for_ = kSelectionDelay;
    }
    receive_color();
    if (enabled_ && !title_.empty()) request_color();
    if (reset_pending_ || !enabled_) {
        current_ = {float(kControllerDefaultColor.r), float(kControllerDefaultColor.g), float(kControllerDefaultColor.b)};
        reset_pending_ = false;
    } else {
        const float amount = 1 - std::exp(-dt / 0.28f);
        const std::array<float, 3> target{float(target_.r), float(target_.g), float(target_.b)};
        for (std::size_t i = 0; i < current_.size(); ++i) {
            current_[i] += (target[i] - current_[i]) * amount;
            if (std::abs(target[i] - current_[i]) < 0.35f) current_[i] = target[i];
        }
    }
    const ControllerRgb color{static_cast<std::uint8_t>(std::lround(current_[0])),
                              static_cast<std::uint8_t>(std::lround(current_[1])),
                              static_cast<std::uint8_t>(std::lround(current_[2]))};
    // Pad itself deduplicates native calls. The low-frequency heartbeat lets
    // it restore a stable color after reconnection or the PS system overlay.
    if (!apply || apply_after_ > 0) return;
    if (apply(color.r, color.g, color.b)) {
        const bool stable = applied_valid_ && applied_ == color && color == target_;
        applied_ = color; applied_valid_ = true;
        apply_after_ = stable ? 1.0f : kApplyPeriod;
    } else {
        apply_after_ = 1.0f;
        applied_valid_ = false;
    }
}

void ControllerAmbientLight::worker() {
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || job_.has_value(); });
            if (stopping_) return;
            job = std::move(*job_); job_.reset();
        }
        Result result;
        result.job = std::move(job);
        result.ok = read_cached_palette(result.job.path, result.color, &generation_, result.job.generation);
        std::lock_guard lock(mutex_);
        if (!stopping_ && generation_.load() == result.job.generation) result_ = std::move(result);
    }
}

void ControllerAmbientLight::stop() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        ++generation_;
        job_.reset(); result_.reset();
    }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
}
