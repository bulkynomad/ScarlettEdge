#define SCARLETTX_BUILDING_XCURL 1
#include "scarlettx/net/xcurl_abi.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#endif

namespace {

struct XCurlEasyHandle {
    std::string url;
    std::string method = "GET";
    bool custom_method = false;
    std::string user_agent;
    const char* raw_post_ptr = nullptr;
    bool has_copied_post = false;
    int64_t explicit_post_size = -1;
    std::vector<uint8_t> post_data;
    std::vector<std::string> headers;
    curl_write_callback write_cb = nullptr;
    void* write_data = nullptr;
    curl_write_callback header_cb = nullptr;
    void* header_data = nullptr;
    void* private_ptr = nullptr;
    char* error_buffer = nullptr;
    int64_t timeout_ms = 30000;
    int64_t response_code = 0;
    int64_t downloaded_bytes = 0;
    std::string content_type;
};

struct XCurlMockRule {
    std::string url_prefix;
    int64_t response_code = 200;
    std::string content_type = "application/json";
    std::vector<uint8_t> body;
};

struct XCurlMultiHandle {
    std::vector<CURL*> attached;
    std::vector<CURL*> pending;
    std::deque<CURLMsg> done;
    CURLMsg current{};
};

std::mutex g_mutex;
std::condition_variable g_multi_wake;
bool g_global_inited = false;
std::unordered_map<void*, std::shared_ptr<XCurlEasyHandle>> g_handles;
std::unordered_map<void*, std::unique_ptr<XCurlMultiHandle>> g_multis;
std::vector<XCurlMockRule> g_mocks;

bool XCurlLiveNetworkEnabled() {
    const char* v = std::getenv("SCARLETTX_LIVE_NETWORK");
    return v && v[0] == '1';
}

void WriteCurlError(char* err_buf, const char* msg) {
    if (!err_buf) return;
    if (!msg) {
        err_buf[0] = '\0';
        return;
    }
    std::strncpy(err_buf, msg, CURL_ERROR_SIZE - 1);
    err_buf[CURL_ERROR_SIZE - 1] = '\0';
}

bool StartsWithIgnoreCase(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

#if defined(_WIN32)
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return std::wstring(s.begin(), s.end());
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

std::string WideToUtf8(const wchar_t* ws, size_t wlen) {
    if (!ws || wlen == 0) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, ws, static_cast<int>(wlen), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, static_cast<int>(wlen), out.data(), needed, nullptr, nullptr);
    return out;
}

bool XCurlPerformWinHttp(
    const std::string& method,
    const std::string& url,
    const std::string& user_agent,
    const std::vector<std::string>& headers,
    const std::vector<uint8_t>& post_data,
    int64_t timeout_ms,
    int64_t& out_code,
    std::string& out_content_type,
    std::vector<std::string>& out_header_lines,
    std::vector<uint8_t>& out_body) {
    static HMODULE wh = LoadLibraryW(L"winhttp.dll");
    if (!wh) return false;
    auto fnOpen = reinterpret_cast<decltype(&WinHttpOpen)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpOpen")));
    auto fnCrack = reinterpret_cast<decltype(&WinHttpCrackUrl)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpCrackUrl")));
    auto fnConnect = reinterpret_cast<decltype(&WinHttpConnect)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpConnect")));
    auto fnOpenReq = reinterpret_cast<decltype(&WinHttpOpenRequest)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpOpenRequest")));
    auto fnSetTimeouts = reinterpret_cast<decltype(&WinHttpSetTimeouts)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpSetTimeouts")));
    auto fnSend = reinterpret_cast<decltype(&WinHttpSendRequest)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpSendRequest")));
    auto fnRecv = reinterpret_cast<decltype(&WinHttpReceiveResponse)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpReceiveResponse")));
    auto fnQueryHdr = reinterpret_cast<decltype(&WinHttpQueryHeaders)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpQueryHeaders")));
    auto fnQueryAvail = reinterpret_cast<decltype(&WinHttpQueryDataAvailable)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpQueryDataAvailable")));
    auto fnRead = reinterpret_cast<decltype(&WinHttpReadData)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpReadData")));
    auto fnClose = reinterpret_cast<decltype(&WinHttpCloseHandle)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpCloseHandle")));
    if (!fnOpen || !fnCrack || !fnConnect || !fnOpenReq || !fnSend || !fnRecv || !fnQueryHdr || !fnQueryAvail || !fnRead || !fnClose) {
        return false;
    }

    std::wstring wurl = Utf8ToWide(url);
    std::wstring wmethod = Utf8ToWide(method);
    std::wstring wua = Utf8ToWide(user_agent.empty() ? "ScarlettEdge-XCurl/1.0" : user_agent);
    std::string hdr_joined;
    for (const auto& hdr : headers) {
        hdr_joined += hdr;
        hdr_joined += "\r\n";
    }
    std::wstring wheaders = Utf8ToWide(hdr_joined);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!fnCrack(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) return false;

    HINTERNET hSession = fnOpen(wua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hSession) return false;
    if (fnSetTimeouts) {
        constexpr int64_t kMaxIntMs = static_cast<int64_t>((std::numeric_limits<int>::max)());
        const int ms = (timeout_ms <= 0) ? 15000
                                         : static_cast<int>(std::min<int64_t>(timeout_ms, kMaxIntMs));
        fnSetTimeouts(hSession, ms, ms, ms, ms);
    }
    HINTERNET hConnect = fnConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { fnClose(hSession); return false; }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = fnOpenReq(hConnect, wmethod.c_str(), path[0] ? path : L"/", nullptr, nullptr, nullptr, flags);
    if (!hReq) { fnClose(hConnect); fnClose(hSession); return false; }

    BOOL ok = fnSend(hReq,
                     wheaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wheaders.c_str(),
                     static_cast<DWORD>(wheaders.size()),
                     post_data.empty() ? nullptr : const_cast<uint8_t*>(post_data.data()),
                     static_cast<DWORD>(post_data.size()),
                     static_cast<DWORD>(post_data.size()), 0);
    if (ok) ok = fnRecv(hReq, nullptr);
    if (ok) {
        DWORD status = 0;
        DWORD status_sz = sizeof(status);
        fnQueryHdr(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &status_sz, nullptr);
        out_code = static_cast<int64_t>(status);

        out_header_lines.clear();
        out_content_type = "application/octet-stream";
        DWORD raw_hdr_bytes = 0;
        fnQueryHdr(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                   nullptr, &raw_hdr_bytes, WINHTTP_NO_HEADER_INDEX);
        if (raw_hdr_bytes > sizeof(wchar_t)) {
            std::vector<wchar_t> wbuf((raw_hdr_bytes / sizeof(wchar_t)) + 1, L'\0');
            if (fnQueryHdr(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                           wbuf.data(), &raw_hdr_bytes, WINHTTP_NO_HEADER_INDEX)) {
                const std::string raw = WideToUtf8(wbuf.data(), raw_hdr_bytes / sizeof(wchar_t));
                size_t pos = 0;
                while (pos < raw.size()) {
                    size_t eol = raw.find("\r\n", pos);
                    if (eol == std::string::npos) break;
                    std::string line = raw.substr(pos, eol - pos + 2);
                    pos = eol + 2;
                    out_header_lines.push_back(line);
                    if (StartsWithIgnoreCase(line, "Content-Type:")) {
                        size_t v = 13;
                        while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
                        std::string ct = line.substr(v);
                        while (!ct.empty() && (ct.back() == '\r' || ct.back() == '\n' || ct.back() == ' ')) {
                            ct.pop_back();
                        }
                        if (!ct.empty()) out_content_type = ct;
                    }
                }
            }
        }

        out_body.clear();
        for (;;) {
            DWORD avail = 0;
            if (!fnQueryAvail(hReq, &avail) || avail == 0) break;
            const size_t cur = out_body.size();
            out_body.resize(cur + avail);
            DWORD read = 0;
            if (!fnRead(hReq, out_body.data() + cur, avail, &read) || read == 0) {
                out_body.resize(cur);
                break;
            }
            out_body.resize(cur + read);
        }
    }
    fnClose(hReq);
    fnClose(hConnect);
    fnClose(hSession);
    return ok != FALSE;
}
#endif

// Caller holds g_mutex. Messages already handed out stay readable.
void DetachLocked(XCurlMultiHandle& m, CURL* curl) {
    m.attached.erase(std::remove(m.attached.begin(), m.attached.end(), curl), m.attached.end());
    m.pending.erase(std::remove(m.pending.begin(), m.pending.end(), curl), m.pending.end());
    m.done.erase(std::remove_if(m.done.begin(), m.done.end(),
                                [curl](const CURLMsg& msg) { return msg.easy_handle == curl; }),
                 m.done.end());
}

}  // namespace

