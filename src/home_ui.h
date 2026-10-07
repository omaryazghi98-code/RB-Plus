// Stremio PS5 - Native controller interface built with ps5-homebrew-ui.
// Copyright (C) 2026 LoZazaMastro and contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

class App;
namespace hui {
struct InputFrame;
namespace ui {
struct Canvas;
struct Feedback;
struct FontRef;
}
}

// Owns UI presentation and component state. App remains the authority for
// catalog requests, selections, authentication, settings and playback.
// All geometry uses the kit's 1920 x 1080 canvas. No GL resource is owned here.
class HomeUi {
public:
    using TextureLookup = std::function<std::uint32_t(const std::string& path)>;
    using TextureSizeLookup = std::function<std::pair<int, int>(const std::string& path)>;

    explicit HomeUi(TextureLookup textures = {});
    ~HomeUi();
    HomeUi(const HomeUi&) = delete;
    HomeUi& operator=(const HomeUi&) = delete;
    HomeUi(HomeUi&&) noexcept;
    HomeUi& operator=(HomeUi&&) noexcept;

    // The callback returns 0 while an asynchronous load is pending. Only
    // visible artwork is requested; eviction and uploads belong to the caller.
    void set_texture_lookup(TextureLookup textures);
    // Returns cached texture dimensions only; presentation never reads artwork
    // from disk to measure a variable-aspect title logo.
    void set_texture_size_lookup(TextureSizeLookup dimensions);
    // Optional broad Unicode atlas for captions; the caller owns its lifetime.
    // Subtitle parsing already applies FriBidi before text reaches this UI.
    void set_subtitle_font(const hui::ui::FontRef& font);
    void set_subtitle_fonts(const hui::ui::FontRef& sans, const hui::ui::FontRef& serif,
                            const hui::ui::FontRef& mono);
    void handle(App& app, const hui::InputFrame& input, hui::ui::Feedback& feedback);
    void update(const App& app, float dt);
    void draw(const App& app, hui::ui::Canvas& canvas) const;
    // Render the base first, flush it, then build a blurred texture when
    // wants_glass() is true. Pass that texture as Canvas.glass for overlays.
    // draw() remains a convenient single-pass alternative.
    void draw_base(const App& app, hui::ui::Canvas& canvas) const;
    void draw_overlays(const App& app, hui::ui::Canvas& canvas) const;
    bool wants_glass() const;

    // Settle a prepared model for a deterministic host screenshot. Does not
    // fetch catalogs, change App or stand in for a hardware playback test.
    void snap(const App& app);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
