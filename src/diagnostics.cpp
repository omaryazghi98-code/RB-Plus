// RBTV+ - Bounded asynchronous diagnostics with credential redaction.
// Copyright (C) 2026 Stremio PS5 contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#if defined(__linux__) || defined(PLATFORM_PS5)
#include <sys/ucontext.h>
#endif

#ifdef PLATFORM_PS5
extern "C" int sceKernelDebugOutText(int channel, const char* text);
#endif

#ifndef STREMIO_DIAGNOSTICS_ROTATE_BYTES
#define STREMIO_DIAGNOSTICS_ROTATE_BYTES (4u * 1024u * 1024u)
#endif
#ifndef STREMIO_VERSION
#error "Diagnostics require the application version supplied by the build"
#endif
#ifndef STREMIO_TITLE_ID
#error "Diagnostics require the title ID supplied by the build"
#endif
#ifndef STREMIO_BUILD_ID
#define STREMIO_BUILD_ID STREMIO_VERSION " " __DATE__ " " __TIME__
#endif

namespace {
constexpr std::size_t kFileBytes = STREMIO_DIAGNOSTICS_ROTATE_BYTES;
constexpr std::size_t kMessageBytes = 4096;
constexpr std::size_t kQueueEntries = 1024;
constexpr std::size_t kQueueBytes = 2u * 1024u * 1024u;
constexpr int kGenerations = 3; // active plus two older files
constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

bool ascii_alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
std::string ascii_lower(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    return text;
}
int unhex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode ordinary URL/JSON escaping before finding secret labels. Decoding is
// bounded and is used only for log text, never for a request or configuration.
std::string unescape_for_redaction(const std::string& input) {
    std::string current = input.substr(0, kMessageBytes);
    for (int pass = 0; pass < 2; ++pass) {
        std::string next;
        next.reserve(current.size());
        for (std::size_t i = 0; i < current.size(); ++i) {
            if (current[i] == '%' && i + 2 < current.size() && unhex(current[i + 1]) >= 0 && unhex(current[i + 2]) >= 0) {
                next += char(unhex(current[i + 1]) * 16 + unhex(current[i + 2]));
                i += 2;
            } else if (current[i] == '\\' && i + 1 < current.size()) {
                const char escaped = current[i + 1];
                if (escaped == '/' || escaped == '"' || escaped == '\\') { next += escaped; ++i; }
                else if (escaped == 'n' || escaped == 'r' || escaped == 't') { next += '\n'; ++i; }
                else if (escaped == 'u' && i + 5 < current.size() && current[i + 2] == '0' && current[i + 3] == '0' &&
                         unhex(current[i + 4]) >= 0 && unhex(current[i + 5]) >= 0) {
                    next += char(unhex(current[i + 4]) * 16 + unhex(current[i + 5]));
                    i += 5;
                } else next += current[i];
            } else next += current[i];
        }
        if (next == current) break;
        current = std::move(next);
    }
    return current;
}

bool url_end(unsigned char c) {
    return c <= 32 || c == '"' || c == '\'' || c == '<' || c == '>' || c == ')' || c == ']' || c == '}';
}

std::string redact_urls(const std::string& input) {
    const std::string folded = ascii_lower(input);
    static const char* schemes[] = {"https://", "http://", "stremio://", "magnet:?", "ftp://", "file://", "wss://", "ws://", "socks5://"};
    std::string output;
    for (std::size_t i = 0; i < input.size();) {
        std::size_t prefix = 0;
        for (const char* scheme : schemes) {
            if (folded.compare(i, std::strlen(scheme), scheme) == 0) { prefix = std::strlen(scheme); break; }
        }
        // Relative request paths can also carry an add-on configuration token.
        const bool relative = input[i] == '/' && (i == 0 || url_end(static_cast<unsigned char>(input[i - 1])) || input[i - 1] == '=');
        if (!prefix && !relative) { output += input[i++]; continue; }
        std::size_t end = i + (prefix ? prefix : 1);
        while (end < input.size() && !url_end(static_cast<unsigned char>(input[end]))) ++end;
        if (prefix && folded.compare(i, 7, "magnet:") != 0 && folded.compare(i, 7, "file://") != 0) {
            const std::size_t authority_begin = i + prefix;
            std::size_t authority_end = authority_begin;
            while (authority_end < end && input[authority_end] != '/' && input[authority_end] != '?' && input[authority_end] != '#') ++authority_end;
            std::string authority = input.substr(authority_begin, authority_end - authority_begin);
            const auto userinfo = authority.rfind('@');
            if (userinfo != std::string::npos) authority.erase(0, userinfo + 1);
            bool valid_host = !authority.empty() && authority.size() < 254;
            for (unsigned char c : authority) valid_host = valid_host && (ascii_alnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']');
            if (valid_host) output += input.substr(i, prefix) + authority + "/[redacted]";
            else output += "[url:redacted]";
        } else output += relative ? "[path:redacted]" : "[url:redacted]";
        i = end;
    }
    return output;
}

std::string redact_labels(const std::string& input) {
    // Drop the rest of a secret-bearing line. In particular, Cookie contains
    // several semicolon-separated credentials, and a JSON value can contain
    // spaces or punctuation; masking one whitespace-delimited word is unsafe.
    static const char* labels[] = {
        "proxy-authorization", "authorization", "set-cookie", "cookies", "cookie", "headers",
        "client_secret", "clientsecret", "access_token", "accesstoken", "refresh_token", "refreshtoken",
        "auth_key", "authkey", "api_key", "api-key", "apikey", "password", "passwd", "passphrase",
        "secret", "bearer", "basic", "token", "jwt", "email", "username", "user_name", "credential"
    };
    std::string output;
    std::size_t begin = 0;
    while (begin < input.size()) {
        const auto newline = input.find_first_of("\r\n", begin);
        const auto end = newline == std::string::npos ? input.size() : newline;
        const std::string line = input.substr(begin, end - begin);
        const auto folded = ascii_lower(line);
        std::size_t cut = std::string::npos;
        for (const char* label : labels) {
            std::size_t pos = 0;
            while ((pos = folded.find(label, pos)) != std::string::npos) {
                const auto after = pos + std::strlen(label);
                const bool before_ok = pos == 0 || (!ascii_alnum(static_cast<unsigned char>(folded[pos - 1])) && folded[pos - 1] != '_');
                const bool after_ok = after == folded.size() || !ascii_alnum(static_cast<unsigned char>(folded[after]));
                if (before_ok && after_ok) { cut = std::min(cut, pos); break; }
                pos = after;
            }
        }
        if (cut == std::string::npos) output += line;
        else output += line.substr(0, cut) + "[credentials:redacted]";
        if (newline == std::string::npos) break;
        output += " | ";
        begin = end + 1;
    }
    return output;
}

std::string redact_emails_and_tokens(const std::string& input) {
    std::string output = input;
    auto local = [](unsigned char c) { return ascii_alnum(c) || std::strchr(".!#$%&'*+-/=?^_`{|}~", c) != nullptr; };
    std::size_t at = 0;
    while ((at = output.find('@', at)) != std::string::npos) {
        std::size_t begin = at, end = at + 1;
        while (begin && local(static_cast<unsigned char>(output[begin - 1]))) --begin;
        while (end < output.size() && (ascii_alnum(static_cast<unsigned char>(output[end])) || output[end] == '.' || output[end] == '-')) ++end;
        if (begin < at && end > at + 1) {
            output.replace(begin, end - begin, "[email:redacted]");
            at = begin + 16;
        } else ++at;
    }
    // Unlabelled opaque IDs may be debrid keys or JWT segments. Their exact
    // contents are never needed to diagnose transport or rendering failures.
    std::string safe;
    for (std::size_t i = 0; i < output.size();) {
        if (!ascii_alnum(static_cast<unsigned char>(output[i])) && output[i] != '_' && output[i] != '-') { safe += output[i++]; continue; }
        std::size_t end = i;
        while (end < output.size() && (ascii_alnum(static_cast<unsigned char>(output[end])) || output[end] == '_' || output[end] == '-')) ++end;
        if (end - i >= 24) safe += "[opaque:redacted]";
        else safe += output.substr(i, end - i);
        i = end;
    }
    for (char& c : safe) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    if (safe.size() > kMessageBytes) safe.resize(kMessageBytes);
    return safe;
}

std::int64_t epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string utc_stamp(std::int64_t milliseconds) {
    const time_t seconds = time_t(milliseconds / 1000);
    struct tm time{};
    gmtime_r(&seconds, &time);
    char date[40];
    std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &time);
    char complete[56];
    std::snprintf(complete, sizeof(complete), "%s.%03dZ", date, int(milliseconds % 1000));
    return complete;
}
std::string json_text(const json& value, int indent = -1) {
    return value.dump(indent, ' ', false, json::error_handler_t::replace);
}
bool write_small(const std::string& path, const std::string& data) {
    const auto temporary = path + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < data.size()) {
        const auto count = ::write(fd, data.data() + done, data.size() - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ::close(fd); ::unlink(temporary.c_str()); return false; }
        done += std::size_t(count);
    }
    const bool closed = ::close(fd) == 0;
    return closed && ::rename(temporary.c_str(), path.c_str()) == 0;
}

