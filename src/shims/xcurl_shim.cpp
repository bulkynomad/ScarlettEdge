#define SCARLETTX_BUILDING_XCURL 1
#include "scarlettx/net/xcurl_abi.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <deque>
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
    std::string user_agent;
    std::vector<uint8_t> post_data;
    curl_write_callback write_cb = nullptr;
    void* write_data = nullptr;
    int64_t timeout_sec = 30;
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

// There is no network behind this shim, so a transfer runs to completion
// inside curl_multi_perform, exactly as curl_easy_perform does it.
struct XCurlMultiHandle {
    std::vector<CURL*> attached;
    std::vector<CURL*> pending;
    std::deque<CURLMsg> done;
    CURLMsg current{};
};

std::mutex g_mutex;
std::condition_variable g_multi_wake;
bool g_global_inited = false;
std::unordered_map<void*, std::unique_ptr<XCurlEasyHandle>> g_handles;
std::unordered_map<void*, std::unique_ptr<XCurlMultiHandle>> g_multis;
std::vector<XCurlMockRule> g_mocks;

bool XCurlLiveNetworkEnabled() {
    const char* v = std::getenv("SCARLETTX_LIVE_NETWORK");
    return v && v[0] == '1';
}

#if defined(_WIN32)
bool XCurlPerformWinHttp(
    const std::string& method,
    const std::string& url,
    const std::vector<uint8_t>& post_data,
    int64_t timeout_sec,
    int64_t& out_code,
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

    std::wstring wurl(url.begin(), url.end());
    std::wstring wmethod(method.begin(), method.end());
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!fnCrack(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) return false;

    HINTERNET hSession = fnOpen(L"ScarlettEdge-XCurl/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hSession) return false;
    if (fnSetTimeouts) {
        const int ms = static_cast<int>(timeout_sec > 0 ? timeout_sec * 1000 : 15000);
        fnSetTimeouts(hSession, ms, ms, ms, ms);
    }
    HINTERNET hConnect = fnConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { fnClose(hSession); return false; }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = fnOpenReq(hConnect, wmethod.c_str(), path[0] ? path : L"/", nullptr, nullptr, nullptr, flags);
    if (!hReq) { fnClose(hConnect); fnClose(hSession); return false; }

    BOOL ok = fnSend(hReq, nullptr, 0,
                     post_data.empty() ? nullptr : const_cast<uint8_t*>(post_data.data()),
                     static_cast<DWORD>(post_data.size()),
                     static_cast<DWORD>(post_data.size()), 0);
    if (ok) ok = fnRecv(hReq, nullptr);
    if (ok) {
        DWORD status = 0;
        DWORD status_sz = sizeof(status);
        fnQueryHdr(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &status_sz, nullptr);
        out_code = static_cast<int64_t>(status);
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
    auto h = std::make_unique<XCurlEasyHandle>();
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
            if (s) h.method = s;
            break;
        }
        case CURLOPT_USERAGENT: {
            const char* s = va_arg(args, const char*);
            h.user_agent = s ? s : "";
            break;
        }
        case CURLOPT_POST: {
            const long v = va_arg(args, long);
            if (v) h.method = "POST";
            break;
        }
        case CURLOPT_POSTFIELDS: {
            const char* p = va_arg(args, const char*);
            if (p) {
                const size_t len = std::strlen(p);
                h.post_data.assign(reinterpret_cast<const uint8_t*>(p),
                                   reinterpret_cast<const uint8_t*>(p) + len);
            }
            break;
        }
        case CURLOPT_POSTFIELDSIZE: {
            (void)va_arg(args, long);
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
        case CURLOPT_HTTPHEADER: {
            (void)va_arg(args, curl_slist*);
            break;
        }
        case CURLOPT_TIMEOUT: {
            h.timeout_sec = va_arg(args, long);
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
    curl_write_callback cb = nullptr;
    void* ud = nullptr;
    std::vector<uint8_t> body;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handles.find(curl);
        if (it == g_handles.end()) return CURLE_BAD_FUNCTION_ARGUMENT;
        auto& h = *it->second;
        if (h.url.empty()) return CURLE_URL_MALFORMAT;

        bool matched = false;
        for (const auto& rule : g_mocks) {
            if (h.url.rfind(rule.url_prefix, 0) == 0) {
                h.response_code = rule.response_code;
                h.content_type = rule.content_type;
                body = rule.body;
                matched = true;
                break;
            }
        }
        if (!matched) {
#if defined(_WIN32)
            int64_t live_code = 0;
            std::vector<uint8_t> live_body;
            if (XCurlLiveNetworkEnabled() &&
                XCurlPerformWinHttp(h.method, h.url, h.post_data, h.timeout_sec, live_code, live_body)) {
                h.response_code = live_code;
                h.content_type = "application/octet-stream";
                body = std::move(live_body);
                matched = true;
            }
#endif
        }
        if (!matched) {
            h.response_code = 503;
            h.content_type = "application/json";
            const std::string fallback = "{\"status\":\"offline\",\"emulator\":\"ScarlettX-XCurl\"}";
            body.assign(fallback.begin(), fallback.end());
        }
        h.downloaded_bytes = static_cast<int64_t>(body.size());
        cb = h.write_cb;
        ud = h.write_data;
    }

    if (cb && !body.empty()) {
        const size_t written = cb(reinterpret_cast<char*>(body.data()), 1, body.size(), ud);
        if (written != body.size()) {
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
        // Runs the title's callbacks, which may remove handles or clean up.
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

// No sockets to wait on: returns once a transfer is waiting for
// curl_multi_perform, or when the timeout has passed.
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
