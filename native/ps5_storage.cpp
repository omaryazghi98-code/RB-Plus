// RBTV+ native startup storage. SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_storage.h"
#include "filesystem/elevation.hpp"
#include "download_writer/client.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <initializer_list>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int sceKernelDebugOutText(int channel, const char* text);

#ifdef PLATFORM_PS5_NATIVE
namespace {
struct DiagnosticSockaddrIn {
    std::uint8_t length;
    std::uint8_t family;
    std::uint16_t port;
    std::uint32_t address;
    std::uint16_t virtual_port;
    std::uint8_t zero[6];
};
struct DiagnosticConfig {
    char server[64]{};
    std::uint16_t port = 9300;
    bool enabled = false;
};
}
extern "C" {
int sceNetConnect(int socket, const void* address, std::uint32_t address_length);
int sceNetSend(int socket, const void* data, std::size_t length, int flags);
int sceNetSetsockopt(int socket, int level, int option, const void* value, std::uint32_t size);
int sceNetSocket(const char* name, int domain, int type, int protocol);
int sceNetSocketClose(int socket);
}
#endif

namespace {
constexpr char kLogDirectory[] = "/data/RBTVPlus";
constexpr char kAppDataDirectory[] = "/data/RBTVPlus/appdata";
constexpr char kSandboxApp[] = "/mnt/sandbox/PPSA98273_000/app0";
constexpr char kSandboxData[] = "/mnt/sandbox/PPSA98273_000/download0/rbtvplus";
constexpr char kLegacySandboxData[] = "/mnt/sandbox/PPSA74126_000/download0/stremio";
constexpr char kInstalledApp[] = "/data/homebrew/PPSA98273";
int boot_descriptor = -1;

struct Descriptor {
    int value = -1;
    explicit Descriptor(int descriptor = -1) noexcept : value(descriptor) {}
    ~Descriptor() { if (value >= 0) (void)::close(value); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int close() noexcept {
        const int current = value;
        value = -1;
        return current < 0 ? 0 : ::close(current);
    }
};

struct StorageProbe {
    const char* stage = "ready";
    int error = 0;