json build_metadata() {
    return {{"application", "RBTV+"}, {"app_version", STREMIO_VERSION}, {"title_id", STREMIO_TITLE_ID},
            {"build_id", STREMIO_BUILD_ID}, {"compiler", __VERSION__},
#ifdef PLATFORM_PS5
            {"platform", "PS5 native"},
#else
            {"platform", "Linux host preview"},
#endif
            {"log_file_bytes", kFileBytes}, {"log_generations", kGenerations}, {"redaction", "enabled"}};
}
bool runtime_key(const std::string& key) {
    static const char* allowed[] = {
        "display", "video", "audio", "gpu", "network", "width", "height", "ui_width", "ui_height",
        "output_width", "output_height", "framebuffer_width", "framebuffer_height", "ui_resolution",
        "configured_width", "configured_height", "requested_width", "requested_height", "automatic",
        "gl_version", "gl_vendor", "gl_renderer", "gl_error", "renderer", "driver", "gpu_yuv", "gpu_status",
        "hardware_decoder", "decoder", "codec", "pixel_format", "bit_depth", "color_primaries", "color_transfer",
        "color_matrix", "color_range", "hdr_mode", "tone_mapping", "video_width", "video_height", "video_fps",
        "audio_codec", "sample_rate", "audio_channels", "addon_count", "enabled_addon_count", "server_configured",
        "network_state", "connection_type", "buffer_bytes", "buffer_seconds", "download_bytes_per_second",
        "fps", "frame_ms", "dropped_frames", "decoded_frames", "presented_frames", "rebuffer_count",
        "queue_depth", "audio_underruns", "decoder_errors", "http_status", "error_code", "state", "status",
        "hardware_decode_enabled", "output_mode", "vsync", "reduced_motion", "subtitle_language",
        "audio_language", "interface_language", "language", "log_io_failures", "log_dropped_events"
    };
    for (const char* name : allowed) if (key == name) return true;
    return false;
}
json safe_runtime(const json& source, int depth = 0) {
    json result = json::object();
    if (!source.is_object() || depth > 3) return result;
    unsigned count = 0;
    for (auto it = source.begin(); it != source.end() && count < 64; ++it) {
        if (!runtime_key(it.key())) continue;
        ++count;
        if (it.value().is_object()) result[it.key()] = safe_runtime(it.value(), depth + 1);
        else if (it.value().is_string()) result[it.key()] = diagnostics_redact(it.value().get<std::string>());
        else if (it.value().is_number() || it.value().is_boolean() || it.value().is_null()) result[it.key()] = it.value();
    }
    return result;
}
std::string safe_category(const std::string& value) {
    static const char* allowed[] = {"app", "http", "player", "video", "display", "lifecycle", "decoder", "audio", "subtitles", "addon", "account", "settings", "ui", "session", "diagnostics", "crash", "ffmpeg", "gpu"};
    for (const char* name : allowed) if (value == name) return value;
    return "app";
}