extern "C" {

CURLcode curl_global_init(int64_t flags) {
    (void)flags;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_global_inited = true;
    return CURLE_OK;
}

void curl_global_cleanup(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handles.clear();
    g_multis.clear();
    g_global_inited = false;
    g_multi_wake.notify_all();
}

CURL* curl_easy_init(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto h = std::make_shared<XCurlEasyHandle>();
    void* raw = h.get();
    g_handles[raw] = std::move(h);
    return raw;
}

void curl_easy_cleanup(CURL* curl) {
    if (!curl) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& multi : g_multis) DetachLocked(*multi.second, curl);
    g_handles.erase(curl);
}

void curl_easy_reset(CURL* curl) {
    if (!curl) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_handles.find(curl);
    if (it != g_handles.end()) {
        *it->second = XCurlEasyHandle{};
    }
}

CURLcode curl_easy_setopt(CURL* curl, CURLoption option, ...) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_handles.find(curl);
    if (it == g_handles.end()) return CURLE_BAD_FUNCTION_ARGUMENT;
    auto& h = *it->second;

    va_list args;
    va_start(args, option);
    CURLcode rc = CURLE_OK;
    switch (option) {
        case CURLOPT_URL: {
            const char* s = va_arg(args, const char*);
            h.url = s ? s : "";
            break;
        }
        case CURLOPT_CUSTOMREQUEST: {
            const char* s = va_arg(args, const char*);
            if (s && *s) {
                h.method = s;
                h.custom_method = true;
            } else {
                h.custom_method = false;
            }
            break;
        }
        case CURLOPT_USERAGENT: {
            const char* s = va_arg(args, const char*);
            h.user_agent = s ? s : "";
            break;
        }
        case CURLOPT_HTTPGET: {
            const long v = va_arg(args, long);
            if (v) {
                if (!h.custom_method) h.method = "GET";
                h.raw_post_ptr = nullptr;
                h.has_copied_post = false;
                h.post_data.clear();
                h.explicit_post_size = -1;
            }
            break;
        }
        case CURLOPT_POST: {
            const long v = va_arg(args, long);
            if (v) {
                if (!h.custom_method) h.method = "POST";
            } else if (!h.custom_method) {
                h.method = "GET";
            }
            break;
        }
        case CURLOPT_POSTFIELDS: {
            // Do NOT call strlen(p) here: caller may set CURLOPT_POSTFIELDSIZE on a binary
            // buffer in the next curl_easy_setopt call! Length resolution is deferred to
            // curl_easy_perform when explicit_post_size < 0.
            const char* p = va_arg(args, const char*);
            h.raw_post_ptr = p;
            h.has_copied_post = false;
            h.post_data.clear();
            if (p && !h.custom_method && h.method == "GET") {
                h.method = "POST";
            }
            break;
        }
        case CURLOPT_COPYPOSTFIELDS: {
            const char* p = va_arg(args, const char*);
            h.raw_post_ptr = nullptr;
            h.has_copied_post = true;
            if (p) {
                const size_t len = (h.explicit_post_size >= 0)
                                       ? static_cast<size_t>(h.explicit_post_size)
                                       : std::strlen(p);
                h.post_data.assign(reinterpret_cast<const uint8_t*>(p),
                                   reinterpret_cast<const uint8_t*>(p) + len);
                if (!h.custom_method && h.method == "GET") {
                    h.method = "POST";
                }
            } else {
                h.post_data.clear();
            }
            break;
        }
        case CURLOPT_POSTFIELDSIZE: {
            const long sz = va_arg(args, long);
            h.explicit_post_size = (sz < 0) ? -1 : static_cast<int64_t>(sz);
            if (h.has_copied_post && sz >= 0 && static_cast<size_t>(sz) < h.post_data.size()) {
                h.post_data.resize(static_cast<size_t>(sz));
            }
            break;
        }
        case CURLOPT_POSTFIELDSIZE_LARGE: {
            const int64_t sz = va_arg(args, int64_t);
            h.explicit_post_size = (sz < 0) ? -1 : sz;
            if (h.has_copied_post && sz >= 0 && static_cast<size_t>(sz) < h.post_data.size()) {
                h.post_data.resize(static_cast<size_t>(sz));
            }
            break;
        }
        case CURLOPT_WRITEFUNCTION: {
            h.write_cb = va_arg(args, curl_write_callback);
            break;
        }
        case CURLOPT_WRITEDATA: {
            h.write_data = va_arg(args, void*);
            break;
        }
        case CURLOPT_HEADERFUNCTION: {
            h.header_cb = va_arg(args, curl_write_callback);
            break;
        }
        case CURLOPT_HEADERDATA: {
            h.header_data = va_arg(args, void*);
            break;
        }
        case CURLOPT_PRIVATE: {
            h.private_ptr = va_arg(args, void*);
            break;
        }
        case CURLOPT_ERRORBUFFER: {
            h.error_buffer = va_arg(args, char*);
            if (h.error_buffer) h.error_buffer[0] = '\0';
            break;
        }
        case CURLOPT_HTTPHEADER: {
            const curl_slist* list = va_arg(args, const curl_slist*);
            h.headers.clear();
            for (const curl_slist* cur = list; cur != nullptr; cur = cur->next) {
                if (cur->data) h.headers.emplace_back(cur->data);
            }
            break;
        }
        case CURLOPT_TIMEOUT: {
            const long sec = va_arg(args, long);
            constexpr long kMaxSec = 2147483L;
            if (sec <= 0) {
                h.timeout_ms = 0;
            } else if (sec > kMaxSec) {
                h.timeout_ms = static_cast<int64_t>(kMaxSec) * 1000LL;
            } else {
                h.timeout_ms = static_cast<int64_t>(sec) * 1000LL;
            }
            break;
        }
        case CURLOPT_TIMEOUT_MS: {
            const long ms = va_arg(args, long);
            h.timeout_ms = (ms <= 0) ? 0 : static_cast<int64_t>(ms);
            break;
        }
        case CURLOPT_FOLLOWLOCATION:
        case CURLOPT_SSL_VERIFYPEER: {
            (void)va_arg(args, long);
            break;
        }
        default:
            break;
    }
    va_end(args);
    return rc;
}

