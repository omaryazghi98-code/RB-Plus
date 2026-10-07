// Stremio - Artwork-derived controller light, with bounded background sampling.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

struct ControllerRgb {
    std::uint8_t r = 0, g = 0, b = 0;
    bool operator==(const ControllerRgb&) const = default;
};

inline constexpr ControllerRgb kControllerDefaultColor{134, 90, 247};

// The same palette algorithm is used for in-memory RGBA and existing ArtCache
// files. Samples at most 64 x 64 pixels, ignores transparent borders and avoids
// driving an LED at a cover's full white/black extrema. No HTTP or GPU access.
bool dominant_controller_color(std::span<const unsigned char> rgba, int width, int height,
                               ControllerRgb& color);
bool dominant_controller_color_file(const std::string& cached_rgba_path, ControllerRgb& color);

// Own on the UI thread, alongside the existing Pad. select() and update() never
// read artwork on that thread; the one worker only samples a cached local file.
// The apply callback runs exclusively inside update(), on its caller's thread.
class ControllerAmbientLight {
public:
    using Apply = std::function<bool(std::uint8_t, std::uint8_t, std::uint8_t)>;

    ControllerAmbientLight() = default;
    ~ControllerAmbientLight();
    ControllerAmbientLight(const ControllerAmbientLight&) = delete;
    ControllerAmbientLight& operator=(const ControllerAmbientLight&) = delete;

    // Empty title resets to the default. A temporarily empty path for the same
    // title keeps its existing artwork/color through detail, loading and video.
    void select(const std::string& title_key, const std::string& cached_rgba_path);
    void reset();
    void update(float dt, bool enabled, const Apply& apply);
    void stop();

private:
    struct Job {
        std::string path;
        std::uint64_t generation = 0;
    };
    struct Result {
        Job job;
        ControllerRgb color;
        bool ok = false;
    };
    struct Cached {
        ControllerRgb color;
        bool ok = false;
        double retry_at = 0;
        std::uint64_t used = 0;
    };

    void cancel_job();
    void request_color();
    void receive_color();
    void worker();
    void remember(const Result& result);

    std::string title_, path_;
    std::array<float, 3> current_{134, 90, 247};
    ControllerRgb target_ = kControllerDefaultColor;
    ControllerRgb applied_ = kControllerDefaultColor;
    bool enabled_ = true, applied_valid_ = false, reset_pending_ = true;
    double elapsed_ = 0;
    float selected_for_ = 0, apply_after_ = 0;
    std::uint64_t requested_generation_ = 0, cache_order_ = 0;
    std::map<std::string, Cached> cache_;

    std::atomic<std::uint64_t> generation_{1};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Job> job_;
    std::optional<Result> result_;
    std::thread worker_;
    bool stopping_ = false;
};