    bool fail(const char* operation, int code = errno) noexcept {
        stage = operation;
        error = code != 0 ? code : EIO;
        return false;
    }
};

#ifdef PLATFORM_PS5_NATIVE
bool diagnostic_ipv4(const char* text, std::uint32_t& out) noexcept {
    std::uint32_t address = 0;
    const char* p = text;
    for (unsigned i = 0; i < 4; ++i) {
        if (*p < '0' || *p > '9') return false;
        unsigned value = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + static_cast<unsigned>(*p - '0');
            if (++digits > 3 || value > 255) return false;
            ++p;
        }
        address |= value << (i * 8);
        if (i != 3) {
            if (*p++ != '.') return false;
        } else if (*p != '\0') {
            return false;
        }
    }
    out = address;
    return true;
}

bool diagnostic_read_config(DiagnosticConfig& config) noexcept {
    constexpr const char* paths[] = {
        "/app0/dev.conf",
        "/mnt/sandbox/PPSA98273_000/app0/dev.conf"
    };
    char contents[2048]{};
    std::size_t used = 0;
    for (const char* path : paths) {
        const int fd = ::open(path, O_RDONLY);
        if (fd < 0) continue;
        while (used + 1 < sizeof(contents)) {
            const auto count = ::read(fd, contents + used, sizeof(contents) - used - 1);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            used += static_cast<std::size_t>(count);
        }
        (void)::close(fd);
        if (used) break;
    }
    if (!used) return false;
    contents[used] = '\0';

    const char* enabled = std::strstr(contents, "DEV_ENABLED=");
    const char* server = std::strstr(contents, "DEV_SERVER=");
    const char* port = std::strstr(contents, "DEV_PORT=");
    if (!enabled || enabled[12] != '1' || !server || !port) return false;
    server += sizeof("DEV_SERVER=") - 1;
    const auto server_length = std::strcspn(server, "\r\n");
    if (!server_length || server_length >= sizeof(config.server)) return false;
    std::memcpy(config.server, server, server_length);
    config.server[server_length] = '\0';
    std::uint32_t ignored = 0;
    if (!diagnostic_ipv4(config.server, ignored)) return false;

    port += sizeof("DEV_PORT=") - 1;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(port, &end, 10);
    if (end == port || parsed < 1 || parsed > 65535) return false;
    config.port = static_cast<std::uint16_t>(parsed);
    config.enabled = true;
    return true;
}

void diagnostic_event(const char* message) noexcept {
    DiagnosticConfig config;
    std::uint32_t address_value = 0;
    if (!message || !diagnostic_read_config(config) || !config.enabled ||
        !diagnostic_ipv4(config.server, address_value)) return;

    const int socket = sceNetSocket("rbtvplus-diag", 2, 1, 6);
    if (socket < 0) return;
    constexpr int level = 0xffff;
    constexpr int timeout_us = 250000;
    (void)sceNetSetsockopt(socket, level, 0x1105, &timeout_us, sizeof(timeout_us));
    (void)sceNetSetsockopt(socket, level, 0x1106, &timeout_us, sizeof(timeout_us));
    (void)sceNetSetsockopt(socket, level, 0x1109, &timeout_us, sizeof(timeout_us));
    const auto port = static_cast<std::uint16_t>(
        (config.port << 8) | (config.port >> 8));
    const DiagnosticSockaddrIn endpoint{
        sizeof(DiagnosticSockaddrIn), 2, port, address_value, 0, {0}
    };
    if (sceNetConnect(socket, &endpoint, sizeof(endpoint)) >= 0) {
        char line[640];
        const int length = std::snprintf(line, sizeof(line), "RBTVAPP %s\n", message);
        if (length > 0 && static_cast<std::size_t>(length) < sizeof(line)) {
            std::size_t offset = 0;
            while (offset < static_cast<std::size_t>(length)) {
                const int count = sceNetSend(socket, line + offset,
                    static_cast<std::size_t>(length) - offset, 0);
                if (count <= 0) break;
                offset += static_cast<std::size_t>(count);
            }
        }
    }
    (void)sceNetSocketClose(socket);
}

#else
void diagnostic_event(const char*) noexcept {}
#endif

void report_probe(const char* phase, bool available, const StorageProbe& probe) noexcept {
    char message[400];
    std::snprintf(message, sizeof(message),
        "phase=%s available=%d stage=\"%s\" errno=%d uid=%u euid=%u gid=%u egid=%u",
        phase, int(available), probe.stage, probe.error,
        unsigned(::getuid()), unsigned(::geteuid()), unsigned(::getgid()), unsigned(::getegid()));
    diagnostic_event(message);
}

void report_directory(const char* phase, const char* path) noexcept {
    struct stat info{};
    char message[400];
    if (::lstat(path, &info) == 0) {
        std::snprintf(message, sizeof(message),
            "phase=%s path=%s mode=%04o uid=%u gid=%u dev=%llu ino=%llu",
            phase, path, unsigned(info.st_mode & 07777), unsigned(info.st_uid), unsigned(info.st_gid),
            static_cast<unsigned long long>(info.st_dev), static_cast<unsigned long long>(info.st_ino));
    } else {
        const int error = errno;
        std::snprintf(message, sizeof(message), "phase=%s path=%s lstat_errno=%d",
            phase, path, error);
    }
    diagnostic_event(message);
}

struct DirectorySnapshot {
    struct stat info{};
    int error = 0;
};

DirectorySnapshot directory_snapshot(const char* path) noexcept {
    DirectorySnapshot result;
    if (::lstat(path, &result.info) != 0) result.error = errno;
    return result;
}

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
bool probe_writes(const char* parent, StorageProbe& result) noexcept {
    struct stat info{};
    // Never reuse a probe left by an interrupted launch. Only the directory
    // created by this invocation is removed, and the probe contains no user data.
    char directory[128], temporary[160], complete[160];
    bool created = false;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        std::snprintf(directory, sizeof(directory), "%s/.storage-probe-%ld-%u",
            parent, static_cast<long>(::getpid()), attempt);
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
        constexpr char token[] = "RBTV+ storage proof\n";
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

bool probe_storage(StorageProbe& result) noexcept {
    struct stat info{};
    if (::lstat("/data", &info) != 0) return result.fail("lstat /data");
    if (!S_ISDIR(info.st_mode)) return result.fail("directory /data", ENOTDIR);
    if (::mkdir(kLogDirectory, 0700) != 0 && errno != EEXIST)
        return result.fail("mkdir /data/RBTVPlus");
    if (::lstat(kLogDirectory, &info) != 0) return result.fail("lstat /data/RBTVPlus");
    if (!S_ISDIR(info.st_mode)) return result.fail("directory /data/RBTVPlus", ENOTDIR);
    for (const char* directory : {"/data", kLogDirectory}) {
        DIR* entries = ::opendir(directory);
        if (!entries) return result.fail(directory == kLogDirectory ? "opendir /data/RBTVPlus" : "opendir /data");
        errno = 0;
        const auto entry = ::readdir(entries);
        const int read_error = entry == nullptr ? errno : 0;
        const int closed = ::closedir(entries);
        if (read_error) return result.fail("readdir storage", read_error);
        if (closed != 0) return result.fail("closedir storage");
    }
    return probe_writes(kLogDirectory, result);
}

bool regular_destination(const char* path, bool& exists, StorageProbe& result) noexcept {
    struct stat info{};
    exists = ::lstat(path, &info) == 0;
    if (!exists) return errno == ENOENT || result.fail("lstat appdata target");
    if (!S_ISREG(info.st_mode))
        return result.fail("regular appdata target", S_ISLNK(info.st_mode) ? ELOOP : EINVAL);
    return true;
}

bool migrate_file(const char* legacy, const char* name, off_t limit,
                  StorageProbe& result, unsigned& copied) noexcept {
    char source[192], target[192], temporary[240];
    std::snprintf(source, sizeof(source), "%s/%s", legacy, name);
    std::snprintf(target, sizeof(target), "%s/%s", kAppDataDirectory, name);
    bool exists = false;
    if (!regular_destination(target, exists, result)) return false;
    if (exists) return true;

    struct stat before{};
    if (::lstat(source, &before) != 0)
        return errno == ENOENT || result.fail("lstat legacy settings");
    if (!S_ISREG(before.st_mode))
        return result.fail("regular legacy settings", S_ISLNK(before.st_mode) ? ELOOP : EINVAL);
    if (before.st_size < 0 || before.st_size > limit)
        return result.fail("legacy settings size limit", EFBIG);
    Descriptor input(::open(source, O_RDONLY | O_NOFOLLOW | O_NONBLOCK));
    if (input.value < 0) return result.fail("open legacy settings");
    struct stat opened{};
    if (::fstat(input.value, &opened) != 0) return result.fail("fstat legacy settings");
    if (!S_ISREG(opened.st_mode) || opened.st_dev != before.st_dev ||
        opened.st_ino != before.st_ino || opened.st_size != before.st_size)
        return result.fail("legacy settings changed", EAGAIN);

    Descriptor output;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        std::snprintf(temporary, sizeof(temporary), "%s/.migration-%ld-%u-%s.tmp",
            kAppDataDirectory, static_cast<long>(::getpid()), attempt, name);
        output.value = ::open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (output.value >= 0) break;
        if (errno != EEXIST) return result.fail("open migration temporary");
    }
    if (output.value < 0) return result.fail("open migration temporary", EEXIST);
    bool ok = [&]() noexcept {
        struct stat info{};
        if (::fstat(output.value, &info) != 0) return result.fail("fstat migration temporary");
        if (!S_ISREG(info.st_mode)) return result.fail("regular migration temporary", EINVAL);
        char bytes[16384];
        off_t remaining = opened.st_size;
        while (remaining > 0) {
            const size_t wanted = remaining < static_cast<off_t>(sizeof(bytes)) ? size_t(remaining) : sizeof(bytes);
            const auto count = ::read(input.value, bytes, wanted);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return result.fail("read legacy settings", count < 0 ? errno : EIO);
            size_t offset = 0;
            while (offset < size_t(count)) {
                const auto written = ::write(output.value, bytes + offset, size_t(count) - offset);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) return result.fail("write migration temporary", written < 0 ? errno : EIO);
                offset += size_t(written);
            }
            remaining -= count;
        }
        if (::fstat(input.value, &info) != 0) return result.fail("fstat copied legacy settings");
        if (info.st_size != opened.st_size || info.st_mtime != opened.st_mtime ||
            info.st_ctime != opened.st_ctime)
            return result.fail("legacy settings changed", EAGAIN);
        if (::fsync(output.value) != 0) return result.fail("fsync migration temporary");
        if (output.close() != 0) return result.fail("close migration temporary");
        // Native startup is the sole appdata writer: no application workers
        // exist yet. Recheck immediately before the atomic rename and keep
        // an existing target. link/linkat are payload-only in the pinned SDK.
        if (!regular_destination(target, exists, result)) return false;
        if (exists) return true;
        if (::rename(temporary, target) != 0) return result.fail("publish migrated settings");
        ++copied;
        return true;
    }();
    (void)output.close();
    const int cleanup = ::unlink(temporary);
    if (ok && cleanup != 0 && errno != ENOENT) return result.fail("unlink migration temporary");
    return ok;
}

