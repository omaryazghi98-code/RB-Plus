// Download folder browsing and creation regressions. SPDX-License-Identifier: GPL-3.0-or-later
#include "download_directory.h"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
int checks = 0;
bool ignore_permission_change = false;
void expect(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
mode_t mode(const fs::path& path) {
    struct stat info{};
    if (::stat(path.c_str(), &info)) throw std::runtime_error("stat failed");
    return info.st_mode & 0777;
}
}

extern "C" int __real_fchmod(int, mode_t);
extern "C" int __wrap_fchmod(int fd, mode_t requested) {
    return ignore_permission_change ? 0 : __real_fchmod(fd, requested);
}

int main() {
    char temporary[] = "/tmp/stremio-directory-test-XXXXXX";
    const char* made = ::mkdtemp(temporary);
    if (!made) return 2;
    const fs::path root(made);
    try {
        using namespace download_directory;
        expect(normalize("/mnt//ext1/./Movies/") == "/mnt/ext1/Movies", "navigation normalizes separators");
        expect(normalize("/data/Stremio/../Movies") == "/data/Movies", "navigation resolves a parent");
        expect(normalize("/../data").empty(), "parent cannot escape root");
        expect(normalize("relative").empty(), "relative paths rejected");
        expect(normalize(std::string("/data\0/Movies", 13)).empty(), "embedded NUL rejected");
        expect(normalize("/data/Movies\\other").empty(), "backslash rejected consistently with writer");
        expect(normalize("/data/Movies\nother").empty(), "control characters rejected");
        expect(normalize("/" + std::string(256, 'x')).empty(), "oversized component rejected");
        expect(parent("/") == "/" && parent("/mnt") == "/", "parent navigation clamps at root");
        expect(child("/mnt/ext1", "Cinema italiano") == "/mnt/ext1/Cinema italiano", "spaces in folder names supported");
        expect(child("/mnt", "../data").empty() && child("/mnt", "..").empty(), "child cannot escape parent");

        fs::create_directory(root / "Zulu");
        fs::create_directory(root / "alpha");
        fs::create_directory(root / "Beta");
        fs::create_directory(root / ".hidden");
        std::ofstream(root / "media.part") << "retained";
        ::chmod(root.c_str(), 0750);
        const auto listing = list(root.string());
        expect(!listing.error && !listing.limited, "real folder listing succeeds");
        expect(listing.folders == std::vector<std::string>({".hidden", "alpha", "Beta", "Zulu"}), "only directories, sorted without case bias");
        expect(mode(root) == 0750, "browsing does not change folder permissions");
        expect(fs::file_size(root / "media.part") == 8, "browsing preserves existing files");
        expect(list((root / "media.part").string()).error == ENOTDIR, "files cannot be opened as folders");
        expect(list((root / "missing").string()).error == ENOENT, "unavailable folder reports an error");
        auto cancel = std::make_shared<std::atomic<bool>>(true);
        const auto cancelled = list(root.string(), cancel);
        expect(cancelled.error == ECANCELED && cancelled.folders.empty(), "cancelled scans publish no stale rows");

        const auto created = create(root.string());
        const auto target = root / folder_name;
        expect(created.created && !created.error && created.path == target.string(), "Square creates exactly the named child");
        expect(mode(target) == 0777, "new child gets the requested permissions despite umask");
        expect(mode(root) == 0750 && mode(root / "alpha") != 0777, "creation leaves parent and siblings unchanged");
        expect(fs::is_empty(target), "write probe is removed completely");
        std::ofstream(target / "existing.part") << "keep this media";
        ::chmod((target / "existing.part").c_str(), 0600);
        const auto repeated = create(root.string());
        expect(!repeated.created && !repeated.error && repeated.path == created.path, "existing named folder can be entered without replacement");
        expect(mode(target / "existing.part") == 0600 && fs::file_size(target / "existing.part") == 15,
               "reusing a folder never chmods or alters its contents");
        expect(std::distance(fs::directory_iterator(target), fs::directory_iterator{}) == 1,
               "repeated write verification leaves no hidden probe files");

        ::chmod(target.c_str(), 0750);
        ignore_permission_change = true;
        const auto unchanged_permissions = create(root.string());
        ignore_permission_change = false;
        expect(unchanged_permissions.error == EACCES && mode(target) == 0750,
               "a filesystem reporting chmod success without applying 0777 is rejected");
        expect(fs::file_size(target / "existing.part") == 15 &&
               std::distance(fs::directory_iterator(target), fs::directory_iterator{}) == 1,
               "failed permission read-back leaves existing content and no probe files");

        const auto linked_parent = root / "parent-link";
        fs::create_directory_symlink(root / "alpha", linked_parent);
        expect(create(linked_parent.string()).error != 0, "creation refuses a linked parent");
        expect(!fs::exists(root / "alpha" / folder_name), "refusing a linked parent leaves its target untouched");
        fs::create_directory_symlink(root / "Beta", root / "Zulu" / folder_name);
        expect(create((root / "Zulu").string()).error != 0, "creation refuses an existing linked child");
        expect(fs::is_empty(root / "Beta"), "linked child target receives no probe or content");
        std::ofstream(root / "Beta" / folder_name) << "unchanged";
        expect(create((root / "Beta").string()).error != 0, "existing file is never replaced by directory");
        expect(fs::file_size(root / "Beta" / folder_name) == 9, "existing filename collision preserves bytes");
        expect(error_text(ENOSPC, false).find("space") != std::string::npos, "drive-full failure is explained");
        expect(error_text(EROFS, false).find("read-only") != std::string::npos, "read-only failure is explained");
        expect(error_text(ELOOP, false).find("symbolic") != std::string::npos, "symlink failure is explained");
        fs::remove_all(root);
        std::cout << checks << " download directory checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