struct Event {
    std::uint64_t sequence = 0;
    std::int64_t time = 0;
    std::string category;
    std::string message;
};
struct LogFile {
    std::string stem, extension, directory;
    FILE* stream = nullptr;
    std::size_t bytes = 0;
    std::string path(int generation = 0) const {
        return directory + "/" + stem + (generation ? "." + std::to_string(generation) : "") + extension;
    }
    bool open() {
        const int fd = ::open(path().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd < 0) return false;
        stream = ::fdopen(fd, "a");
        if (!stream) { ::close(fd); return false; }
        ::setvbuf(stream, nullptr, _IOFBF, 64u * 1024u);
        struct stat info{};
        bytes = ::fstat(fd, &info) == 0 ? std::size_t(info.st_size) : 0;
        return true;
    }
    void close() { if (stream) ::fclose(stream); stream = nullptr; }
    bool put(const std::string& text) {
        if (!stream) return false;
        if (bytes && bytes + text.size() > kFileBytes) {
            close();
            if (::unlink(path(kGenerations - 1).c_str()) != 0 && errno != ENOENT) return false;
            for (int generation = kGenerations - 2; generation >= 0; --generation) {
                if (::rename(path(generation).c_str(), path(generation + 1).c_str()) != 0 && errno != ENOENT) return false;
            }
            if (!open()) return false;
        }
        const auto written = ::fwrite(text.data(), 1, text.size(), stream);
        bytes += written;
        return written == text.size();
    }
    bool flush() { return !stream || ::fflush(stream) == 0; }
};
struct State {
    std::mutex lifecycle;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread writer;
    std::vector<Event> queue;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> dropped_total{0};
    std::size_t queued_bytes = 0;
    std::uint64_t next_sequence = 0, flush_requested = 0, flush_done = 0;
    std::uint64_t io_failures = 0;
    bool stopping = false, runtime_dirty = false;
    std::string directory, session;
    json runtime = json::object();
    json marker = json::object();
    LogFile text, events;
};
State& state() {
    // Native-title static teardown is unreliable. This small control object
    // has process lifetime; explicit stop joins the worker and closes its files.
    static State* instance = new State;
    return *instance;
}

