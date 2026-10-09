#include "http.h"

#include <curl/curl.h>
#include <algorithm>
#include <limits>

#include "util.h"

const char* kUserAgent = "Mozilla/5.0 (PlayStation; PlayStation 5/1.00) Stremio-PS5/1.0";

static std::string g_ca_bundle;

std::string HttpResponse::describe() const {
	if (!error.empty()) return error;
	if (status == 0) return "no response";
	return "HTTP " + std::to_string(status);
}

void http_init(const std::string& ca_bundle_path) {
	curl_global_init(CURL_GLOBAL_ALL);
	g_ca_bundle = ca_bundle_path;
	dlog("curl %s, CA bundle %s (%s)", curl_version(), ca_bundle_path.c_str(),
	     file_exists(ca_bundle_path) ? "found" : "MISSING");
}

const std::string& http_ca_bundle() { return g_ca_bundle; }

#ifdef PLATFORM_PS5_NATIVE
extern "C" int console_curl_nonblocking(int socket);  // native/console_curl.c

static int on_socket(void*, curl_socket_t s, curlsocktype) {
	console_curl_nonblocking(s);  // without it a finished request can hang
	return CURL_SOCKOPT_OK;
}
#endif

void http_setup_handle(void* curl) {
#ifdef PLATFORM_PS5_NATIVE
	curl_easy_setopt(static_cast<CURL*>(curl), CURLOPT_SOCKOPTFUNCTION, on_socket);
#else
	(void)curl;
#endif
}

bool http_valid_url(const std::string& url) {
	const auto scheme_end = url.find("://");
	if (scheme_end == std::string::npos || url.size() > 64u * 1024) return false;
	const auto scheme = lower(url.substr(0, scheme_end));
	if (scheme != "http" && scheme != "https") return false;
	for (size_t i = 0; i < url.size(); ++i) {
		unsigned char c = url[i];
		if (c <= 32 || c == 127 || c == '\\') return false;
		if (c == '%') {
			auto hex = [](char d) { return (d >= '0' && d <= '9') || (d >= 'a' && d <= 'f') || (d >= 'A' && d <= 'F'); };
			if (i + 2 >= url.size() || !hex(url[i + 1]) || !hex(url[i + 2])) return false;
			i += 2;
		}
	}
	const auto authority_start = scheme_end + 3;
	const auto authority_end = url.find_first_of("/?#", authority_start);
	std::string authority = url.substr(authority_start, authority_end == std::string::npos ? authority_end : authority_end - authority_start);
	const auto userinfo = authority.rfind('@');
	if (userinfo != std::string::npos) authority.erase(0, userinfo + 1);
	if (authority.empty()) return false;
	std::string port;
	if (authority.front() == '[') {
		const auto bracket = authority.find(']');
		if (bracket == std::string::npos || bracket == 1) return false;
		if (bracket + 1 < authority.size()) {
			if (authority[bracket + 1] != ':') return false;
			port = authority.substr(bracket + 2);
			if (port.empty()) return false;
		}
	} else {
		const auto colon = authority.find(':');
		if (colon != std::string::npos) {
			if (colon == 0 || authority.find(':', colon + 1) != std::string::npos) return false;
			port = authority.substr(colon + 1);
			if (port.empty()) return false;
			authority.resize(colon);
		}
		if (authority.find_first_of("[]") != std::string::npos) return false;
	}
	if (!port.empty()) {
		unsigned number = 0;
		for (char c : port) {
			if (c < '0' || c > '9' || number > 6553) return false;
			number = number * 10 + (c - '0');
		}
		if (number == 0 || number > 65535) return false;
	}
	return true;
}

std::string http_log_target(const std::string& url) {
	if (!http_valid_url(url)) return "[invalid URL]";
	auto start = url.find("://") + 3;
	auto end = url.find_first_of("/?#", start);
	std::string authority = url.substr(start, end == std::string::npos ? end : end - start);
	auto at = authority.rfind('@');
	if (at != std::string::npos) authority.erase(0, at + 1);
	return url.substr(0, start) + authority + "/[redacted]";
}

struct TransferBuffer {
	HttpResponse* response;
	size_t limit;
	size_t header_bytes = 0;
	bool body_limit_hit = false, header_limit_hit = false, allocation_failed = false;
};

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
	auto* buffer = static_cast<TransferBuffer*>(userdata);
	auto& out = buffer->response->body;
	if ((size && nmemb > std::numeric_limits<size_t>::max() / size) ||
	    size * nmemb > buffer->limit - std::min(buffer->limit, out.size())) {
		buffer->body_limit_hit = true;
		return 0;
	}
	const size_t bytes = size * nmemb;
	try { out.append(ptr, bytes); }
	catch (...) { buffer->allocation_failed = true; return 0; }
	return bytes;
}

