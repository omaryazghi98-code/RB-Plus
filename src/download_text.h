// Presentation text for safe download diagnostics.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

// The queue retains the original safe English diagnostic for logs/persistence.
// Unknown reasons receive a short Italian retry message in the interface.
std::string download_error_text(const std::string& error, bool italian);

// Runtime telemetry is formatted from the worker's cached snapshot only.
// A negative ETA means that a useful estimate is not available yet. The caller
// shows these labels only for active transfers, never for completed files.
std::string download_remaining_text(std::int64_t seconds, bool italian);
// Counts include only connected torrent peers; seeders are a subset of peers.
// Negative counts are unknown/not applicable and must never become zero.
std::string download_connections_text(int peers, int seeders, bool italian);
