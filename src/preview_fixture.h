// Host-only presentation fixtures. Never linked into the native PS5 title.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
class App;

// Populate public UI data in an App that was initialized in offline mode.
// This does not install add-ons, authenticate, fetch media or start playback.
// Images are optional local paths, relative to the JSON file. A missing image
// uses the supplied app art. The player scenario models its overlay only.
bool load_preview_fixture(App& app, const std::string& scenario, const std::string& fixture_path);
