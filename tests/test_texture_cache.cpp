#include "texture_cache.h"
#include "gfx/gl_batch.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace {
int checks = 0;
std::mutex image_mutex;
std::map<std::string, int> image_reads;
bool recover_image = false;
bool decode_on_ui = false, upload_off_ui = false, delete_off_ui = false;
const std::thread::id ui_thread = std::this_thread::get_id();
uint32_t next_texture = 1;
size_t created = 0, deleted = 0;
std::map<uint32_t, size_t> live_gpu;
void expect(bool value, const char* description) {
    ++checks;
    if (!value) throw std::runtime_error(description);
}
template<typename Predicate> void until(Predicate done, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        if (done()) return;
        std::this_thread::sleep_for(3ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(message);
}
int reads(const std::string& fragment) {
    std::lock_guard<std::mutex> lock(image_mutex);
    int count = 0;
    for (const auto& entry : image_reads) if (entry.first.find(fragment) != std::string::npos) count += entry.second;
    return count;
}
void asynchronous_and_retry() {
    hui::gfx::GlBatch batch;
    TextureCache cache(batch, "/art");
    expect(cache.get("https://untrusted.example/image.png") == 0 && reads("untrusted") == 0,
           "GPU artwork loader never performs URL requests");
    expect(cache.dimensions("missing.rgba") == std::pair<int, int>{0, 0} && reads("missing") == 0,
           "dimension lookup does not schedule work for unavailable textures");
    expect(cache.get("poster.rgba") == 0, "first uncached texture request returns without decoding on the UI thread");
    uint32_t texture = 0;
    until([&] { cache.begin_frame(); texture = cache.get("poster.rgba"); return texture != 0; }, "poster never became a texture");
    expect(reads("poster.rgba") == 1, "a pending/cached image is decoded once");
    expect(cache.get("poster.rgba") == texture, "cached texture handle remains stable");
    expect(cache.dimensions("poster.rgba") == std::pair<int, int>{512, 256} &&
           cache.dimensions("/art/poster.rgba") == std::pair<int, int>{512, 256},
           "dimension lookup exposes actual loaded size with the same relative/absolute cache key");
    expect(!decode_on_ui && !upload_off_ui, "CPU loads are worker-only and GPU uploads are UI-only");
    cache.get("recover.rgba");
    until([&] { cache.begin_frame(); return reads("recover.rgba") == 1 && !cache.pending(); }, "failed image did not settle");
    for (int i = 0; i < 20; ++i) { cache.get("recover.rgba"); cache.begin_frame(); }
    expect(reads("recover.rgba") == 1, "failed local file is not hammered on every frame");
    { std::lock_guard<std::mutex> lock(image_mutex); recover_image = true; }
    for (int i = 0; i < 120; ++i) cache.begin_frame();
    until([&] { cache.begin_frame(); return cache.get("recover.rgba") != 0; }, "fixed local image remained permanently blacklisted");
    expect(reads("recover.rgba") == 2, "local load failure retries after cooldown");
    cache.release();
    expect(cache.dimensions("poster.rgba") == std::pair<int, int>{0, 0},
           "release clears dimensions along with the GPU texture");
    expect(live_gpu.empty() && !delete_off_ui, "release deletes every GPU texture on its owner thread");
}
void upload_budget_and_backpressure() {
    hui::gfx::GlBatch batch;
    TextureCache cache(batch, "/art");
    cache.get("large-first-b.rgba");
    cache.get("large-second-b.rgba");
    until([&] { return reads("large-") == 2; }, "large image workers did not decode");
    bool first = false, second = false;
    size_t maximum_uploads = 0;
    until([&] {
        const auto before = created;
        cache.begin_frame();
        maximum_uploads = std::max(maximum_uploads, created - before);
        first = cache.get("large-first-b.rgba") != 0;
        second = cache.get("large-second-b.rgba") != 0;
        return first && second;
    }, "large images did not finish uploading");
    expect(maximum_uploads == 1, "two 8MiB hero images upload on separate frames");
    cache.release();

    TextureCache rows(batch, "/art");
    for (int i = 0; i < 20; ++i) rows.get("row-" + std::to_string(i) + ".rgba");
    until([&] { return reads("row-") >= 8; }, "row worker pool did not reach backpressure");
    std::this_thread::sleep_for(40ms);
    expect(reads("row-") <= 8, "decoded CPU queue is bounded at six ready plus two active images");
    maximum_uploads = 0;
    until([&] {
        const auto before = created;
        rows.begin_frame();
        maximum_uploads = std::max(maximum_uploads, created - before);
        return !rows.pending();
    }, "row uploads did not drain");
    expect(maximum_uploads <= 2 && maximum_uploads > 0, "a completed poster row respects the two-texture frame limit");
    rows.release();
    expect(live_gpu.empty(), "backpressure/release leaves no live GPU handles");
}
void eviction_and_stale_work() {
    hui::gfx::GlBatch batch;
    TextureCache cache(batch, "/art", 1u << 20);
    for (int i = 0; i < 10; ++i) cache.get("eviction-" + std::to_string(i) + ".rgba");
    until([&] { cache.begin_frame(); return !cache.pending(); }, "eviction fixture did not finish");
    for (int i = 0; i < 4; ++i) cache.begin_frame();
    size_t bytes = 0;
    for (const auto& image : live_gpu) bytes += image.second;
    expect(bytes <= (1u << 20) && deleted > 0, "unused textures are evicted to the GPU budget after the short reuse window");
    cache.release();
    expect(live_gpu.empty(), "eviction and explicit release do not double-own GPU resources");
}
} // namespace

bool load_image_rgba(const std::string& path, std::vector<unsigned char>& pixels, int& w, int& h) {
    {
        std::lock_guard<std::mutex> lock(image_mutex);
        ++image_reads[path];
        if (std::this_thread::get_id() == ui_thread) decode_on_ui = true;
        if (path.find("recover") != std::string::npos && !recover_image) { w = h = 0; pixels.clear(); return false; }
    }
    if (path.find("large-") != std::string::npos) { w = 2048; h = 1024; }
    else { w = 512; h = 256; }
    pixels.assign(size_t(w) * h * 4, 200);
    return true;
}
namespace hui::gfx {
GlBatch::~GlBatch() = default;
uint32_t GlBatch::create_texture(int w, int h, const uint8_t*) {
    if (std::this_thread::get_id() != ui_thread) upload_off_ui = true;
    const auto handle = next_texture++;
    live_gpu[handle] = size_t(w) * h * 4;
    ++created;
    return handle;
}
} // namespace hui::gfx
extern "C" void glDeleteTextures(GLsizei count, const GLuint* textures) {
    if (std::this_thread::get_id() != ui_thread) delete_off_ui = true;
    for (int i = 0; i < count; ++i) { live_gpu.erase(textures[i]); ++deleted; }
}

int main() {
    try {
        asynchronous_and_retry(); upload_budget_and_backpressure(); eviction_and_stale_work();
        std::cout << "PASS " << checks << " texture cache checks (actual scheduler; fake disk pixels/GPU handles).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