static size_t header_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
	auto* buffer = static_cast<TransferBuffer*>(userdata);
	constexpr size_t max_headers = 128u * 1024;
	if ((size && nmemb > std::numeric_limits<size_t>::max() / size) ||
	    size * nmemb > max_headers - std::min(max_headers, buffer->header_bytes)) {
		buffer->header_limit_hit = true;
		return 0;
	}
	const size_t bytes = size * nmemb;
	buffer->header_bytes += bytes;
	try {
		std::string line(ptr, bytes);
		auto& response = *buffer->response;
		if (starts_with(line, "HTTP/")) {
			// A redirect/interim response is not the final JSON or artwork body.
			response.body.clear();
			response.cache_control.clear();
			response.etag.clear();
			response.retry_after.clear();
			response.content_range.clear();
			response.content_encoding.clear();
			response.rb_session.clear();
		} else {
			auto colon = line.find(':');
			if (colon != std::string::npos) {
				const auto name = lower(trim(line.substr(0, colon)));
				const auto value = trim(line.substr(colon + 1));
				if (name == "etag") response.etag = value;
				else if (name == "retry-after") response.retry_after = value;
				else if (name == "content-range") response.content_range = value;
				else if (name == "content-encoding") response.content_encoding = lower(value);
				else if (name == "rb-session") response.rb_session = value;
				else if (name == "cache-control") {
					if (!response.cache_control.empty()) response.cache_control += ", ";
					response.cache_control += value;
				}
			}
		}
	} catch (...) { buffer->allocation_failed = true; return 0; }
	return bytes;
}

static bool valid_request_header(const std::string& header) {
	const auto colon = header.find(':');
	if (colon == std::string::npos || colon == 0 || colon > 256 || header.size() > 64u * 1024) return false;
	for (size_t i = 0; i < header.size(); ++i) {
		unsigned char c = header[i];
		if ((c < 32 && (c != '\t' || i < colon)) || c == 127) return false;
		if (i < colon && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || std::string("!#$%&'*+-.^_`|~").find(c) != std::string::npos)) return false;
	}
	return true;
}

static int progress_cb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	auto* cancel = static_cast<const std::atomic<bool>*>(clientp);
	return (cancel && cancel->load()) ? 1 : 0;
}

static HttpResponse perform(const std::string& url, const std::string* post_body, long timeout_s,
                            const std::atomic<bool>* cancel, const std::vector<std::string>& headers,
                            size_t max_body_bytes) {
	HttpResponse r;
	if (!http_valid_url(url)) {
		r.error = "invalid or unsupported HTTP(S) URL";
		return r;
	}
	if (cancel && cancel->load()) { r.error = "cancelled"; return r; }
	for (const auto& header : headers)
		if (!valid_request_header(header)) { r.error = "invalid HTTP request header"; return r; }
	CURL* c = curl_easy_init();
	if (!c) {
		r.error = "curl_easy_init failed";
		return r;
	}
	char errbuf[CURL_ERROR_SIZE] = {0};
	TransferBuffer buffer{&r, max_body_bytes};
	struct curl_slist* hdrs = nullptr;
	auto add_header = [&](const char* value) {
		auto* next = curl_slist_append(hdrs, value);
		if (!next) return false;
		hdrs = next;
		return true;
	};
	bool headers_ok = true;
	for (const auto& h : headers) headers_ok = headers_ok && add_header(h.c_str());
	if (post_body) headers_ok = headers_ok && add_header("Content-Type: application/json");
	if (!headers_ok) {
		r.error = "could not allocate HTTP headers";
		if (hdrs) curl_slist_free_all(hdrs);
		curl_easy_cleanup(c);
		return r;
	}

	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
	curl_easy_setopt(c, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");  // whatever curl was built with (gzip, br)
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, std::max(timeout_s, 1L));
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	// The PS5 has no IPv6 route; don't let an AAAA record fail the connect.
	curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, &buffer);
	curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(std::min<uint64_t>(
	    max_body_bytes, static_cast<uint64_t>(std::numeric_limits<curl_off_t>::max()))));
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
	if (!g_ca_bundle.empty() && file_exists(g_ca_bundle)) curl_easy_setopt(c, CURLOPT_CAINFO, g_ca_bundle.c_str());
	http_setup_handle(c);
	if (cancel) {
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
		curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void*)cancel);
	}
	if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
	if (post_body) {
		curl_easy_setopt(c, CURLOPT_POST, 1L);
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_body->c_str());
		curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, long(post_body->size()));
	}

	CURLcode rc = curl_easy_perform(c);
	if (rc != CURLE_OK) {
		// Curl's verbose error can echo a URL containing private configuration.
		r.error = curl_easy_strerror(rc);
		if (buffer.body_limit_hit || rc == CURLE_FILESIZE_EXCEEDED) r.error = "HTTP response exceeds size limit";
		else if (buffer.header_limit_hit) r.error = "HTTP response headers exceed size limit";
		else if (buffer.allocation_failed) r.error = "not enough memory for HTTP response";
		if (cancel && cancel->load()) r.error = "cancelled";
		char* ip = nullptr;
		curl_easy_getinfo(c, CURLINFO_PRIMARY_IP, &ip);
		dlog("http: %s %s -> %s (ip %s)", post_body ? "POST" : "GET", http_log_target(url).c_str(), r.error.c_str(),
		     ip && *ip ? ip : "none");
	}
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
	char* ct = nullptr;
	if (curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct) r.content_type = ct;
	if (hdrs) curl_slist_free_all(hdrs);
	curl_easy_cleanup(c);
	return r;
}

