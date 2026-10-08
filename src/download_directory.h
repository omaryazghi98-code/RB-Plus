// Download directory browsing and creation. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace download_directory {

inline constexpr const char* folder_name = "Stremio Plus Downloads";

struct Listing {
    std::string path;
    std::vector<std::string> folders;
    int error = 0;
    bool limited = false;
};

struct Creation {
    std::string path;
    int error = 0;
    bool created = false;
};

// Navigation is read-only. DownloadManager separately validates a writable
// destination before changing where new downloads are stored.
std::string normalize(const std::string& path);
std::string parent(const std::string& path);
std::string child(const std::string& path, const std::string& name);
Listing list(const std::string& path,
             const std::shared_ptr<std::atomic<bool>>& cancel = {});
Creation create(const std::string& parent);
std::string error_text(int error, bool italian);

} // namespace download_directory