volatile sig_atomic_t crash_fd = -1;
volatile sig_atomic_t handling_crash = 0;
std::array<struct sigaction, 5> previous_actions{};
std::array<bool, 5> installed_actions{};
struct sigaction default_action{};
alignas(16) unsigned char crash_stack[64u * 1024u];
stack_t previous_stack{};
bool installed_stack = false;
char crash_header[256];
std::size_t crash_header_size = 0;

char* append_literal(char* out, const char* text) {
    while (*text) *out++ = *text++;
    return out;
}
char* append_hex(char* out, std::uintptr_t value) {
    static const char digits[] = "0123456789abcdef";
    out = append_literal(out, "0x");
    for (int nibble = int(sizeof(value) * 2) - 1; nibble >= 0; --nibble) *out++ = digits[(value >> (nibble * 4)) & 15u];
    return out;
}
void crash_handler(int signal_number, siginfo_t* info, void* context) {
    if (!handling_crash) {
        handling_crash = 1;
        char line[512];
        char* out = line;
        for (std::size_t i = 0; i < crash_header_size; ++i) *out++ = crash_header[i];
        out = append_literal(out, "signal=");
        out = append_hex(out, std::uintptr_t(signal_number));
        std::uintptr_t pc = 0, sp = 0;
#if defined(__linux__) && defined(__x86_64__)
        if (context) {
            const auto* machine = static_cast<const ucontext_t*>(context);
            pc = std::uintptr_t(machine->uc_mcontext.gregs[REG_RIP]);
            sp = std::uintptr_t(machine->uc_mcontext.gregs[REG_RSP]);
        }
#elif defined(PLATFORM_PS5) && defined(__x86_64__)
        if (context) {
            const auto* machine = static_cast<const ucontext_t*>(context);
            pc = std::uintptr_t(machine->uc_mcontext.mc_rip);
            sp = std::uintptr_t(machine->uc_mcontext.mc_rsp);
        }
#else
        (void)context;
#endif
        out = append_literal(out, " pc="); out = append_hex(out, pc);
        out = append_literal(out, " sp="); out = append_hex(out, sp);
        out = append_literal(out, " fault="); out = append_hex(out, info ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0);
        out = append_literal(out, " anchor="); out = append_hex(out, reinterpret_cast<std::uintptr_t>(&diagnostics_start));
        *out++ = '\n';
        const int fd = crash_fd;
        if (fd >= 0) {
            // Pre-opened descriptor, fixed stack buffer, async-signal-safe syscalls.
            (void)::write(fd, line, std::size_t(out - line));
            (void)::fsync(fd);
        }
    }
    // Preserve the platform's normal fatal-signal handling after saving the
    // small receipt. No allocation, logging framework, locks, or stdio here.
    (void)::sigaction(signal_number, &default_action, nullptr);
    (void)::kill(::getpid(), signal_number);
}

