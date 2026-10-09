// Download directory browsing and creation. SPDX-License-Identifier: GPL-3.0-or-later
#include "download_directory.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef PLATFORM_PS5_NATIVE
#include "../native/download_writer/protocol.hpp"
extern "C" int sceKernelGetdents(int, char*, int);
#endif

namespace download_directory {
namespace {
constexpr std::size_t max_path = 1023;
constexpr std::size_t max_folders = 4096;
constexpr std::size_t max_entries = 65536;

bool name_valid(const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name.size() > 255) return false;
    for (unsigned char c : name)
        if (c == '/' || c == '\\' || c == 0 || c < 32 || c == 127) return false;
    return true;
}

bool cancelled(const std::shared_ptr<std::atomic<bool>>& cancel) {
    return cancel && cancel->load(std::memory_order_relaxed);
}

bool directory(const std::string& path) {
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

struct Directory {
    int fd = -1;
    ~Directory() { if (fd >= 0) ::close(fd); }
};

bool same_directory(int left, int right) {
    struct stat a{}, b{};
    return ::fstat(left, &a) == 0 && ::fstat(right, &b) == 0 &&
        S_ISDIR(a.st_mode) && S_ISDIR(b.st_mode) && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

int open_real_directory(const std::string& path, int& error) {
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) { error = errno; return -1; }
    for (std::size_t at = 1; at < path.size();) {
        const auto slash = path.find('/', at);
        const auto end = slash == path.npos ? path.size() : slash;
#ifdef PLATFORM_PS5_NATIVE
        const auto prefix = path.substr(0, end);
        struct stat before{}, after{};
        const bool identified = ::lstat(prefix.c_str(), &before) == 0;
        if ((!identified && errno != EPERM && errno != EACCES) ||
            (identified && (!S_ISDIR(before.st_mode) || S_ISLNK(before.st_mode)))) {
            error = identified ? ELOOP : errno; ::close(fd); return -1;
        }
        const int next = ::open(prefix.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (next >= 0 && (::fstat(next, &after) != 0 || !S_ISDIR(after.st_mode) ||
            (identified && (before.st_dev != after.st_dev || before.st_ino != after.st_ino)))) {
            error = ELOOP; ::close(next); ::close(fd); return -1;
        }
#else
        const auto part = path.substr(at, end - at);
        const int next = ::openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
#endif
        const int saved = errno;
        ::close(fd);
        if (next < 0) { error = saved; return -1; }
        fd = next;
        at = end + 1;
    }
    return fd;
}

bool parent_unchanged(const std::string& path, int expected, int& error) {
    Directory current{open_real_directory(path, error)};
    if (current.fd < 0) return false;
    if (same_directory(expected, current.fd)) return true;
    error = ELOOP;
    return false;
}

int writable(const std::string& path, int fd) {
    int error = 0;
    if (!parent_unchanged(path, fd, error)) return error;
    struct stat before{}, after{};
    if (::fstat(fd, &before) != 0) return errno;
    if (!S_ISDIR(before.st_mode)) return EACCES;
    if (::fchmod(fd, 0777) != 0) return errno;
    if (::fstat(fd, &after) != 0) return errno;
    if (!S_ISDIR(after.st_mode) || (after.st_mode & 07777) != 0777 ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino) return EACCES;
    static std::atomic<unsigned int> probe_counter{0};
    const std::string probe = ".rbtvplus-folder-probe-" + std::to_string(::getpid()) + '-' +
        std::to_string(probe_counter.fetch_add(1, std::memory_order_relaxed));
#ifdef PLATFORM_PS5_NATIVE
    if (!parent_unchanged(path, fd, error)) return error;
    const auto probe_path = path + '/' + probe;
    const int check = ::open(probe_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#else
    const int check = ::openat(fd, probe.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#endif
    if (check < 0) return errno;
    if (::write(check, "\0", 1) != 1 || ::fsync(check) != 0) error = errno ? errno : EIO;
    if (::close(check) != 0 && !error) error = errno;
#ifdef PLATFORM_PS5_NATIVE
    if (!parent_unchanged(path, fd, error)) return error;
    if (::unlink(probe_path.c_str()) != 0 && !error) error = errno;
#else
    if (::unlinkat(fd, probe.c_str(), 0) != 0 && !error) error = errno;
#endif
    if (::fsync(fd) != 0 && errno != EINVAL && errno != ENOTSUP && !error) error = errno;
    return error;
}

void sort(Listing& listing) {
    std::sort(listing.folders.begin(), listing.folders.end(), [](const auto& a, const auto& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
            [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
    });
}
} // namespace

std::string normalize(const std::string& path) {
    if (path.empty() || path.front() != '/' || path.size() > max_path) return {};
    for (unsigned char c : path) if (c == '\\' || c == 0 || c < 32 || c == 127) return {};
    std::vector<std::string> parts;
    for (std::size_t at = 1; at < path.size();) {
        const auto slash = path.find('/', at);
        const auto part = path.substr(at, slash == std::string::npos ? path.size() - at : slash - at);
        if (part == "..") {
            if (parts.empty()) return {};
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            if (!name_valid(part)) return {};
            parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    std::string result;
    for (const auto& part : parts) result += '/' + part;
    return result.empty() ? "/" : result;
}

std::string parent(const std::string& path) {
    const auto clean = normalize(path);
    if (clean.empty() || clean == "/") return "/";
    const auto slash = clean.find_last_of('/');
    return slash == 0 ? "/" : clean.substr(0, slash);
}

std::string child(const std::string& path, const std::string& name) {
    const auto clean = normalize(path);
    if (clean.empty() || !name_valid(name)) return {};
    const auto joined = clean == "/" ? clean + name : clean + '/' + name;
    return joined.size() > max_path ? std::string{} : joined;
}

Listing list(const std::string& path, const std::shared_ptr<std::atomic<bool>>& cancel) {
    Listing result;
    result.path = normalize(path);
    if (result.path.empty()) { result.error = EINVAL; return result; }
    if (cancelled(cancel)) { result.error = ECANCELED; return result; }
    std::size_t scanned = 0;
    auto append = [&](const std::string& name) {
        if (name_valid(name) && directory(child(result.path, name))) result.folders.push_back(name);
    };
#ifdef PLATFORM_PS5_NATIVE
    const int fd = ::open(result.path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) { result.error = errno; return result; }
    struct Close { int fd; ~Close() { ::close(fd); } } close_fd{fd};
    // ProsperoEden uses the same 64 KiB read size for mounted PS5 volumes.
    // Keep the buffer off the worker's small native stack.
    std::vector<char> bytes(64u << 10);
    while (!result.error && !result.limited && !cancelled(cancel)) {
        const int count = sceKernelGetdents(fd, bytes.data(), static_cast<int>(bytes.size()));
        if (count == 0) break;
        if (count < 0 || count > static_cast<int>(bytes.size())) {
            result.error = count == -1 ? errno : count < 0
                ? static_cast<int>(static_cast<unsigned int>(count) & 0xffffu) : EIO;
            if (result.error == 0) result.error = EIO;
            break;
        }
        for (std::size_t at = 0; at < static_cast<std::size_t>(count);) {
            constexpr auto name_at = offsetof(dirent, d_name);
            const auto left = static_cast<std::size_t>(count) - at;
            if (left <= name_at) { result.error = EIO; break; }
            std::uint16_t length = 0;
            std::memcpy(&length, bytes.data() + at + offsetof(dirent, d_reclen), sizeof(length));
            if (length <= name_at || length > left) { result.error = EIO; break; }
            const char* name = bytes.data() + at + name_at;
            const auto end = static_cast<const char*>(std::memchr(name, 0, length - name_at));
            if (!end) { result.error = EIO; break; }
            append(std::string(name, static_cast<std::size_t>(end - name)));
            at += length;
            if (++scanned >= max_entries || result.folders.size() >= max_folders) {
                result.limited = true; break;
            }
            if (cancelled(cancel)) break;
        }
    }
#else
    DIR* handle = ::opendir(result.path.c_str());
    if (!handle) { result.error = errno; return result; }
    struct Close { DIR* value; ~Close() { ::closedir(value); } } close_dir{handle};
    while (!cancelled(cancel)) {
        errno = 0;
        const auto entry = ::readdir(handle);
        if (!entry) { result.error = errno; break; }
        append(entry->d_name);
        if (++scanned >= max_entries || result.folders.size() >= max_folders) {
            result.limited = true; break;
        }
    }
#endif
    if (cancelled(cancel)) { result.error = ECANCELED; result.folders.clear(); }
    if (result.error) result.folders.clear();
    sort(result);
    return result;
}

Creation create(const std::string& path) {
    Creation result;
    const auto clean = normalize(path);
    result.path = child(clean, folder_name);
    if (clean.empty() || result.path.empty()) { result.error = EINVAL; return result; }
#ifdef PLATFORM_PS5_NATIVE
    if (!download_writer::wire::download_directory(clean) ||
        !download_writer::wire::download_directory(result.path)) { result.error = EINVAL; return result; }
#endif
    Directory parent_fd{open_real_directory(clean, result.error)};
    if (parent_fd.fd < 0) return result;
    if (!parent_unchanged(clean, parent_fd.fd, result.error)) return result;
#ifdef PLATFORM_PS5_NATIVE
    const int made = ::mkdir(result.path.c_str(), 0777);
#else
    const int made = ::mkdirat(parent_fd.fd, folder_name, 0777);
#endif
    if (made != 0 && errno != EEXIST) { result.error = errno; return result; }
    result.created = made == 0;
    if (!parent_unchanged(clean, parent_fd.fd, result.error)) return result;
#ifdef PLATFORM_PS5_NATIVE
    Directory child_fd{open_real_directory(result.path, result.error)};
#else
    Directory child_fd{::openat(parent_fd.fd, folder_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW)};
    if (child_fd.fd < 0) result.error = errno;
#endif
    if (child_fd.fd < 0 || !parent_unchanged(clean, parent_fd.fd, result.error)) return result;
    result.error = writable(result.path, child_fd.fd);
    return result;
}

std::string error_text(int error, bool italian) {
    if (error == EACCES || error == EPERM)
        return italian ? "Accesso negato. Controlla i permessi della cartella." : "Access denied. Check the folder permissions.";
    if (error == ENOSPC || error == EDQUOT)
        return italian ? "Spazio libero insufficiente su questa unità." : "Not enough free space on this drive.";
    if (error == EROFS)
        return italian ? "Questa unità è di sola lettura." : "This drive is read-only.";
    if (error == ENOENT || error == ENODEV || error == ENXIO)
        return italian ? "La cartella o l'unità non è disponibile. Ricollegala e aggiorna." : "The folder or drive is unavailable. Reconnect it and refresh.";
    if (error == ELOOP)
        return italian ? "Scegli una cartella reale, senza collegamenti simbolici." : "Choose a real folder without symbolic links.";
    if (error == EEXIST)
        return italian ? "Esiste già un file con questo nome." : "A file with this name already exists.";
    if (error == ENOTDIR || error == EINVAL || error == ENAMETOOLONG)
        return italian ? "Il percorso non è una cartella valida." : "The path is not a valid folder.";
    return (italian ? "Impossibile aprire la cartella (errore " : "Could not open the folder (error ") +
        std::to_string(error ? error : EIO) + ").";
}
} // namespace download_directory