CURLcode curl_easy_perform(CURL* curl) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;

    std::shared_ptr<XCurlEasyHandle> h_sp;
    std::string req_method;
    std::string req_url;
    std::string req_ua;
    std::vector<std::string> req_headers;
    std::vector<uint8_t> req_post;
    int64_t req_timeout_ms = 30000;
    curl_write_callback write_cb = nullptr;
    void* write_ud = nullptr;
    curl_write_callback header_cb = nullptr;
    void* header_ud = nullptr;
    char* err_buf = nullptr;

    std::vector<uint8_t> body;
    std::vector<std::string> header_lines;
    bool matched_mock_or_offline = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handles.find(curl);
        if (it == g_handles.end()) return CURLE_BAD_FUNCTION_ARGUMENT;
        h_sp = it->second;
        auto& h = *h_sp;
        err_buf = h.error_buffer;
        WriteCurlError(err_buf, "");

        if (h.url.empty()) {
            WriteCurlError(err_buf, "URL using bad/illegal format or missing URL");
            return CURLE_URL_MALFORMAT;
        }

        // Resolve POST payload length now that both POSTFIELDS and POSTFIELDSIZE have been set.
        if (h.has_copied_post) {
            req_post = h.post_data;
        } else if (h.raw_post_ptr != nullptr) {
            const size_t len = (h.explicit_post_size >= 0)
                                   ? static_cast<size_t>(h.explicit_post_size)
                                   : std::strlen(h.raw_post_ptr);
            req_post.assign(reinterpret_cast<const uint8_t*>(h.raw_post_ptr),
                            reinterpret_cast<const uint8_t*>(h.raw_post_ptr) + len);
        }

        write_cb = h.write_cb;
        write_ud = h.write_data;
        header_cb = h.header_cb;
        header_ud = h.header_data;

        for (const auto& rule : g_mocks) {
            if (h.url.rfind(rule.url_prefix, 0) == 0) {
                h.response_code = rule.response_code;
                h.content_type = rule.content_type;
                body = rule.body;
                h.downloaded_bytes = static_cast<int64_t>(body.size());
                char status_line[64]{};
                std::snprintf(status_line, sizeof(status_line), "HTTP/1.1 %lld OK\r\n",
                              static_cast<long long>(rule.response_code));
                header_lines.emplace_back(status_line);
                header_lines.push_back("Content-Type: " + rule.content_type + "\r\n");
                header_lines.emplace_back("\r\n");
                matched_mock_or_offline = true;
                break;
            }
        }

        if (!matched_mock_or_offline) {
            if (XCurlLiveNetworkEnabled()) {
                req_method = h.method;
                req_url = h.url;
                req_ua = h.user_agent;
                req_headers = h.headers;
                req_timeout_ms = h.timeout_ms;
            } else {
                h.response_code = 503;
                h.content_type = "application/json";
                const std::string fallback = "{\"status\":\"offline\",\"emulator\":\"ScarlettX-XCurl\"}";
                body.assign(fallback.begin(), fallback.end());
                h.downloaded_bytes = static_cast<int64_t>(body.size());
                header_lines.emplace_back("HTTP/1.1 503 Service Unavailable\r\n");
                header_lines.emplace_back("Content-Type: application/json\r\n");
                header_lines.emplace_back("\r\n");
                matched_mock_or_offline = true;
            }
        }
    }

    if (!matched_mock_or_offline) {
#if defined(_WIN32)
        // Perform live WinHTTP I/O WITHOUT holding g_mutex!
        int64_t live_code = 0;
        std::string live_ct;
        std::vector<std::string> live_hdrs;
        std::vector<uint8_t> live_body;
        if (!XCurlPerformWinHttp(req_method, req_url, req_ua, req_headers, req_post,
                                 req_timeout_ms, live_code, live_ct, live_hdrs, live_body)) {
            std::lock_guard<std::mutex> lock(g_mutex);
            h_sp->response_code = 0;
            h_sp->downloaded_bytes = 0;
            WriteCurlError(h_sp->error_buffer, curl_easy_strerror(CURLE_COULDNT_CONNECT));
            return CURLE_COULDNT_CONNECT;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            h_sp->response_code = live_code;
            h_sp->content_type = std::move(live_ct);
            h_sp->downloaded_bytes = static_cast<int64_t>(live_body.size());
            write_cb = h_sp->write_cb;
            write_ud = h_sp->write_data;
            header_cb = h_sp->header_cb;
            header_ud = h_sp->header_data;
            err_buf = h_sp->error_buffer;
        }
        body = std::move(live_body);
        header_lines = std::move(live_hdrs);
        if (header_lines.empty() || header_lines.back() != "\r\n") {
            header_lines.emplace_back("\r\n");
        }
#else
        std::lock_guard<std::mutex> lock(g_mutex);
        h_sp->response_code = 0;
        h_sp->downloaded_bytes = 0;
        WriteCurlError(h_sp->error_buffer, curl_easy_strerror(CURLE_COULDNT_CONNECT));
        return CURLE_COULDNT_CONNECT;
#endif
    }

    if (header_cb) {
        for (std::string& line : header_lines) {
            const size_t written = header_cb(line.data(), 1, line.size(), header_ud);
            if (written != line.size()) {
                WriteCurlError(err_buf, curl_easy_strerror(CURLE_WRITE_ERROR));
                return CURLE_WRITE_ERROR;
            }
        }
    }

    if (write_cb && !body.empty()) {
        const size_t written = write_cb(reinterpret_cast<char*>(body.data()), 1, body.size(), write_ud);
        if (written != body.size()) {
            WriteCurlError(err_buf, curl_easy_strerror(CURLE_WRITE_ERROR));
            return CURLE_WRITE_ERROR;
        }
    }
    return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL* curl, CURLINFO info, ...) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_handles.find(curl);
    if (it == g_handles.end()) return CURLE_BAD_FUNCTION_ARGUMENT;
    const auto& h = *it->second;

    va_list args;
    va_start(args, info);
    CURLcode rc = CURLE_OK;
    switch (info) {
        case CURLINFO_RESPONSE_CODE: {
            long* out = va_arg(args, long*);
            if (out) *out = static_cast<long>(h.response_code);
            break;
        }
        case CURLINFO_SIZE_DOWNLOAD_T: {
            int64_t* out = va_arg(args, int64_t*);
            if (out) *out = h.downloaded_bytes;
            break;
        }
        case CURLINFO_CONTENT_TYPE: {
            const char** out = va_arg(args, const char**);
            if (out) *out = h.content_type.c_str();
            break;
        }
        case CURLINFO_PRIVATE: {
            void** out = va_arg(args, void**);
            if (out) *out = h.private_ptr;
            break;
        }
        default:
            rc = CURLE_BAD_FUNCTION_ARGUMENT;
            break;
    }
    va_end(args);
    return rc;
}