bool prepare_appdata(StorageProbe& result, unsigned& copied) noexcept {
    // Called only after the genuine /data probe. Never manufacture /data or
    // change modes of an existing appdata, Stremio, or downloads directory.
    if (::mkdir(kAppDataDirectory, 0700) != 0 && errno != EEXIST)
        return result.fail("mkdir appdata");
    struct stat before{};
    if (::lstat(kAppDataDirectory, &before) != 0) return result.fail("lstat appdata");
    if (!S_ISDIR(before.st_mode))
        return result.fail("directory appdata", S_ISLNK(before.st_mode) ? ELOOP : ENOTDIR);
    Descriptor directory(::open(kAppDataDirectory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
    if (directory.value < 0) return result.fail("open appdata directory");
    struct stat opened{};
    if (::fstat(directory.value, &opened) != 0) return result.fail("fstat appdata directory");
    if (!S_ISDIR(opened.st_mode) || opened.st_dev != before.st_dev || opened.st_ino != before.st_ino)
        return result.fail("appdata directory changed", EAGAIN);
    if (!probe_writes(kAppDataDirectory, result)) return false;

    struct MigrationFile { const char* name; off_t limit; };
    constexpr MigrationFile files[] = {
        {"settings.json", 4 << 20}, {"progress.json", 16 << 20}, {"config.json", 4 << 20},
    };
    // Validate existing target types even when no old mount is available.
    for (const auto& file : files) {
        char target[192];
        std::snprintf(target, sizeof(target), "%s/%s", kAppDataDirectory, file.name);
        bool exists = false;
        if (!regular_destination(target, exists, result)) return false;
    }
    for (const char* legacy : {"/download0/stremio", kLegacySandboxData, kSandboxData}) {
        struct stat info{};
        if (::lstat(legacy, &info) != 0) {
            if (errno == ENOENT || errno == ENOTDIR) continue;
            return result.fail("lstat legacy data directory");
        }
        if (!S_ISDIR(info.st_mode))
            return result.fail("directory legacy data", S_ISLNK(info.st_mode) ? ELOOP : ENOTDIR);
        Descriptor old_directory(::open(legacy, O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
        if (old_directory.value < 0) return result.fail("open legacy data directory");
        struct stat old_opened{};
        if (::fstat(old_directory.value, &old_opened) != 0) return result.fail("fstat legacy data directory");
        if (!S_ISDIR(old_opened.st_mode) || info.st_dev != old_opened.st_dev || info.st_ino != old_opened.st_ino)
            return result.fail("legacy directory changed", EAGAIN);
        for (const auto& file : files)
            if (!migrate_file(legacy, file.name, file.limit, result, copied)) return false;
    }
    return true;
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

void log_directory(const char* phase, const char* path, const DirectorySnapshot& snapshot) noexcept {
    char text[320];
    if (snapshot.error)
        std::snprintf(text, sizeof(text), "storage_directory phase=%s path=%s errno=%d\n",
            phase, path, snapshot.error);
    else
        std::snprintf(text, sizeof(text),
            "storage_directory phase=%s path=%s mode=%04o owner=%u group=%u device=%llu\n",
            phase, path, unsigned(snapshot.info.st_mode & 07777), unsigned(snapshot.info.st_uid),
            unsigned(snapshot.info.st_gid), static_cast<unsigned long long>(snapshot.info.st_dev));
    (void)sceKernelDebugOutText(0, text);
    boot_write(text);
}
} // namespace

Ps5StoragePaths ps5_prepare_storage() noexcept {
    // The recovery screen may retry before application workers are started.
    // Close the preceding receipt before rotating it and proving storage again.
    ps5_boot_close();
    Ps5StoragePaths paths;
    const auto root_before = directory_snapshot(kLogDirectory);
    const auto downloads_before = directory_snapshot("/data/RBTVPlus/downloads");
    const auto euid_before = ::geteuid();
    StorageProbe before, after;
    bool elevation_granted = false;
    paths.filesystem_available = probe_storage(before);
    report_probe("initial_probe", paths.filesystem_available, before);
    after = before;
    if (!paths.filesystem_available && needs_filesystem_request(before)) {
        paths.helper_requested = true;
        diagnostic_event("phase=elevation_request_begin");
        paths.filesystem_status = static_cast<int>(
            elevation::request(elevation::Capability::filesystem));
        char elevation_message[256];
        std::snprintf(elevation_message, sizeof(elevation_message),
            "phase=elevation_return status=%d uid=%u euid=%u gid=%u egid=%u",
            paths.filesystem_status, unsigned(::getuid()), unsigned(::geteuid()),
            unsigned(::getgid()), unsigned(::getegid()));
        diagnostic_event(elevation_message);
        if (paths.filesystem_status == static_cast<int>(elevation::Status::ok)) {
            elevation_granted = true;
            after = {};
            paths.filesystem_available = probe_storage(after);
            report_probe("post_elevation_probe", paths.filesystem_available, after);
            report_directory("post_elevation", "/data");
            report_directory("post_elevation", kLogDirectory);
            report_directory("post_elevation", kAppDataDirectory);
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
    StorageProbe data_probe = after;
    unsigned migrated_files = 0;
    // A granted title can still be denied read/enumeration access to the shared
    // logging directory while its dedicated appdata directory is usable. After
    // a successful grant, test appdata directly instead of making that parent
    // directory listing a hard prerequisite. prepare_appdata performs its own
    // mkdir/lstat/open/write/read/rename checks and reports the concrete failure.
    if (paths.filesystem_available || elevation_granted) {
        data_probe = {};
        paths.data_available = prepare_appdata(data_probe, migrated_files);
        report_probe("appdata_probe", paths.data_available, data_probe);
        if (paths.data_available) {
            paths.filesystem_available = true;
            after = data_probe;
        }
    }
    if (!paths.data_available) {
        paths.data_error = data_probe.stage;
        paths.data_errno = data_probe.error != 0 ? data_probe.error : EIO;
    }

    if (ready) {
        // Keep only the current and preceding startup receipt in this folder.
        (void)::rename("/data/RBTVPlus/boot-current.txt", "/data/RBTVPlus/boot-last.txt");
        boot_descriptor = ::open("/data/RBTVPlus/boot-current.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        paths.logs_available = boot_descriptor >= 0;
    }
    char status[640];
    std::snprintf(status, sizeof(status),
        "[RBTV+ %s %s] filesystem_status=%d helper_requested=%d logs_available=%d filesystem_available=%d\n"
        "storage_probe before=\"%s\" before_errno=%d after=\"%s\" after_errno=%d\n",
        STREMIO_VERSION, STREMIO_TITLE_ID, paths.filesystem_status, int(paths.helper_requested),
        int(paths.logs_available), int(paths.filesystem_available),
        before.stage, before.error, after.stage, after.error);
    (void)sceKernelDebugOutText(0, status);
    boot_write(status);
    std::snprintf(status, sizeof(status),
        "appdata path=%s available=%d stage=\"%s\" errno=%d migrated_files=%u\n",
        paths.data, int(paths.data_available), data_probe.stage, data_probe.error, migrated_files);
    (void)sceKernelDebugOutText(0, status);
    boot_write(status);
    std::snprintf(status, sizeof(status), "storage_process euid_before=%u euid_after=%u\n",
        unsigned(euid_before), unsigned(::geteuid()));
    (void)sceKernelDebugOutText(0, status);
    boot_write(status);
    log_directory("before", kLogDirectory, root_before);
    log_directory("before", "/data/RBTVPlus/downloads", downloads_before);
    log_directory("after", kLogDirectory, directory_snapshot(kLogDirectory));
    log_directory("after", "/data/RBTVPlus/downloads", directory_snapshot("/data/RBTVPlus/downloads"));
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