bool install_crash_capture(const std::string& directory, const std::string& session) {
    const auto current = directory + "/crash-current.txt";
    struct stat status{};
    if (::stat(current.c_str(), &status) == 0 && status.st_size > 0) (void)::rename(current.c_str(), (directory + "/crash-last.txt").c_str());
    crash_fd = ::open(current.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (crash_fd < 0) return false;
    handling_crash = 0;
    const int count = std::snprintf(crash_header, sizeof(crash_header), "RBTV+ %s (%s) build=%s session=%s\n",
                                    STREMIO_VERSION, STREMIO_TITLE_ID, STREMIO_BUILD_ID, session.c_str());
    crash_header_size = count > 0 ? std::min(std::size_t(count), sizeof(crash_header) - 1) : 0;
    default_action = {};
    default_action.sa_handler = SIG_DFL;
    ::sigemptyset(&default_action.sa_mask);
    stack_t stack{};
    stack.ss_sp = crash_stack;
    stack.ss_size = sizeof(crash_stack);
    installed_stack = ::sigaltstack(&stack, &previous_stack) == 0;
    struct sigaction action{};
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO | (installed_stack ? SA_ONSTACK : 0);
    ::sigemptyset(&action.sa_mask);
    bool complete = true;
    for (std::size_t i = 0; i < installed_actions.size(); ++i) {
        installed_actions[i] = ::sigaction(kSignals[i], &action, &previous_actions[i]) == 0;
        complete = complete && installed_actions[i];
    }
    return complete;
}
void close_crash_capture() {
    for (std::size_t i = 0; i < installed_actions.size(); ++i) if (installed_actions[i]) {
        (void)::sigaction(kSignals[i], &previous_actions[i], nullptr);
        installed_actions[i] = false;
    }
    if (installed_stack) { (void)::sigaltstack(&previous_stack, nullptr); installed_stack = false; }
    const int fd = crash_fd;
    crash_fd = -1;
    if (fd >= 0) ::close(fd);
}

void write_event(State& s, const Event& event) {
    const auto safe = diagnostics_redact(event.message);
    const auto stamp = utc_stamp(event.time);
    const auto category = safe_category(event.category);
    const std::string text = stamp + " [" + category + "] " + safe + "\n";
    const json structured = {{"utc", stamp}, {"sequence", event.sequence}, {"session", s.session}, {"category", category}, {"message", safe}};
    if (!s.text.put(text)) ++s.io_failures;
    if (!s.events.put(json_text(structured) + "\n")) ++s.io_failures;
#ifdef PLATFORM_PS5
    // Kernel output also runs on the writer and receives only redacted text.
    const std::string kernel = "[RBTV+] " + safe.substr(0, 1500) + "\n";
    sceKernelDebugOutText(0, kernel.c_str());
#else
    ::fwrite(text.data(), 1, text.size(), stdout);
#endif
}
void save_runtime(State& s, const json& runtime) {
    json result = build_metadata();
    result["session"] = s.session;
    result["updated_utc"] = utc_stamp(epoch_ms());
    result["runtime"] = runtime;
    result["log_dropped_events"] = s.dropped_total.load(std::memory_order_relaxed);
    result["log_io_failures"] = s.io_failures;
    if (!write_small(s.directory + "/runtime.json", json_text(result, 2) + "\n")) ++s.io_failures;
}
void writer_main() {
    State& s = state();
    // Reuse both vectors' storage between batches. An empty vector needs no
    // allocation, so even the writer's idle loop remains safe under pressure.
    std::vector<Event> batch;
    for (;;) {
        batch.clear();
        json runtime;
        bool update_runtime = false, finish = false;
        std::uint64_t flush_ticket = 0;
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            s.changed.wait_for(lock, std::chrono::milliseconds(250), [&] {
                return s.stopping || s.queue.size() >= 64 || s.runtime_dirty || s.flush_requested > s.flush_done;
            });
            batch.swap(s.queue);
            s.queued_bytes = 0;
            update_runtime = s.runtime_dirty;
            if (update_runtime) {
                try { runtime = s.runtime; }
                catch (...) { ++s.io_failures; update_runtime = false; }
                s.runtime_dirty = false;
            }
            flush_ticket = s.flush_requested;
            finish = s.stopping;
        }
        try {
            for (const auto& event : batch) write_event(s, event);
            const auto dropped = s.dropped.exchange(0, std::memory_order_relaxed);
            if (dropped) write_event(s, {0, epoch_ms(), "diagnostics", "Bounded writer queue dropped " + std::to_string(dropped) + " events; playback was not blocked."});
            if (!s.text.flush()) ++s.io_failures;
            if (!s.events.flush()) ++s.io_failures;
            if (update_runtime) save_runtime(s, runtime);
        } catch (...) {
            ++s.io_failures; // A diagnostics failure must not terminate playback.
        }
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.flush_done = std::max(s.flush_done, flush_ticket);
            finish = finish && s.queue.empty();
        }
        s.changed.notify_all();
        if (finish) break;
    }
    try {
        s.marker["clean_shutdown"] = true;
        s.marker["ended_utc"] = utc_stamp(epoch_ms());
        s.marker["log_dropped_events"] = s.dropped_total.load(std::memory_order_relaxed);
        s.marker["log_io_failures"] = s.io_failures;
        (void)write_small(s.directory + "/session.json", json_text(s.marker, 2) + "\n");
        save_runtime(s, s.runtime);
    } catch (...) {}
    s.text.close();
    s.events.close();
}
} // namespace

