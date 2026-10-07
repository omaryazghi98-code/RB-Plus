// Stremio - GPU video conversion on the same OpenGL context as the UI.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "player.h"
#include <cstdint>

class VideoGl {
public:
    ~VideoGl();
    bool init();
    bool upload(const Player::VideoFrame& frame);
    void clear();
    void release();
    uint32_t texture() const { return valid_ ? output_ : 0; }
    double aspect() const { return aspect_; }
private:
    uint32_t program_ = 0, vao_ = 0, framebuffer_ = 0, output_ = 0, planes_[3]{};
    int width_ = 0, height_ = 0, format_ = -1;
    bool valid_ = false;
    double aspect_ = 16.0 / 9.0;
};