const char* curl_easy_strerror(CURLcode code) {
    switch (code) {
        case CURLE_OK: return "No error";
        case CURLE_UNSUPPORTED_PROTOCOL: return "Unsupported protocol";
        case CURLE_URL_MALFORMAT: return "URL using bad/illegal format";
        case CURLE_COULDNT_CONNECT: return "Couldn't connect to server";
        case CURLE_WRITE_ERROR: return "Failed writing received data";
        case CURLE_OPERATION_TIMEDOUT: return "Timeout was reached";
        case CURLE_BAD_FUNCTION_ARGUMENT: return "Bad function argument";
        default: return "Unknown error";
    }
}

curl_slist* curl_slist_append(curl_slist* list, const char* string) {
    if (!string) return list;
    auto* node = static_cast<curl_slist*>(std::malloc(sizeof(curl_slist)));
    if (!node) return nullptr;
    const size_t len = std::strlen(string);
    node->data = static_cast<char*>(std::malloc(len + 1));
    if (!node->data) {
        std::free(node);
        return nullptr;
    }
    std::memcpy(node->data, string, len + 1);
    node->next = nullptr;
    if (!list) return node;
    curl_slist* tail = list;
    while (tail->next) tail = tail->next;
    tail->next = node;
    return list;
}

void curl_slist_free_all(curl_slist* list) {
    while (list) {
        curl_slist* next = list->next;
        std::free(list->data);
        std::free(list);
        list = next;
    }
}