std::string diagnostics_redact(const std::string& message) {
    return redact_emails_and_tokens(redact_labels(redact_urls(unescape_for_redaction(message))));
}

bool diagnostics_start(const std::string& log_directory) {
    State& s = state();
    std::lock_guard<std::mutex> lifecycle(s.lifecycle);
    if (s.running.load(std::memory_order_acquire)) return true;
    if (log_directory.empty()) return false;
    try {
        s.directory = log_directory;
        while (s.directory.size() > 1 && s.directory.back() == '/') s.directory.pop_back();
        if (!make_dirs(s.directory)) return false;
        s.queue.clear(); s.queued_bytes = 0; s.next_sequence = 0; s.io_failures = 0;
        s.flush_requested = 0; s.flush_done = 0; s.stopping = false;
        s.dropped = 0; s.dropped_total = 0;
        s.runtime = json::object(); s.runtime_dirty = true;
        const auto start_time = epoch_ms();
        char identity[64];
        std::snprintf(identity, sizeof(identity), "%llx-%lx", static_cast<unsigned long long>(start_time), static_cast<unsigned long>(::getpid()));
        s.session = identity;
        bool previous_unclean = file_exists(s.directory + "/session.json");
        json previous;
        if (load_json(s.directory + "/session.json", previous) && previous.is_object()) {
            previous_unclean = !jbool(previous, "clean_shutdown", false);
            json safe_previous = {{"clean_shutdown", !previous_unclean}};
            for (const char* key : {"session", "started_utc", "ended_utc", "build_id", "title_id"}) {
                if (previous.contains(key) && previous[key].is_string()) safe_previous[key] = diagnostics_redact(previous[key].get<std::string>());
            }
            (void)write_small(s.directory + "/previous-session.json", json_text(safe_previous, 2) + "\n");
        }
        s.marker = build_metadata();
        s.marker["session"] = s.session;
        s.marker["started_utc"] = utc_stamp(start_time);
        s.marker["clean_shutdown"] = false;
        s.marker["previous_shutdown_unclean"] = previous_unclean;
        s.text = {"log", ".txt", s.directory, nullptr, 0};
        s.events = {"events", ".jsonl", s.directory, nullptr, 0};
        if (!s.text.open() || !s.events.open()) { s.text.close(); s.events.close(); return false; }
        s.marker["crash_capture_installed"] = install_crash_capture(s.directory, s.session);
        if (!write_small(s.directory + "/session.json", json_text(s.marker, 2) + "\n")) {
            close_crash_capture(); s.text.close(); s.events.close(); return false;
        }
        s.running.store(true, std::memory_order_release);
        s.writer = std::thread(writer_main);
        diagnostics_note("session", std::string("RBTV+ session started; previous shutdown ") + (previous_unclean ? "unclean (crash, forced close, or power loss)." : "clean or first launch."));
        return true;
    } catch (...) {
        s.running = false;
        if (s.writer.joinable()) {
            { std::lock_guard<std::mutex> lock(s.mutex); s.stopping = true; }
            s.changed.notify_all();
            s.writer.join();
        }
        close_crash_capture(); s.text.close(); s.events.close();
        return false;
    }
}

