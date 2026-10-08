// Native directory record parsing, using the host ABI as a bounded fixture.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "download_directory.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
int checks = 0, calls = 0, scenario = 0;
bool sized = true;
void expect(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
}

extern "C" int sceKernelGetdents(int, char* buffer, int capacity) {
    sized = sized && capacity == 65536;
    if (calls++) return 0;
    if (scenario == 1) return capacity + 1;
    if (scenario == 2) return static_cast<int>(offsetof(dirent, d_name));
    if (scenario == 5) return static_cast<int>(0x8002000du);
    if (scenario == 6) { errno = ENODEV; return -1; }
    dirent record{};
    record.d_reclen = static_cast<decltype(record.d_reclen)>(offsetof(dirent, d_name) + 8);
    std::memcpy(record.d_name, "Movies", 7);
    if (scenario == 3) record.d_reclen = 0;
    if (scenario == 4) std::memset(record.d_name, 'x', sizeof(record.d_name));
    std::memcpy(buffer, &record, sizeof(record));
    return static_cast<int>(offsetof(dirent, d_name) + 8);
}

int main() {
    char temporary[] = "/tmp/stremio-native-directory-test-XXXXXX";
    const char* made = ::mkdtemp(temporary);
    if (!made) return 2;
    const std::filesystem::path root(made);
    try {
        std::filesystem::create_directory(root / "Movies");
        const auto valid = download_directory::list(root.string());
        expect(sized, "native mounted volume requests a 64 KiB buffer");
        expect(!valid.error && valid.folders == std::vector<std::string>{"Movies"}, "valid bounded records reach the listing");
        for (scenario = 1; scenario <= 4; ++scenario) {
            calls = 0;
            const auto broken = download_directory::list(root.string());
            expect(broken.error == EIO && broken.folders.empty(), "malformed native records fail without publishing entries");
        }
        scenario = 5; calls = 0;
        expect(download_directory::list(root.string()).error == EACCES, "kernel error value translates to POSIX error");
        scenario = 6; calls = 0;
        expect(download_directory::list(root.string()).error == ENODEV, "POSIX error preserves a disconnected device failure");
        expect(download_directory::create("/").error == EINVAL, "native root cannot become a download destination");
        expect(download_directory::create("/mnt").error == EINVAL, "mount container is not writable as a destination");
        expect(download_directory::create(root.string()).error == EINVAL, "native writes are limited to data and mounted drives");
        std::filesystem::remove_all(root);
        std::cout << checks << " native directory parser checks passed (host ABI fixture)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
