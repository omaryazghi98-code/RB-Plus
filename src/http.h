#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct HttpResponse {
	long status = 0;       // HTTP status, 0 when the request never got an answer
	std::string body;
	std::string error;     // transport error (DNS, TLS, timeout, ...)
	std::string content_type;
	std::string cache_control, etag, retry_after, content_range, content_encoding;
	bool ok() const { return error.empty() && status >= 200 && status < 300; }
	std::string describe() const;  // "HTTP 404" / "Could not resolve host: ..."
};

void http_init(const std::string& ca_bundle_path);

// Blocking requests, for worker threads. `cancel` may be flipped to abort.
HttpResponse http_get(const std::string& url, long timeout_s = 20, const std::atomic<bool>* cancel = nullptr,
                      const std::vector<std::string>& headers = {}, size_t max_body_bytes = 64u * 1024 * 1024);
HttpResponse http_post_json(const std::string& url, const std::string& body, long timeout_s = 20,
                            const std::atomic<bool>* cancel = nullptr, size_t max_body_bytes = 64u * 1024 * 1024);

bool http_valid_url(const std::string& url);
// URL paths and queries can contain addon, Debrid and account credentials.
std::string http_log_target(const std::string& url);

// OpenSubtitles' file hash is NOT the torrent infoHash. The pure helper also
// works with blocks read from the native torrent engine. Returns empty unless
// both 64-KiB blocks and a file size of at least 128 KiB are available.
std::string opensubtitles_hash(const std::string& first, const std::string& last, uint64_t file_size);
// Worker-thread helper; two bounded HTTP ranges, never a whole-file download.
// file_size may be known even if the tail request/hash fails.
bool http_opensubtitles_hash(const std::string& url, const std::vector<std::string>& headers,
                            std::string& hash, uint64_t& file_size, std::string& error,
                            const std::atomic<bool>* cancel = nullptr, long timeout_s = 8);

const std::string& http_ca_bundle();
// What every curl handle needs on this platform (a CURL*). In the native PS5
// app: sockets made non-blocking with the console's own option.
void http_setup_handle(void* curl);
extern const char* kUserAgent;
