// Stremio Plus native startup storage. SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_storage.h"
#include "filesystem/elevation.hpp"
#include "download_writer/client.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <initializer_list>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int sceKernelDebugOutText(int channel, const char* text);

namespace {
constexpr char kLogDirectory[] = "/data/Stremio";
constexpr char kSandboxApp[] = "/mnt/sandbox/PPSA74126_000/app0";
constexpr char kSandboxData[] = "/mnt/sandbox/PPSA74126_000/download0/stremio";
constexpr char kInstalledApp[] = "/data/homebrew/PPSA74126";
int boot_descriptor = -1;

struct StorageProbe {
    const char* stage = "ready";
    int error = 0;

    bool fail(const char* operation, int code = errno) noexcept {
        stage = operation;
        error = code != 0 ? code : EIO;
        return false;
    }
};

bool is_directory(const char* path) noexcept {
    struct stat info{};
    return ::stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

bool has_app_files(const char* path) noexcept {
    char probe[192];
    std::snprintf(probe, sizeof(probe), "%s/hui/fonts/inter-regular.huifont", path);
    struct stat info{};
    return ::stat(probe, &info) == 0 && S_ISREG(info.st_mode);
}

bool prepare_log_directory() noexcept {
    // Never create a different /data inside a sandbox. The console must make
    // its existing /data directory accessible first.
    if (!is_directory("/data")) return false;
    if (::mkdir(kLogDirectory, 0700) != 0 && errno != EEXIST) return false;
    return is_directory(kLogDirectory);
}

// A loader can expose /data and allow stat/mkdir/open while the native title
// still cannot lstat or enumerate it. A writable log is therefore not proof
// that the download inventory can be used. Exercise the operations needed by
// the private download folders before starting any application worker.
bool probe_storage(StorageProbe& result) noexcept {
    struct stat info{};
    if (::lstat("/data", &info) != 0) return result.fail("lstat /data");
    if (!S_ISDIR(info.st_mode)) return result.fail("directory /data", ENOTDIR);
    if (::mkdir(kLogDirectory, 0700) != 0 && errno != EEXIST)
        return result.fail("mkdir /data/Stremio");
    if (::lstat(kLogDirectory, &info) != 0) return result.fail("lstat /data/Stremio");
    if (!S_ISDIR(info.st_mode)) return result.fail("directory /data/Stremio", ENOTDIR);
    for (const char* directory : {"/data", kLogDirectory}) {
        DIR* entries = ::opendir(directory);
        if (!entries) return result.fail(directory == kLogDirectory ? "opendir /data/Stremio" : "opendir /data");
        errno = 0;
        const auto entry = ::readdir(entries);
        const int read_error = entry == nullptr ? errno : 0;
        const int closed = ::closedir(entries);
        if (read_error) return result.fail("readdir storage", read_error);
        if (closed != 0) return result.fail("closedir storage");
    }

    // Never reuse a probe left by an interrupted launch. Only the directory
    // created by this invocation is removed, and the probe contains no user data.
    char directory[128], temporary[160], complete[160];
    bool created = false;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        std::snprintf(directory, sizeof(directory), "%s/.storage-probe-%ld-%u",
            kLogDirectory, static_cast<long>(::getpid()), attempt);
        if (::mkdir(directory, 0700) == 0) { created = true; break; }
        if (errno != EEXIST) return result.fail("mkdir storage probe");
    }
    if (!created) return result.fail("mkdir storage probe", EEXIST);
    std::snprintf(temporary, sizeof(temporary), "%s/write.tmp", directory);
    std::snprintf(complete, sizeof(complete), "%s/read.bin", directory);
    int descriptor = -1;
    bool renamed = false;
    bool ok = [&]() noexcept {
        if (::lstat(directory, &info) != 0) return result.fail("lstat storage probe");
        if (!S_ISDIR(info.st_mode)) return result.fail("directory storage probe", ENOTDIR);
        if (::chmod(directory, 0700) != 0) return result.fail("chmod storage probe");
        descriptor = ::open(temporary, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (descriptor < 0) return result.fail("open storage probe");
        if (::fchmod(descriptor, 0600) != 0) return result.fail("fchmod storage probe");
        if (::fstat(descriptor, &info) != 0) return result.fail("fstat storage probe");
        if (!S_ISREG(info.st_mode)) return result.fail("regular storage probe", EINVAL);
        constexpr char token[] = "Stremio Plus storage proof\n";
        std::size_t offset = 0;
        while (offset < sizeof(token)) {
            const auto count = ::write(descriptor, token + offset, sizeof(token) - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return result.fail("write storage probe", count < 0 ? errno : EIO);
            offset += static_cast<std::size_t>(count);
        }
        if (::fsync(descriptor) != 0) return result.fail("fsync storage probe");
        if (::lseek(descriptor, 0, SEEK_SET) != 0) return result.fail("seek storage probe");
        char actual[sizeof(token)];
        offset = 0;
        while (offset < sizeof(actual)) {
            const auto count = ::read(descriptor, actual + offset, sizeof(actual) - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return result.fail("read storage probe", count < 0 ? errno : EIO);
            offset += static_cast<std::size_t>(count);
        }
        if (std::memcmp(token, actual, sizeof(token)) != 0)
            return result.fail("compare storage probe", EIO);
        const auto closed = ::close(descriptor);
        descriptor = -1;
        if (closed != 0) return result.fail("close storage probe");
        if (::rename(temporary, complete) != 0) return result.fail("rename storage probe");
        renamed = true;
        if (::lstat(complete, &info) != 0) return result.fail("lstat completed probe");
        if (!S_ISREG(info.st_mode) || info.st_size != sizeof(token))
            return result.fail("size completed probe", EIO);
        if (::unlink(complete) != 0) return result.fail("unlink storage probe");
        renamed = false;
        if (::rmdir(directory) != 0) return result.fail("rmdir storage probe");
        return true;
    }();
    if (!ok) {
        if (descriptor >= 0) (void)::close(descriptor);
        (void)::unlink(renamed ? complete : temporary);
        (void)::rmdir(directory);
    }
    return ok;
}

bool needs_filesystem_request(const StorageProbe& probe) noexcept {
    // A full disk, corrupt storage or a redirected directory is not fixed by
    // granting filesystem access. Preserve those errors for diagnostics.
    return probe.error == EPERM || probe.error == EACCES || probe.error == ENOENT;
}

void boot_write(const char* text) noexcept {
    if (boot_descriptor < 0 || !text) return;
    std::size_t left = std::strlen(text);
    while (left) {
        const auto count = ::write(boot_descriptor, text, left);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        text += count;
        left -= static_cast<std::size_t>(count);
    }
}
} // namespace

Ps5StoragePaths ps5_prepare_storage() noexcept {
    Ps5StoragePaths paths;
    StorageProbe before, after;
    paths.filesystem_available = probe_storage(before);
    after = before;
    if (!paths.filesystem_available && needs_filesystem_request(before)) {
        paths.helper_requested = true;
        paths.filesystem_status = static_cast<int>(
            elevation::request(elevation::Capability::filesystem));
        if (paths.filesystem_status == static_cast<int>(elevation::Status::ok)) {
            after = {};
            paths.filesystem_available = probe_storage(after);
        }
    }
    // Keep startup diagnostics when the loader permits plain log writes even
    // if a stronger operation needed by Downloads was refused.
    const bool ready = prepare_log_directory();

    // A filesystem grant changes the process's root. Resolve the application's
    // existing mounts before any fonts, settings or caches are opened. Prefer
    // the currently mounted image over a possibly stale extracted app folder.
    if (has_app_files("/app0")) paths.app = "/app0";
    else if (has_app_files(kSandboxApp)) paths.app = kSandboxApp;
    else if (has_app_files(kInstalledApp)) paths.app = kInstalledApp;
    download_writer::configure_helper(std::string(paths.app) + "/download-writer.elf");
    if (!is_directory("/download0") &&
        is_directory("/mnt/sandbox/PPSA74126_000/download0"))
        paths.data = kSandboxData;

    if (ready) {
        // Keep only the current and preceding startup receipt in this folder.
        (void)::rename("/data/Stremio/boot-current.txt", "/data/Stremio/boot-last.txt");
        boot_descriptor = ::open("/data/Stremio/boot-current.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        paths.logs_available = boot_descriptor >= 0;
    }
    char status[640];
    std::snprintf(status, sizeof(status),
        "[Stremio Plus %s %s] filesystem_status=%d helper_requested=%d logs_available=%d filesystem_available=%d\n"
        "storage_probe before=\"%s\" before_errno=%d after=\"%s\" after_errno=%d\n",
        STREMIO_VERSION, STREMIO_TITLE_ID, paths.filesystem_status, int(paths.helper_requested),
        int(paths.logs_available), int(paths.filesystem_available),
        before.stage, before.error, after.stage, after.error);
    (void)sceKernelDebugOutText(0, status);
    boot_write(status);
    ps5_boot_note("filesystem paths resolved");
    return paths;
}

void ps5_boot_note(const char* stage) noexcept {
    boot_write(stage);
    boot_write("\n");
}

void ps5_boot_close() noexcept {
    if (boot_descriptor >= 0) (void)::close(boot_descriptor);
    boot_descriptor = -1;
}
