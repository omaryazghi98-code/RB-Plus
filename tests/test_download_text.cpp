// Runtime download telemetry: unknown values, conservative time rounding and
// localization must remain truthful at transfer boundaries.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "download_text.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

int main() {
    int checks = 0;
    int failures = 0;
    const auto check = [&](bool ok, const char* reason) {
        ++checks;
        if (!ok) { ++failures; std::cerr << "FAIL: " << reason << '\n'; }
    };

    check(download_remaining_text(-1, true) == "Tempo rimanente: in stima…",
          "unknown ETA is explicitly pending in Italian");
    check(download_remaining_text(-1, false) == "Time remaining: estimating…",
          "unknown ETA is explicitly pending in English");
    check(download_remaining_text(std::numeric_limits<std::int64_t>::min(), true).find("in stima") != std::string::npos,
          "all negative estimates stay unknown");
    check(download_remaining_text(0, true) == "Tempo rimanente: meno di 1 min" &&
          download_remaining_text(59, false) == "Time remaining: less than 1 min",
          "finishing transfers never display a misleading zero-minute promise");
    check(download_remaining_text(60, true) == "Tempo rimanente: ~1 min" &&
          download_remaining_text(61, true) == "Tempo rimanente: ~2 min",
          "partial minutes are rounded up");
    check(download_remaining_text(3599, false) == "Time remaining: ~1 h" &&
          download_remaining_text(3601, false) == "Time remaining: ~1 h 1 min",
          "hour boundary carries rounding correctly");
    check(download_remaining_text(86399, true) == "Tempo rimanente: ~1 g" &&
          download_remaining_text(86401, false) == "Time remaining: ~1 d 1 h",
          "day estimates use readable conservative hour precision");
    check(download_remaining_text(std::numeric_limits<std::int64_t>::max(), true) ==
          "Tempo rimanente: ~106751991167300 g 16 h",
          "maximum signed estimate formats without overflowing or wrapping");

    check(download_connections_text(-1, -1, true).empty() &&
          download_connections_text(-1, -1, false).empty(),
          "HTTP and unknown counters do not invent a torrent label");
    check(download_connections_text(0, 0, true) == "Peer connessi: 0 · seeder: 0",
          "known zero connections remain visible");
    check(download_connections_text(18, 7, false) == "Connected peers: 18 · seeders: 7",
          "connected seeders remain explicitly a part of peer telemetry");
    check(download_connections_text(18, -1, true) == "Peer connessi: 18",
          "unknown seeders never become zero or a misleading total");
    check(download_connections_text(-1, 7, true) == "Seeder connessi: 7" &&
          download_connections_text(-1, 0, false) == "Connected seeders: 0",
          "independently known seeder counts survive missing peer totals");
    check(download_error_text("Download source returned HTTP 429", true) == "La sorgente ha risposto con HTTP 429.",
          "existing safe diagnostics keep their localization");

    if (failures) return 1;
    std::cout << "PASS: " << checks << " download telemetry formatting checks (unknown values, rounding, overflow, IT/EN)\n";
    return 0;
}