HttpResponse http_get(const std::string& url, long timeout_s, const std::atomic<bool>* cancel,
                      const std::vector<std::string>& headers, size_t max_body_bytes) {
	return perform(url, nullptr, timeout_s, cancel, headers, max_body_bytes);
}

HttpResponse http_post_json(const std::string& url, const std::string& body, long timeout_s,
                            const std::atomic<bool>* cancel, size_t max_body_bytes) {
	return perform(url, &body, timeout_s, cancel, {}, max_body_bytes);
}

std::string opensubtitles_hash(const std::string& first, const std::string& last, uint64_t file_size) {
	constexpr size_t block_bytes = 65536;
	if (first.size() != block_bytes || last.size() != block_bytes || file_size < 2 * block_bytes) return "";
	uint64_t hash = file_size;
	for (const auto* block : {&first, &last}) {
		for (size_t i = 0; i < block_bytes; i += 8) {
			uint64_t word = 0;
			for (size_t byte = 0; byte < 8; ++byte)
				word |= static_cast<uint64_t>(static_cast<unsigned char>((*block)[i + byte])) << (8 * byte);
			hash += word; // specified modulo-2^64 addition
		}
	}
	static const char* hex = "0123456789abcdef";
	std::string result(16, '0');
	for (int i = 15; i >= 0; --i) { result[static_cast<size_t>(i)] = hex[hash & 15]; hash >>= 4; }
	return result;
}

static bool range_numbers(const std::string& value, uint64_t& first, uint64_t& last, uint64_t& total) {
	if (!starts_with(lower(value), "bytes ")) return false;
	const auto dash = value.find('-', 6), slash = value.find('/', 6);
	if (dash == std::string::npos || slash == std::string::npos || dash >= slash) return false;
	auto number = [](const std::string& text, uint64_t& result) {
		if (text.empty()) return false;
		result = 0;
		for (char c : text) {
			if (c < '0' || c > '9' || result > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10) return false;
			result = result * 10 + (c - '0');
		}
		return true;
	};
	return number(value.substr(6, dash - 6), first) && number(value.substr(dash + 1, slash - dash - 1), last) &&
	       number(value.substr(slash + 1), total) && first <= last && last < total;
}

bool http_opensubtitles_hash(const std::string& url, const std::vector<std::string>& headers,
                            std::string& hash, uint64_t& file_size, std::string& error,
                            const std::atomic<bool>* cancel, long timeout_s) {
	hash.clear();
	file_size = 0;
	error.clear();
	std::vector<std::string> range_headers;
	for (const auto& header : headers) {
		const auto name = lower(trim(header.substr(0, header.find(':'))));
		if (name != "range" && name != "accept-encoding" && name != "if-range") range_headers.push_back(header);
	}
	range_headers.push_back("Accept-Encoding: identity");
	range_headers.push_back("Range: bytes=0-65535");
	const auto head = http_get(url, timeout_s, cancel, range_headers, 65536);
	uint64_t first = 0, last = 0, total = 0;
	if (!head.ok() || head.status != 206 || head.body.size() != 65536 ||
	    (!head.content_encoding.empty() && head.content_encoding != "identity") ||
	    !range_numbers(head.content_range, first, last, total) || first != 0 || last != 65535 || total < 131072) {
		error = head.ok() ? "source does not provide exact byte ranges for subtitle matching" : head.describe();
		return false;
	}
	file_size = total;
	range_headers.back() = "Range: bytes=" + std::to_string(total - 65536) + "-" + std::to_string(total - 1);
	// A changing representation must not mix the head of one file and tail
	// of another. Weak ETags cannot be used for If-Range.
	if (!head.etag.empty() && head.etag.front() == '"') range_headers.push_back("If-Range: " + head.etag);
	const auto tail = http_get(url, timeout_s, cancel, range_headers, 65536);
	uint64_t tail_total = 0;
	if (!tail.ok() || tail.status != 206 || tail.body.size() != 65536 ||
	    (!tail.content_encoding.empty() && tail.content_encoding != "identity") ||
	    !range_numbers(tail.content_range, first, last, tail_total) || tail_total != total ||
	    first != total - 65536 || last != total - 1 ||
	    (!head.etag.empty() && !tail.etag.empty() && head.etag != tail.etag)) {
		error = tail.ok() ? "source byte ranges changed during subtitle matching" : tail.describe();
		return false;
	}
	hash = opensubtitles_hash(head.body, tail.body, total);
	return !hash.empty();
}
