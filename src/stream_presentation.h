// Stream badges extracted from provider metadata, without changing playback.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Stream;

struct StreamPresentation {
    // ISO 639-2 bibliographic aliases, matching Stremio's language helpers.
    // A provider's "multi" marker stays "mul"; it does not invent languages.
    std::vector<std::string> languages;
    int64_t seeders = -1; // Unknown is different from a reported zero.
    std::string type;    // torrent/direct/debrid/external/youtube/usenet/archive/unavailable
    std::string quality;
    std::string size;
    bool cached = false; // Only positive, explicit provider evidence.
};

// Optional/unknown addon fields are tolerated. Source URLs, tracker URLs,
// account names, subtitle tracks and country restrictions are not interpreted
// as audio languages, seed counts or cache status.
StreamPresentation stream_presentation(const Stream& stream);