const char* curl_version(void) {
    return "ScarlettX-XCurl/8.0.0-HLE";
}

CURLM* curl_multi_init(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto m = std::make_unique<XCurlMultiHandle>();
    void* raw = m.get();
    g_multis[raw] = std::move(m);
    return raw;
}

CURLMcode curl_multi_cleanup(CURLM* multi) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_multis.erase(multi) == 0) return CURLM_BAD_HANDLE;
    g_multi_wake.notify_all();
    return CURLM_OK;
}

CURLMcode curl_multi_add_handle(CURLM* multi, CURL* curl) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_multis.find(multi);
    if (it == g_multis.end()) return CURLM_BAD_HANDLE;
    if (g_handles.find(curl) == g_handles.end()) return CURLM_BAD_EASY_HANDLE;
    for (const auto& other : g_multis) {
        const auto& attached = other.second->attached;
        if (std::find(attached.begin(), attached.end(), curl) != attached.end()) return CURLM_ADDED_ALREADY;
    }
    it->second->attached.push_back(curl);
    it->second->pending.push_back(curl);
    g_multi_wake.notify_all();
    return CURLM_OK;
}

CURLMcode curl_multi_remove_handle(CURLM* multi, CURL* curl) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_multis.find(multi);
    if (it == g_multis.end()) return CURLM_BAD_HANDLE;
    if (g_handles.find(curl) == g_handles.end()) return CURLM_BAD_EASY_HANDLE;
    DetachLocked(*it->second, curl);
    return CURLM_OK;
}

