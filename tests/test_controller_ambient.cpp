// Stremio - Dominant palette and asynchronous controller-color regressions.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "controller_ambient.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace {
int checks = 0;
const auto ui_thread = std::this_thread::get_id();
std::atomic<int> opens{0}, ui_reads{0};
std::atomic<std::size_t> bytes_read{0}, largest_read{0};
std::atomic<bool> expect_worker{false};
std::mutex gate_mutex;
std::condition_variable gate_cv;
std::string held_path;
bool gate_entered = false, gate_released = true;

void expect(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

void release_gate() {
    {
        std::lock_guard lock(gate_mutex);
        gate_released = true;
    }
    gate_cv.notify_all();
}

struct GateRelease { ~GateRelease() { release_gate(); } };

void hold_read(const std::string& path) {
    std::lock_guard lock(gate_mutex);
    held_path = path; gate_entered = false; gate_released = false;
}

bool entered_read() {
    std::unique_lock lock(gate_mutex);
    return gate_cv.wait_for(lock, 1s, [] { return gate_entered; });
}

std::vector<unsigned char> pixels(int w, int h, ControllerRgb color, unsigned char alpha = 255) {
    std::vector<unsigned char> out(std::size_t(w) * std::size_t(h) * 4);
    for (std::size_t i = 0; i < out.size(); i += 4) {
        out[i] = color.r; out[i + 1] = color.g; out[i + 2] = color.b; out[i + 3] = alpha;
    }
    return out;
}

void write_rgba(const std::string& path, int w, int h, const std::vector<unsigned char>& rgba) {
    auto* file = std::fopen(path.c_str(), "wb");
    if (!file) throw std::runtime_error("cannot create fixture");
    unsigned char header[12] = {'R', 'G', 'B', 'A'};
    for (int i = 0; i < 4; ++i) {
        header[4 + i] = static_cast<unsigned char>(std::uint32_t(w) >> (8 * i));
        header[8 + i] = static_cast<unsigned char>(std::uint32_t(h) >> (8 * i));
    }
    std::fwrite(header, 1, 12, file);
    if (!rgba.empty()) std::fwrite(rgba.data(), 1, rgba.size(), file);
    std::fclose(file);
}

ControllerRgb palette(ControllerRgb rgb) {
    const auto image = pixels(16, 16, rgb);
    ControllerRgb result;
    if (!dominant_controller_color(image, 16, 16, result)) throw std::runtime_error("palette fixture failed");
    return result;
}

void pure_palette(const std::string& dir) {
    ControllerRgb color;
    const auto red = palette({240, 22, 12});
    expect(red.r > red.g * 3 && red.r > red.b * 3, "a red cover keeps a red hue");
    expect(red.r >= 81 && red.r <= 184 && red.g > 0, "LED intensity and saturation have usable bounds");
    const auto blue = palette({12, 30, 245});
    expect(blue.b > blue.r * 3 && blue.b > blue.g * 3, "a blue cover keeps a blue hue");
    auto split = pixels(100, 60, {240, 20, 10});
    for (int y = 0; y < 60; ++y) for (int x = 75; x < 100; ++x) {
        auto* p = split.data() + (y * 100 + x) * 4;
        p[0] = 10; p[1] = 30; p[2] = 240;
    }
    expect(dominant_controller_color(split, 100, 60, color) && color.r > color.b * 3,
           "population wins instead of averaging red and blue into purple");
    auto transparent = pixels(100, 100, {255, 255, 255}, 0);
    for (int y = 25; y < 75; ++y) for (int x = 25; x < 75; ++x) {
        auto* p = transparent.data() + (y * 100 + x) * 4;
        p[0] = 5; p[1] = 235; p[2] = 20; p[3] = 255;
    }
    expect(dominant_controller_color(transparent, 100, 100, color) && color.g > color.r * 3,
           "transparent white borders do not pollute the artwork palette");
    const auto clear = pixels(64, 64, {255, 30, 200}, 0);
    expect(!dominant_controller_color(clear, 64, 64, color), "fully transparent artwork has no claimed color");
    const auto white = palette({255, 255, 255}), black = palette({0, 0, 0});
    expect(white.r == white.g && white.g == white.b && white.r <= 150, "white artwork produces a restrained neutral light");
    expect(black.r == black.g && black.r == black.b && black.r >= 64, "black artwork remains neutral without switching the light off");
    const auto gray = palette({130, 130, 130});
    expect(gray.r == gray.g && gray.g == gray.b, "monochrome artwork is not assigned an invented hue");
    expect(!dominant_controller_color({}, 4, 4, color), "truncated memory input is rejected");
    expect(!dominant_controller_color(split, 0, 10, color), "zero image extent is rejected");
    expect(!dominant_controller_color(split, -1, 10, color), "negative image extent is rejected");
    expect(!dominant_controller_color(split, 8193, 1, color), "unbounded row width is rejected");

    const auto path = dir + "/same.rgba";
    write_rgba(path, 100, 60, split);
    ControllerRgb file_color, memory_color;
    expect(dominant_controller_color_file(path, file_color) && dominant_controller_color(split, 100, 60, memory_color) &&
               file_color == memory_color, "cached-file and in-memory palette paths agree");
    expect(!dominant_controller_color_file(dir + "/missing.rgba", color), "missing cache file safely returns no palette");
    expect(!dominant_controller_color_file(dir + "/wrong.jpg", color), "the worker cannot decode or download an arbitrary image format");
    expect(!dominant_controller_color_file(std::string(4097, 'x') + ".rgba", color), "oversized paths are rejected");
    write_rgba(dir + "/broken.rgba", 20, 20, pixels(1, 1, {1, 2, 3}));
    expect(!dominant_controller_color_file(dir + "/broken.rgba", color), "partial cache writes are rejected");
    write_rgba(dir + "/too-big.rgba", 8192, 8192, {});
    expect(!dominant_controller_color_file(dir + "/too-big.rgba", color), "cache dimensions cannot request an excessive image");
    write_rgba(dir + "/trailing.rgba", 1, 1, pixels(2, 1, {1, 2, 3}));
    expect(!dominant_controller_color_file(dir + "/trailing.rgba", color), "unexpected trailing cache data is rejected");

    // Sparse RGBA proves the upper read bound without a 64 MiB allocation.
    const auto large = dir + "/large.rgba";
    write_rgba(large, 8192, 2048, {});
    auto* file = std::fopen(large.c_str(), "r+b");
    std::fseek(file, 12 + 8192l * 2048l * 4 - 1, SEEK_SET);
    std::fputc(0, file); std::fclose(file);
    bytes_read = 0; largest_read = 0;
    expect(!dominant_controller_color_file(large, color), "fully transparent sparse cache remains empty");
    expect(largest_read <= 32768 && bytes_read <= 12 + 64 * 32768,
           "even a valid 16-megapixel cache reads at most 64 rows with a 32 KiB row buffer");
}

struct Sink {
    ControllerRgb last = kControllerDefaultColor;
    int calls = 0;
    bool wrong_thread = false, accept = true;
    std::vector<ControllerRgb> history;
    ControllerAmbientLight::Apply callback() {
        return [this](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            wrong_thread = wrong_thread || std::this_thread::get_id() != ui_thread;
            last = {r, g, b}; history.push_back(last); ++calls;
            return accept;
        };
    }
};

bool settle(ControllerAmbientLight& ambient, Sink& sink, ControllerRgb expected) {
    const auto apply = sink.callback();
    for (int frame = 0; frame < 240; ++frame) {
        ambient.update(1.0f / 30, true, apply);
        if (sink.last == expected) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

void background_and_lifecycle(const std::string& dir) {
    const auto red_path = dir + "/red.rgba", green_path = dir + "/green.rgba", blue_path = dir + "/blue.rgba";
    write_rgba(red_path, 64, 96, pixels(64, 96, {240, 22, 12}));
    write_rgba(green_path, 64, 96, pixels(64, 96, {10, 235, 40}));
    write_rgba(blue_path, 64, 96, pixels(64, 96, {12, 30, 245}));
    const auto red = palette({240, 22, 12}), green = palette({10, 235, 40});
    expect_worker = true; ui_reads = 0;
    {
        ControllerAmbientLight ambient;
        Sink sink;
        const auto apply = sink.callback();
        const int before = opens;
        ambient.select("movie:red", red_path);
        ambient.update(0.10f, true, apply);
        ambient.update(0.10f, true, apply);
        expect(opens == before, "rapid browsing does not read a cover before the focus debounce");
        expect(settle(ambient, sink, red), "worker result fades to the selected cover's dominant color");
        expect(opens == before + 1, "one selected cached artwork is read once");
        bool intermediate = false;
        for (const auto c : sink.history) if (c != red && c != kControllerDefaultColor) intermediate = true;
        expect(intermediate, "color transitions interpolate instead of jumping directly to the palette");
        for (int i = 0; i < 1000; ++i) { ambient.select("movie:red", red_path); ambient.update(0.01f, true, apply); }
        expect(opens == before + 1, "repeated per-frame selection and stable heartbeat do not reread the image");
        ambient.select("movie:red", {});
        for (int i = 0; i < 120; ++i) ambient.update(1.0f / 60, true, apply);
        expect(sink.last == red && opens == before + 1, "detail-loading-playback path gaps preserve the same title's color");
        ambient.update(0, false, apply);
        expect(sink.last == kControllerDefaultColor, "disabling ambient light immediately restores the app's default color");
        ambient.select("movie:green", green_path);
        for (int i = 0; i < 100; ++i) ambient.update(0.1f, false, apply);
        expect(opens == before + 1, "disabled mode performs no new artwork reads");
        expect(settle(ambient, sink, green), "reenabling samples the currently selected title");
        ambient.select("movie:red", red_path);
        expect(settle(ambient, sink, red) && opens == before + 2, "revisiting a title reuses its cached palette");
        ambient.select({}, {}); ambient.update(0, true, apply);
        expect(sink.last == kControllerDefaultColor, "logout or no selection resets the color immediately");
        expect(!sink.wrong_thread, "every controller write callback executes on the UI thread");
    }
    {
        ControllerAmbientLight ambient;
        GateRelease unblock;
        Sink sink;
        const auto apply = sink.callback();
        const int before = opens;
        hold_read(red_path);
        ambient.select("movie:red", red_path); ambient.update(0.25f, true, apply);
        expect(entered_read(), "the controlled first file job starts on its worker");
        ambient.select("movie:blue", blue_path); ambient.update(0.25f, true, apply);
        ambient.select("movie:green", green_path); ambient.update(0.25f, true, apply);
        release_gate();
        expect(settle(ambient, sink, green), "latest selection survives a delayed previous result");
        expect(opens == before + 2, "only the running job and the newest pending file are read");
        expect(std::find(sink.history.begin(), sink.history.end(), red) == sink.history.end(),
               "an old title's finished palette cannot recolor the new title");
    }
    {
        ControllerAmbientLight ambient;
        GateRelease unblock;
        Sink sink;
        const auto apply = sink.callback();
        hold_read(red_path);
        ambient.select("movie:red", red_path); ambient.update(0.25f, true, apply);
        expect(entered_read(), "logout cancellation fixture starts a background read");
        ambient.reset(); ambient.update(0, true, apply); release_gate();
        for (int i = 0; i < 60; ++i) { ambient.update(0.05f, true, apply); std::this_thread::sleep_for(1ms); }
        expect(sink.last == kControllerDefaultColor &&
                   std::find(sink.history.begin(), sink.history.end(), red) == sink.history.end(),
               "logout invalidates an in-flight result and keeps the default light");
    }
    {
        ControllerAmbientLight ambient;
        Sink sink;
        sink.accept = false;
        const auto apply = sink.callback();
        for (int i = 0; i < 300; ++i) ambient.update(1.0f / 60, true, apply);
        expect(sink.calls <= 6, "a rejected light API is retried at most once per second by the adapter");
        const int before = sink.calls;
        ambient.update(std::numeric_limits<float>::quiet_NaN(), true, apply);
        ambient.update(std::numeric_limits<float>::infinity(), true, apply);
        ambient.update(-10, true, apply);
        expect(sink.calls == before, "invalid frame time cannot produce uncontrolled writes");
    }
    {
        ControllerAmbientLight ambient;
        GateRelease unblock;
        Sink sink;
        const auto apply = sink.callback();
        const auto missing = dir + "/later.rgba";
        const int before = opens;
        hold_read(missing);
        ambient.select("movie:later", missing); ambient.update(0.25f, true, apply);
        expect(entered_read(), "a temporarily unavailable cache is checked on the worker");
        release_gate();
        for (int i = 0; i < 300; ++i) {
            ambient.update(0.01f, true, apply);
            std::this_thread::sleep_for(1ms);
        }
        expect(opens == before + 1 && sink.last == kControllerDefaultColor,
               "a failed file read keeps the default and cannot trigger a per-frame retry storm");
        write_rgba(missing, 16, 16, pixels(16, 16, {240, 22, 12}));
        for (int i = 0; i < 30; ++i) {
            ambient.update(0.1f, true, apply);
            std::this_thread::sleep_for(1ms);
        }
        expect(settle(ambient, sink, red) && opens == before + 2,
               "the same selected title retries an available cache after the failure backoff");
    }
    expect(ui_reads == 0, "selection and animation never perform file reads on the UI thread");
    expect_worker = false;
}
} // namespace

extern "C" std::FILE* __real_fopen(const char*, const char*);
extern "C" std::size_t __real_fread(void*, std::size_t, std::size_t, std::FILE*);

extern "C" std::FILE* __wrap_fopen(const char* path, const char* mode) {
    if (mode && mode[0] == 'r') {
        ++opens;
        if (expect_worker && std::this_thread::get_id() == ui_thread) ++ui_reads;
        std::unique_lock lock(gate_mutex);
        if (path && path == held_path && !gate_released) {
            gate_entered = true; gate_cv.notify_all();
            gate_cv.wait(lock, [] { return gate_released; });
        }
    }
    return __real_fopen(path, mode);
}

extern "C" std::size_t __wrap_fread(void* buffer, std::size_t size, std::size_t count, std::FILE* file) {
    const auto got = __real_fread(buffer, size, count, file);
    const auto bytes = got * size;
    bytes_read += bytes;
    auto largest = largest_read.load();
    while (bytes > largest && !largest_read.compare_exchange_weak(largest, bytes)) {}
    return got;
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("fixture directory argument missing");
        std::filesystem::create_directories(argv[1]);
        pure_palette(argv[1]);
        background_and_lifecycle(argv[1]);
        std::cout << "PASS " << checks << " controller palette/lifecycle checks; no network, no UI-thread artwork reads.\n";
        return 0;
    } catch (const std::exception& error) {
        release_gate();
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
