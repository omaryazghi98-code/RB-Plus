/*
 * ps5-native-app-boilerplate - Application-side elevation API.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "protocol.hpp"

namespace elevation
{
// Call during single-threaded startup, before creating workers or opening files
// outside the sandbox. Only Status::ok permits continuing with elevated work.
// Each call streams the bundled, exact-title Lapy helper to the local elfldr.
[[nodiscard]] Status request(Capability capability,
                             const char *helper_path = "/app0/lapy.elf") noexcept;
// Fixed diagnostic for the latest helper-open attempt; no filesystem contents.
[[nodiscard]] const char *helper_open_diagnostic() noexcept;
} // namespace elevation