CURLMcode curl_multi_perform(CURLM* multi, int* running_handles) {
    std::vector<CURL*> batch;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_multis.find(multi);
        if (it == g_multis.end()) return CURLM_BAD_HANDLE;
        batch.swap(it->second->pending);
    }
    for (CURL* curl : batch) {
        const CURLcode result = curl_easy_perform(curl);
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_multis.find(multi);
        if (it == g_multis.end()) return CURLM_BAD_HANDLE;
        const auto& attached = it->second->attached;
        if (std::find(attached.begin(), attached.end(), curl) == attached.end()) continue;
        CURLMsg msg{};
        msg.msg = CURLMSG_DONE;
        msg.easy_handle = curl;
        msg.data.result = result;
        it->second->done.push_back(msg);
    }
    if (running_handles) *running_handles = 0;
    return CURLM_OK;
}

CURLMsg* curl_multi_info_read(CURLM* multi, int* msgs_in_queue) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_multis.find(multi);
    if (it == g_multis.end() || it->second->done.empty()) {
        if (msgs_in_queue) *msgs_in_queue = 0;
        return nullptr;
    }
    XCurlMultiHandle& m = *it->second;
    m.current = m.done.front();
    m.done.pop_front();
    if (msgs_in_queue) *msgs_in_queue = static_cast<int>(m.done.size());
    return &m.current;
}