namespace {
void enqueue_note(const std::string& category, const std::string& message, bool critical) {
    State& s = state();
    if (!s.running.load(std::memory_order_acquire)) return;
    const auto bytes = std::min(message.size(), kMessageBytes);
    std::unique_lock<std::mutex> lock(s.mutex, std::defer_lock);
    // Ordinary messages never wait on the render/playback thread. A fatal
    // FFmpeg message is followed by abort(), so retain it even if the writer
    // happens to be swapping its queue at this instant.
    if (critical) lock.lock();
    else (void)lock.try_lock();
    if (critical) {
        while (!s.queue.empty() && (s.queue.size() >= kQueueEntries || s.queued_bytes + bytes > kQueueBytes)) {
            s.queued_bytes -= s.queue.back().message.size();
            s.queue.pop_back();
            s.dropped.fetch_add(1, std::memory_order_relaxed);
            s.dropped_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!lock.owns_lock() || s.queue.size() >= kQueueEntries || s.queued_bytes + bytes > kQueueBytes) {
        s.dropped.fetch_add(1, std::memory_order_relaxed);
        s.dropped_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!s.running.load(std::memory_order_relaxed)) return;
    try {
        s.queue.push_back({++s.next_sequence, epoch_ms(), safe_category(category), message.substr(0, kMessageBytes)});
        s.queued_bytes += bytes;
    } catch (...) {
        s.dropped.fetch_add(1, std::memory_order_relaxed);
        s.dropped_total.fetch_add(1, std::memory_order_relaxed);
    }
    const bool wake = critical || s.queue.size() >= 64;
    lock.unlock();
    if (wake) s.changed.notify_one();
}
} // namespace

void diagnostics_note(const std::string& category, const std::string& message) {
    enqueue_note(category, message, false);
}

void diagnostics_set_runtime(const json& context) {
    State& s = state();
    if (!s.running.load(std::memory_order_acquire)) return;
    try {
        json safe = safe_runtime(context);
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.running.load(std::memory_order_relaxed)) return;
        s.runtime.merge_patch(safe);
        s.runtime_dirty = true;
        s.changed.notify_one();
    } catch (...) {}
}

bool diagnostics_flush(unsigned timeout_ms) {
    State& s = state();
    if (!s.running.load(std::memory_order_acquire)) return true;
    std::unique_lock<std::mutex> lock(s.mutex);
    const auto ticket = ++s.flush_requested;
    s.changed.notify_one();
    return s.changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return s.flush_done >= ticket || !s.running.load(std::memory_order_relaxed); });
}

void diagnostics_stop() {
    State& s = state();
    std::lock_guard<std::mutex> lifecycle(s.lifecycle);
    if (!s.running.load(std::memory_order_acquire)) return;
    diagnostics_note("session", "Clean shutdown requested; draining the diagnostics writer.");
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.running.store(false, std::memory_order_release);
        s.stopping = true;
        ++s.flush_requested;
    }
    s.changed.notify_all();
    if (s.writer.joinable()) s.writer.join();
    close_crash_capture();
}

void diagnostics_ffmpeg_log(void* context, int level, const char* format, va_list arguments) {
    (void)context;
    if (level > 32 || !format) return;
    char message[kMessageBytes + 1];
    va_list copy;
    va_copy(copy, arguments);
    std::vsnprintf(message, sizeof(message), format, copy);
    va_end(copy);
    enqueue_note("ffmpeg", message, level <= 8);
    // FFmpeg's fatal assertions may abort immediately after this callback.
    // Drain only fatal/panic messages, on their failing worker, so the useful
    // cause is persisted before the minimal signal receipt is written.
    if (level <= 8) (void)diagnostics_flush(1000); // AV_LOG_FATAL, without an FFmpeg header dependency
}

extern "C" void stremio_diagnostics_ui_log(const char* message) {
    if (message) diagnostics_note("ui", message);
}