CURLMcode curl_multi_poll(
    CURLM* multi,
    struct curl_waitfd* /*extra_fds*/,
    unsigned int /*extra_nfds*/,
    int timeout_ms,
    int* numfds) {
    if (numfds) *numfds = 0;
    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_multis.find(multi) == g_multis.end()) return CURLM_BAD_HANDLE;
    if (timeout_ms > 0) {
        g_multi_wake.wait_for(lock, std::chrono::milliseconds(timeout_ms), [multi] {
            auto it = g_multis.find(multi);
            return it == g_multis.end() || !it->second->pending.empty();
        });
    }
    return CURLM_OK;
}

CURLcode xcurl_register_mock_response(
    const char* url_prefix,
    int64_t response_code,
    const char* content_type,
    const void* body,
    size_t body_size) {
    if (!url_prefix) return CURLE_BAD_FUNCTION_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_mutex);
    XCurlMockRule rule{};
    rule.url_prefix = url_prefix;
    rule.response_code = response_code;
    rule.content_type = content_type ? content_type : "application/octet-stream";
    if (body && body_size > 0) {
        const auto* b = static_cast<const uint8_t*>(body);
        rule.body.assign(b, b + body_size);
    }
    g_mocks.push_back(std::move(rule));
    return CURLE_OK;
}

void xcurl_clear_mocks(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mocks.clear();
}

}  // extern "C"
