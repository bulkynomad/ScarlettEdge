#define SCARLETTX_BUILDING_LIBHTTPCLIENT 1
#include "scarlettx/net/httpclient_abi.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#endif

namespace {

bool AsciiEqualsIgnoreCase(const std::string& a, const char* b) {
    if (!b) return false;
    const size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

void SetHeaderCaseInsensitive(std::vector<std::pair<std::string, std::string>>& headers,
                              const std::string& name,
                              const std::string& value,
                              bool append_duplicate = false) {
    for (auto& kv : headers) {
        if (AsciiEqualsIgnoreCase(kv.first, name.c_str())) {
            if (append_duplicate && !kv.second.empty()) {
                kv.second += ", " + value;
            } else {
                kv.second = value;
            }
            return;
        }
    }
    headers.emplace_back(name, value);
}

const char* FindHeaderCaseInsensitive(
    const std::vector<std::pair<std::string, std::string>>& headers,
    const char* name) {
    for (const auto& kv : headers) {
        if (AsciiEqualsIgnoreCase(kv.first, name)) {
            return kv.second.c_str();
        }
    }
    return nullptr;
}

}  // namespace

struct HC_CALL {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> req_headers;
    std::vector<uint8_t> req_body;
    uint32_t timeout_seconds = 30;

    uint32_t status_code = 0;
    SX_HRESULT network_error = SX_S_OK;
    uint32_t platform_error = 0;
    std::vector<uint8_t> resp_body;
    std::string resp_string;
    std::vector<std::pair<std::string, std::string>> resp_headers;
};

struct HC_WEBSOCKET {
    std::string uri;
    std::string subprotocol;
    std::vector<std::pair<std::string, std::string>> headers;
    HCWebSocketMessageFunction* on_msg = nullptr;
    HCWebSocketBinaryMessageFunction* on_bin = nullptr;
    HCWebSocketCloseEventFunction* on_close = nullptr;
    void* fn_ctx = nullptr;
    std::atomic<bool> connected{false};
    std::atomic<bool> closing{false};
#if defined(_WIN32)
    HINTERNET h_session = nullptr;
    HINTERNET h_connect = nullptr;
    HINTERNET h_ws = nullptr;
    std::thread recv_thread;
#endif
};

namespace {

struct HttpMockRule {
    std::string method;
    std::string url_prefix;
    uint32_t status_code = 200;
    std::vector<uint8_t> body;
    std::string content_type = "application/json";
};

std::mutex g_mutex;
bool g_initialized = false;
std::unordered_map<HC_CALL*, std::shared_ptr<HC_CALL>> g_calls;
std::vector<HttpMockRule> g_mocks;
std::unordered_map<HC_WEBSOCKET*, std::shared_ptr<HC_WEBSOCKET>> g_websockets;
std::vector<std::pair<std::string, std::string>> g_ws_mocks;

bool LiveNetworkEnabled() {
    const char* v = std::getenv("SCARLETTX_LIVE_NETWORK");
    return v && v[0] == '1';
}

XTaskQueueHandle GetImmediateSyncQueue() {
    static std::mutex q_mu;
    static XTaskQueueHandle q = nullptr;
    std::lock_guard<std::mutex> lk(q_mu);
    if (!q) {
        (void)XTaskQueueCreate(
            XTaskQueueDispatchMode_Immediate,
            XTaskQueueDispatchMode_Immediate,
            &q);
    }
    return q;
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

void ParseRawCrLfHeaders(const std::string& raw_crlf,
                         std::vector<std::pair<std::string, std::string>>& out_headers) {
    size_t pos = 0;
    bool first_line = true;
    while (pos < raw_crlf.size()) {
        size_t eol = raw_crlf.find("\r\n", pos);
        if (eol == std::string::npos) eol = raw_crlf.size();
        const std::string line = raw_crlf.substr(pos, eol - pos);
        pos = eol + 2;
        if (line.empty()) continue;
        if (first_line && line.rfind("HTTP/", 0) == 0) {
            first_line = false;
            continue;
        }
        first_line = false;
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        std::string key = line.substr(0, colon);
        size_t val_start = colon + 1;
        while (val_start < line.size() &&
               (line[val_start] == ' ' || line[val_start] == '\t')) {
            ++val_start;
        }
        std::string val = line.substr(val_start);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r')) {
            val.pop_back();
        }
        SetHeaderCaseInsensitive(out_headers, key, val, true);
    }
}

bool PerformWinHttpRequest(
    const std::string& method,
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& req_headers,
    const std::vector<uint8_t>& req_body,
    uint32_t timeout_sec,
    uint32_t& out_status,
    std::vector<uint8_t>& out_body,
    std::vector<std::pair<std::string, std::string>>& out_headers,
    uint32_t& out_win32_err) {
    out_win32_err = 0;
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
    std::string hdr_joined;
    for (const auto& kv : req_headers) {
        hdr_joined += kv.first + ": " + kv.second + "\r\n";
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
    if (!fnCrack(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) {
        out_win32_err = GetLastError();
        return false;
    }

    HINTERNET hSession = fnOpen(L"ScarlettEdge-HC/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hSession) { out_win32_err = GetLastError(); return false; }
    if (fnSetTimeouts) {
        const int ms = (timeout_sec == 0) ? 15000
                                          : (timeout_sec > 2147483u ? 2147483000 : static_cast<int>(timeout_sec * 1000u));
        fnSetTimeouts(hSession, ms, ms, ms, ms);
    }
    HINTERNET hConnect = fnConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { out_win32_err = GetLastError(); fnClose(hSession); return false; }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = fnOpenReq(hConnect, wmethod.c_str(), path[0] ? path : L"/", nullptr, nullptr, nullptr, flags);
    if (!hReq) { out_win32_err = GetLastError(); fnClose(hConnect); fnClose(hSession); return false; }

    BOOL ok = fnSend(hReq,
                     wheaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wheaders.c_str(),
                     static_cast<DWORD>(wheaders.size()),
                     req_body.empty() ? nullptr : const_cast<uint8_t*>(req_body.data()),
                     static_cast<DWORD>(req_body.size()),
                     static_cast<DWORD>(req_body.size()), 0);
    if (ok) ok = fnRecv(hReq, nullptr);
    if (!ok) {
        out_win32_err = GetLastError();
    } else {
        DWORD status = 0;
        DWORD status_sz = sizeof(status);
        fnQueryHdr(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &status_sz, nullptr);
        out_status = status;

        // Query real response headers (WINHTTP_QUERY_RAW_HEADERS_CRLF)
        out_headers.clear();
        DWORD raw_hdr_bytes = 0;
        fnQueryHdr(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                   nullptr, &raw_hdr_bytes, WINHTTP_NO_HEADER_INDEX);
        if (raw_hdr_bytes > sizeof(wchar_t)) {
            std::vector<wchar_t> wbuf((raw_hdr_bytes / sizeof(wchar_t)) + 1, L'\0');
            if (fnQueryHdr(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                           wbuf.data(), &raw_hdr_bytes, WINHTTP_NO_HEADER_INDEX)) {
                const size_t wchars = raw_hdr_bytes / sizeof(wchar_t);
                ParseRawCrLfHeaders(WideToUtf8(wbuf.data(), wchars), out_headers);
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

void ExecuteHttpCall(const std::shared_ptr<HC_CALL>& call) {
    if (!call) return;
    std::string method;
    std::string url;
    std::vector<std::pair<std::string, std::string>> req_headers;
    std::vector<uint8_t> req_body;
    uint32_t timeout_sec = 30;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& rule : g_mocks) {
            const bool method_ok = rule.method.empty() || rule.method == "*" || rule.method == call->method;
            const bool url_ok = call->url.rfind(rule.url_prefix, 0) == 0;
            if (method_ok && url_ok) {
                call->status_code = rule.status_code;
                call->network_error = SX_S_OK;
                call->platform_error = 0;
                call->resp_body = rule.body;
                call->resp_string.assign(
                    reinterpret_cast<const char*>(rule.body.data()), rule.body.size());
                call->resp_headers.clear();
                SetHeaderCaseInsensitive(call->resp_headers, "Content-Type", rule.content_type);
                SetHeaderCaseInsensitive(call->resp_headers, "X-ScarlettX-Offline", "1");
                return;
            }
        }
        method = call->method;
        url = call->url;
        req_headers = call->req_headers;
        req_body = call->req_body;
        timeout_sec = call->timeout_seconds;
    }

#if defined(_WIN32)
    if (LiveNetworkEnabled()) {
        // Blocking WinHTTP network I/O runs WITHOUT holding g_mutex!
        uint32_t live_status = 0;
        std::vector<uint8_t> live_body;
        std::vector<std::pair<std::string, std::string>> live_headers;
        uint32_t win32_err = 0;
        if (PerformWinHttpRequest(method, url, req_headers, req_body, timeout_sec,
                                  live_status, live_body, live_headers, win32_err)) {
            std::lock_guard<std::mutex> lock(g_mutex);
            call->status_code = live_status;
            call->network_error = SX_S_OK;
            call->platform_error = 0;
            call->resp_body = std::move(live_body);
            call->resp_string.assign(
                reinterpret_cast<const char*>(call->resp_body.data()), call->resp_body.size());
            call->resp_headers = std::move(live_headers);
            SetHeaderCaseInsensitive(call->resp_headers, "X-ScarlettX-Live", "1");
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        call->status_code = 0;
        call->network_error = XGR_E_FAIL;
        call->platform_error = win32_err;
        call->resp_body.clear();
        call->resp_string.clear();
        call->resp_headers.clear();
        return;
    }
#endif

    // Deterministic offline fallback so titles probing telemetry/auth don't hang
    const std::string fallback = "{\"status\":\"offline\",\"emulator\":\"ScarlettX\"}";
    std::lock_guard<std::mutex> lock(g_mutex);
    call->status_code = 503;
    call->network_error = SX_S_OK;
    call->platform_error = 0;
    call->resp_string = fallback;
    call->resp_body.assign(fallback.begin(), fallback.end());
    call->resp_headers.clear();
    SetHeaderCaseInsensitive(call->resp_headers, "Content-Type", "application/json");
    SetHeaderCaseInsensitive(call->resp_headers, "X-ScarlettX-Offline", "1");
}

struct HttpPerformContext {
    std::shared_ptr<HC_CALL> call;
    bool restore_null_queue = false;
};

XGameResult SCARLETTX_GDK_CALLBACK HttpPerformProvider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* ctx = static_cast<HttpPerformContext*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            if (ctx && ctx->restore_null_queue && data->async) {
                data->async->queue = nullptr;
            }
            return XAsyncSchedule(data->async, 0);
        case XAsyncOp_DoWork:
            if (ctx) {
                ExecuteHttpCall(ctx->call);
            }
            XAsyncComplete(data->async, SX_S_OK, 0);
            return SX_S_OK;
        case XAsyncOp_Cancel:
            XAsyncComplete(data->async, XGR_E_ABORT, 0);
            return SX_S_OK;
        case XAsyncOp_Cleanup:
            delete ctx;
            return SX_S_OK;
        default:
            return SX_S_OK;
    }
}

#if defined(_WIN32)
void CloseLiveWinHttpWebSocketHandles(HC_WEBSOCKET* ws) {
    if (!ws) return;
    ws->closing.store(true, std::memory_order_release);
    static HMODULE wh = LoadLibraryW(L"winhttp.dll");
    auto fnClose = wh ? reinterpret_cast<decltype(&WinHttpCloseHandle)>(
                            reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpCloseHandle")))
                      : nullptr;
    if (fnClose) {
        if (ws->h_ws) { fnClose(ws->h_ws); ws->h_ws = nullptr; }
        if (ws->h_connect) { fnClose(ws->h_connect); ws->h_connect = nullptr; }
        if (ws->h_session) { fnClose(ws->h_session); ws->h_session = nullptr; }
    }
}

void JoinWebSocketReceiveThread(const std::shared_ptr<HC_WEBSOCKET>& ws) {
    if (!ws) return;
    if (ws->recv_thread.joinable()) {
        if (ws->recv_thread.get_id() == std::this_thread::get_id()) {
            ws->recv_thread.detach();
        } else {
            ws->recv_thread.join();
        }
    }
}

void LiveWebSocketReceiveLoop(std::shared_ptr<HC_WEBSOCKET> ws) {
    static HMODULE wh = LoadLibraryW(L"winhttp.dll");
    auto fnRecv = wh ? reinterpret_cast<decltype(&WinHttpWebSocketReceive)>(
                           reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpWebSocketReceive")))
                     : nullptr;
    auto fnQueryClose = wh ? reinterpret_cast<decltype(&WinHttpWebSocketQueryCloseStatus)>(
                                 reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpWebSocketQueryCloseStatus")))
                           : nullptr;
    if (!fnRecv) return;

    std::vector<uint8_t> chunk(8192);
    std::string utf8_accum;
    std::vector<uint8_t> bin_accum;

    while (!ws->closing.load(std::memory_order_acquire)) {
        HINTERNET h_ws = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            h_ws = ws->h_ws;
        }
        if (!h_ws) break;

        DWORD bytes_read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE buf_type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        const DWORD err = fnRecv(h_ws, chunk.data(), static_cast<DWORD>(chunk.size()), &bytes_read, &buf_type);
        if (err != NO_ERROR || ws->closing.load(std::memory_order_acquire)) {
            break;
        }
        if (buf_type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT status = 1000;
            DWORD reason_used = 0;
            if (fnQueryClose) {
                (void)fnQueryClose(h_ws, &status, nullptr, 0, &reason_used);
            }
            HCWebSocketCloseEventFunction* close_cb = nullptr;
            void* ctx = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (ws->connected.exchange(false, std::memory_order_acq_rel)) {
                    close_cb = ws->on_close;
                    ctx = ws->fn_ctx;
                }
            }
            if (close_cb) close_cb(ws.get(), status, ctx);
            break;
        } else if (buf_type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
            utf8_accum.append(reinterpret_cast<const char*>(chunk.data()), bytes_read);
        } else if (buf_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            utf8_accum.append(reinterpret_cast<const char*>(chunk.data()), bytes_read);
            HCWebSocketMessageFunction* msg_cb = nullptr;
            void* ctx = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                msg_cb = ws->on_msg;
                ctx = ws->fn_ctx;
            }
            if (msg_cb) msg_cb(ws.get(), utf8_accum.c_str(), ctx);
            utf8_accum.clear();
        } else if (buf_type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
            bin_accum.insert(bin_accum.end(), chunk.data(), chunk.data() + bytes_read);
        } else if (buf_type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            bin_accum.insert(bin_accum.end(), chunk.data(), chunk.data() + bytes_read);
            HCWebSocketBinaryMessageFunction* bin_cb = nullptr;
            void* ctx = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                bin_cb = ws->on_bin;
                ctx = ws->fn_ctx;
            }
            if (bin_cb) {
                bin_cb(ws.get(), bin_accum.data(), static_cast<uint32_t>(bin_accum.size()), ctx);
            }
            bin_accum.clear();
        }
    }
}

bool TryConnectLiveWinHttpWebSocket(
    const std::string& uri,
    const std::string& subprotocol,
    const std::vector<std::pair<std::string, std::string>>& headers,
    HINTERNET& out_session,
    HINTERNET& out_connect,
    HINTERNET& out_ws) {
    out_session = nullptr;
    out_connect = nullptr;
    out_ws = nullptr;
    static HMODULE wh = LoadLibraryW(L"winhttp.dll");
    if (!wh) return false;
    auto fnOpen = reinterpret_cast<decltype(&WinHttpOpen)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpOpen")));
    auto fnCrack = reinterpret_cast<decltype(&WinHttpCrackUrl)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpCrackUrl")));
    auto fnConnect = reinterpret_cast<decltype(&WinHttpConnect)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpConnect")));
    auto fnOpenReq = reinterpret_cast<decltype(&WinHttpOpenRequest)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpOpenRequest")));
    auto fnSetOpt = reinterpret_cast<decltype(&WinHttpSetOption)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpSetOption")));
    auto fnSend = reinterpret_cast<decltype(&WinHttpSendRequest)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpSendRequest")));
    auto fnRecv = reinterpret_cast<decltype(&WinHttpReceiveResponse)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpReceiveResponse")));
    auto fnUpgrade = reinterpret_cast<decltype(&WinHttpWebSocketCompleteUpgrade)>(
        reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpWebSocketCompleteUpgrade")));
    auto fnClose = reinterpret_cast<decltype(&WinHttpCloseHandle)>(reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpCloseHandle")));
    if (!fnOpen || !fnCrack || !fnConnect || !fnOpenReq || !fnSetOpt || !fnSend || !fnRecv || !fnUpgrade || !fnClose) {
        return false;
    }

    std::string http_url = uri;
    bool secure = false;
    if (http_url.rfind("wss://", 0) == 0) {
        http_url = "https://" + http_url.substr(6);
        secure = true;
    } else if (http_url.rfind("ws://", 0) == 0) {
        http_url = "http://" + http_url.substr(5);
    }
    std::wstring wurl = Utf8ToWide(http_url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!fnCrack(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) return false;

    std::string hdr_joined;
    bool has_ws_proto = false;
    for (const auto& kv : headers) {
        if (AsciiEqualsIgnoreCase(kv.first, "Sec-WebSocket-Protocol")) {
            has_ws_proto = true;
        }
        hdr_joined += kv.first + ": " + kv.second + "\r\n";
    }
    if (!subprotocol.empty() && !has_ws_proto) {
        hdr_joined += "Sec-WebSocket-Protocol: " + subprotocol + "\r\n";
    }
    std::wstring wheaders = Utf8ToWide(hdr_joined);

    HINTERNET hSess = fnOpen(L"ScarlettEdge-HCWS/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hSess) return false;
    HINTERNET hConn = fnConnect(hSess, host, uc.nPort, 0);
    if (!hConn) { fnClose(hSess); return false; }
    HINTERNET hReq = fnOpenReq(hConn, L"GET", path[0] ? path : L"/", nullptr, nullptr, nullptr,
                               secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) { fnClose(hConn); fnClose(hSess); return false; }

    if (!fnSetOpt(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !fnSend(hReq,
                wheaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wheaders.c_str(),
                static_cast<DWORD>(wheaders.size()),
                nullptr, 0, 0, 0) ||
        !fnRecv(hReq, nullptr)) {
        fnClose(hReq); fnClose(hConn); fnClose(hSess);
        return false;
    }
    HINTERNET hWs = fnUpgrade(hReq, 0);
    fnClose(hReq);
    if (!hWs) { fnClose(hConn); fnClose(hSess); return false; }
    out_session = hSess;
    out_connect = hConn;
    out_ws = hWs;
    return true;
}
#endif

XGameResult SCARLETTX_GDK_CALLBACK WsNoPayloadProvider(XAsyncOp op, const XAsyncProviderData* d) {
    if (op == XAsyncOp_Begin) return XAsyncSchedule(d->async, 0);
    if (op == XAsyncOp_DoWork) {
        XAsyncComplete(d->async, SX_S_OK, 0);
        return SX_S_OK;
    }
    if (op == XAsyncOp_Cancel) {
        XAsyncComplete(d->async, XGR_E_ABORT, 0);
        return SX_S_OK;
    }
    return SX_S_OK;
}

}  // namespace

extern "C" {

SX_HRESULT HCInitialize(void* args) {
    (void)args;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_initialized = true;
    return SX_S_OK;
}

void HCCleanup(void) {
    std::vector<std::shared_ptr<HC_WEBSOCKET>> to_close;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& kv : g_websockets) {
#if defined(_WIN32)
            CloseLiveWinHttpWebSocketHandles(kv.second.get());
#endif
            kv.second->connected.store(false, std::memory_order_release);
            to_close.push_back(std::move(kv.second));
        }
        g_websockets.clear();
        g_calls.clear();
        g_initialized = false;
    }
#if defined(_WIN32)
    for (const auto& ws : to_close) {
        JoinWebSocketReceiveThread(ws);
    }
#endif
}

uint32_t HCIsInitialized(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_initialized ? 1u : 0u;
}

SX_HRESULT HCGetLibVersion(const char** version) {
    if (!version) return SX_E_INVALIDARG;
    *version = "ScarlettX-libHttpClient-GDK-1.0";
    return SX_S_OK;
}

SX_HRESULT HCHttpCallCreate(HCCallHandle* call) {
    if (!call) return SX_E_INVALIDARG;
    *call = nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto obj = std::make_shared<HC_CALL>();
    HC_CALL* raw = obj.get();
    g_calls[raw] = std::move(obj);
    *call = raw;
    return SX_S_OK;
}

SX_HRESULT HCHttpCallCloseHandle(HCCallHandle call) {
    if (!call) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_calls.erase(call) > 0 ? SX_S_OK : SX_E_INVALIDARG;
}

SX_HRESULT HCHttpCallRequestSetUrl(HCCallHandle call, const char* method, const char* url) {
    if (!call || !method || !url) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_calls.find(call);
    if (it == g_calls.end()) return SX_E_INVALIDARG;
    it->second->method = method;
    it->second->url = url;
    return SX_S_OK;
}

SX_HRESULT HCHttpCallRequestSetHeader(
    HCCallHandle call,
    const char* header_name,
    const char* header_value,
    uint32_t allow_tracing) {
    (void)allow_tracing;
    if (!call || !header_name || !header_value) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_calls.find(call);
    if (it == g_calls.end()) return SX_E_INVALIDARG;
    SetHeaderCaseInsensitive(it->second->req_headers, header_name, header_value);
    return SX_S_OK;
}

SX_HRESULT HCHttpCallRequestSetRequestBodyBytes(
    HCCallHandle call,
    const uint8_t* request_body_bytes,
    uint32_t request_body_size) {
    if (!call || (!request_body_bytes && request_body_size > 0)) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_calls.find(call);
    if (it == g_calls.end()) return SX_E_INVALIDARG;
    it->second->req_body.assign(request_body_bytes, request_body_bytes + request_body_size);
    return SX_S_OK;
}

SX_HRESULT HCHttpCallRequestSetTimeout(HCCallHandle call, uint32_t timeout_in_seconds) {
    if (!call) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_calls.find(call);
    if (it == g_calls.end()) return SX_E_INVALIDARG;
    it->second->timeout_seconds = timeout_in_seconds;
    return SX_S_OK;
}

SX_HRESULT HCHttpCallRequestGetUrl(HCCallHandle call, const char** method, const char** url) {
    if (!call) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_calls.find(call);
    if (it == g_calls.end()) return SX_E_INVALIDARG;
    if (method) *method = it->second->method.c_str();
    if (url) *url = it->second->url.c_str();
    return SX_S_OK;
}

SX_HRESULT HCHttpCallRequestGetTimeout(HCCallHandle call, uint32_t* timeout_in_seconds) {
    if (!call || !timeout_in_seconds) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_calls.find(call);
    if (it == g_calls.end()) return SX_E_INVALIDARG;
    *timeout_in_seconds = it->second->timeout_seconds;
    return SX_S_OK;
}

SX_HRESULT HCHttpCallPerformAsync(HCCallHandle call, XAsyncBlock* async_block) {
    if (!call) return SX_E_INVALIDARG;
    std::shared_ptr<HC_CALL> call_sp;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_calls.find(call);
        if (it == g_calls.end()) return SX_E_INVALIDARG;
        call_sp = it->second;
    }

    if (!async_block) {
        ExecuteHttpCall(call_sp);
        return SX_S_OK;
    }

    auto* ctx = new (std::nothrow) HttpPerformContext();
    if (!ctx) return XGR_E_OUTOFMEMORY;
    ctx->call = std::move(call_sp);
    if (!async_block->queue && !LiveNetworkEnabled()) {
        if (XTaskQueueHandle imm = GetImmediateSyncQueue()) {
            async_block->queue = imm;
            ctx->restore_null_queue = true;
        }
    }

    const XGameResult hr = XAsyncBegin(
        async_block,
        ctx,
        reinterpret_cast<const void*>(&HCHttpCallPerformAsync),
        "HCHttpCallPerformAsync",
        &HttpPerformProvider);
    if (hr < 0) {
        if (ctx->restore_null_queue) {
            async_block->queue = nullptr;
        }
        delete ctx;
        return hr;
    }
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetStatusCode(HCCallHandle call, uint32_t* status_code) {
    if (!call || !status_code) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_calls.find(call);
    if (it == g_calls.end()) return SX_E_INVALIDARG;
    *status_code = it->second->status_code;
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetNetworkErrorCode(
    HCCallHandle call,
    SX_HRESULT* network_error_code,
    uint32_t* platform_network_error_code) {
    if (!call || !network_error_code) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_calls.find(call);
    if (it == g_calls.end()) return SX_E_INVALIDARG;
    *network_error_code = it->second->network_error;
    if (platform_network_error_code) {
        *platform_network_error_code = it->second->platform_error;
    }
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetResponseString(HCCallHandle call, const char** response_string) {
    if (!call || !response_string) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_calls.find(call);
    if (it == g_calls.end()) return SX_E_INVALIDARG;
    *response_string = it->second->resp_string.c_str();
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetResponseBodyBytesSize(HCCallHandle call, size_t* buffer_size) {
    if (!call || !buffer_size) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_calls.find(call);
    if (it == g_calls.end()) return SX_E_INVALIDARG;
    *buffer_size = it->second->resp_body.size();
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetResponseBodyBytes(
    HCCallHandle call,
    size_t buffer_size,
    uint8_t* buffer,
    size_t* buffer_used) {
    if (!call || (!buffer && buffer_size > 0)) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_calls.find(call);
    if (it == g_calls.end()) return SX_E_INVALIDARG;
    const size_t n = std::min(buffer_size, it->second->resp_body.size());
    if (n > 0) {
        std::memcpy(buffer, it->second->resp_body.data(), n);
    }
    if (buffer_used) {
        *buffer_used = n;
    }
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetHeader(
    HCCallHandle call,
    const char* header_name,
    const char** header_value) {
    if (!call || !header_name || !header_value) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_calls.find(call);
    if (it == g_calls.end()) return SX_E_INVALIDARG;
    // RFC 7230 Section 3.2: HTTP header field names are case-insensitive.
    *header_value = FindHeaderCaseInsensitive(it->second->resp_headers, header_name);
    return SX_S_OK;
}

SX_HRESULT HCHttpCallRegisterMock(
    const char* method,
    const char* url_prefix,
    uint32_t status_code,
    const uint8_t* body,
    uint32_t body_size,
    const char* content_type) {
    if (!url_prefix) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    HttpMockRule rule{};
    rule.method = method ? method : "*";
    rule.url_prefix = url_prefix;
    rule.status_code = status_code;
    if (body && body_size > 0) {
        rule.body.assign(body, body + body_size);
    }
    rule.content_type = content_type ? content_type : "application/octet-stream";
    g_mocks.push_back(std::move(rule));
    return SX_S_OK;
}

void HCHttpCallClearMocks(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mocks.clear();
}

SX_HRESULT HCWebSocketCreate(
    HCWebSocketHandle* websocket,
    HCWebSocketMessageFunction* message_func,
    HCWebSocketBinaryMessageFunction* binary_message_func,
    HCWebSocketCloseEventFunction* close_func,
    void* function_context) {
    if (!websocket) return SX_E_INVALIDARG;
    *websocket = nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto obj = std::make_shared<HC_WEBSOCKET>();
    obj->on_msg = message_func;
    obj->on_bin = binary_message_func;
    obj->on_close = close_func;
    obj->fn_ctx = function_context;
    HC_WEBSOCKET* raw = obj.get();
    g_websockets[raw] = std::move(obj);
    *websocket = raw;
    return SX_S_OK;
}

SX_HRESULT HCWebSocketSetHeader(
    HCWebSocketHandle websocket,
    const char* header_name,
    const char* header_value) {
    if (!websocket || !header_name || !header_value) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_websockets.find(websocket);
    if (it == g_websockets.end()) return SX_E_INVALIDARG;
    SetHeaderCaseInsensitive(it->second->headers, header_name, header_value);
    return SX_S_OK;
}

SX_HRESULT HCWebSocketConnectAsync(
    const char* uri,
    const char* subprotocol,
    HCWebSocketHandle websocket,
    XAsyncBlock* async_block) {
    if (!uri || !*uri || !websocket) return SX_E_INVALIDARG;
    std::shared_ptr<HC_WEBSOCKET> ws_sp;
    std::string uri_str = uri;
    std::string sub_str = subprotocol ? subprotocol : "";
    std::vector<std::pair<std::string, std::string>> hdr_copy;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_websockets.find(websocket);
        if (it == g_websockets.end()) return SX_E_INVALIDARG;
        ws_sp = it->second;
        ws_sp->uri = uri_str;
        ws_sp->subprotocol = sub_str;
        ws_sp->closing.store(false, std::memory_order_release);
        hdr_copy = ws_sp->headers;
    }

#if defined(_WIN32)
    JoinWebSocketReceiveThread(ws_sp);
    if (LiveNetworkEnabled()) {
        HINTERNET h_sess = nullptr, h_conn = nullptr, h_ws = nullptr;
        const bool ok = TryConnectLiveWinHttpWebSocket(uri_str, sub_str, hdr_copy, h_sess, h_conn, h_ws);
        if (ok) {
            std::lock_guard<std::mutex> lock(g_mutex);
            ws_sp->h_session = h_sess;
            ws_sp->h_connect = h_conn;
            ws_sp->h_ws = h_ws;
            ws_sp->connected.store(true, std::memory_order_release);
            ws_sp->recv_thread = std::thread(LiveWebSocketReceiveLoop, ws_sp);
        } else {
            ws_sp->connected.store(true, std::memory_order_release);
        }
    } else {
        ws_sp->connected.store(true, std::memory_order_release);
    }
#else
    ws_sp->connected.store(true, std::memory_order_release);
#endif

    if (async_block) {
        const XGameResult hr = XAsyncBegin(
            async_block, nullptr, reinterpret_cast<const void*>(&HCWebSocketConnectAsync),
            "HCWebSocketConnectAsync", &WsNoPayloadProvider);
        if (hr < 0) return hr;
    }
    return SX_S_OK;
}

SX_HRESULT HCWebSocketSendMessageAsync(
    HCWebSocketHandle websocket,
    const char* message,
    XAsyncBlock* async_block) {
    if (!websocket || !message) return SX_E_INVALIDARG;
    HCWebSocketMessageFunction* msg_cb = nullptr;
    void* fn_ctx = nullptr;
    std::string reply;
#if defined(_WIN32)
    HINTERNET live_ws = nullptr;
#endif
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_websockets.find(websocket);
        if (it == g_websockets.end() || !it->second->connected.load(std::memory_order_acquire)) {
            return SX_E_INVALIDARG;
        }
        auto& ws = it->second;
#if defined(_WIN32)
        live_ws = ws->h_ws;
#endif
        msg_cb = ws->on_msg;
        fn_ctx = ws->fn_ctx;
        for (const auto& [pre, r] : g_ws_mocks) {
            if (ws->uri.rfind(pre, 0) == 0) {
                reply = r;
                break;
            }
        }
#if defined(_WIN32)
        const bool is_live = (live_ws != nullptr);
#else
        const bool is_live = false;
#endif
        if (reply.empty() && !is_live) {
            if (message[0] == '[') {
                reply = "[1,1,0,{\"subscriptionId\":1,\"state\":\"Subscribed\"}]";
            } else {
                reply = "{\"status\":\"ok\",\"bridge\":\"ScarlettEdge-PlayFab-RTA\"}";
            }
        }
    }

#if defined(_WIN32)
    if (live_ws) {
        static HMODULE wh = LoadLibraryW(L"winhttp.dll");
        auto fnWsSend = wh ? reinterpret_cast<decltype(&WinHttpWebSocketSend)>(
                                 reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpWebSocketSend")))
                           : nullptr;
        if (fnWsSend) {
            (void)fnWsSend(live_ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                           const_cast<char*>(message), static_cast<DWORD>(std::strlen(message)));
        }
    }
#endif

    // Schedule XAsyncBegin BEFORE firing incoming message callbacks!
    if (async_block) {
        const XGameResult hr = XAsyncBegin(
            async_block, nullptr, reinterpret_cast<const void*>(&HCWebSocketSendMessageAsync),
            "HCWebSocketSendMessageAsync", &WsNoPayloadProvider);
        if (hr < 0) return hr;
    }
    if (msg_cb && !reply.empty()) {
        msg_cb(websocket, reply.c_str(), fn_ctx);
    }
    return SX_S_OK;
}

SX_HRESULT HCWebSocketSendBinaryMessageAsync(
    HCWebSocketHandle websocket,
    const uint8_t* payload_bytes,
    uint32_t payload_size,
    XAsyncBlock* async_block) {
    if (!websocket || (!payload_bytes && payload_size > 0)) return SX_E_INVALIDARG;
    HCWebSocketBinaryMessageFunction* bin_cb = nullptr;
    void* fn_ctx = nullptr;
#if defined(_WIN32)
    HINTERNET live_ws = nullptr;
#endif
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_websockets.find(websocket);
        if (it == g_websockets.end() || !it->second->connected.load(std::memory_order_acquire)) {
            return SX_E_INVALIDARG;
        }
        bin_cb = it->second->on_bin;
        fn_ctx = it->second->fn_ctx;
#if defined(_WIN32)
        live_ws = it->second->h_ws;
#endif
    }

#if defined(_WIN32)
    if (live_ws) {
        static HMODULE wh = LoadLibraryW(L"winhttp.dll");
        auto fnWsSend = wh ? reinterpret_cast<decltype(&WinHttpWebSocketSend)>(
                                 reinterpret_cast<void*>(GetProcAddress(wh, "WinHttpWebSocketSend")))
                           : nullptr;
        if (fnWsSend) {
            (void)fnWsSend(live_ws, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                           payload_size > 0 ? const_cast<uint8_t*>(payload_bytes) : nullptr,
                           static_cast<DWORD>(payload_size));
        }
    }
#endif

    // Schedule XAsyncBegin BEFORE firing message callbacks!
    if (async_block) {
        const XGameResult hr = XAsyncBegin(
            async_block, nullptr, reinterpret_cast<const void*>(&HCWebSocketSendBinaryMessageAsync),
            "HCWebSocketSendBinaryMessageAsync", &WsNoPayloadProvider);
        if (hr < 0) return hr;
    }
#if defined(_WIN32)
    const bool is_live = (live_ws != nullptr);
#else
    const bool is_live = false;
#endif
    // Only echo binary payloads in offline synthetic mode; live WebSockets receive real frames from the server!
    if (!is_live && bin_cb && payload_size > 0) {
        bin_cb(websocket, payload_bytes, payload_size, fn_ctx);
    }
    return SX_S_OK;
}

SX_HRESULT HCWebSocketDisconnect(HCWebSocketHandle websocket) {
    if (!websocket) return SX_E_INVALIDARG;
    HCWebSocketCloseEventFunction* close_cb = nullptr;
    void* fn_ctx = nullptr;
    std::shared_ptr<HC_WEBSOCKET> ws_sp;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_websockets.find(websocket);
        if (it == g_websockets.end()) return SX_E_INVALIDARG;
        ws_sp = it->second;
#if defined(_WIN32)
        CloseLiveWinHttpWebSocketHandles(ws_sp.get());
#endif
        if (ws_sp->connected.exchange(false, std::memory_order_acq_rel)) {
            close_cb = ws_sp->on_close;
            fn_ctx = ws_sp->fn_ctx;
        }
    }
#if defined(_WIN32)
    JoinWebSocketReceiveThread(ws_sp);
#endif
    if (close_cb) close_cb(websocket, 1000u /* NormalClosure */, fn_ctx);
    return SX_S_OK;
}

SX_HRESULT HCWebSocketCloseHandle(HCWebSocketHandle websocket) {
    if (!websocket) return SX_E_INVALIDARG;
    std::shared_ptr<HC_WEBSOCKET> ws_sp;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_websockets.find(websocket);
        if (it == g_websockets.end()) return SX_E_INVALIDARG;
        ws_sp = std::move(it->second);
#if defined(_WIN32)
        CloseLiveWinHttpWebSocketHandles(ws_sp.get());
#endif
        ws_sp->connected.store(false, std::memory_order_release);
        g_websockets.erase(it);
    }
#if defined(_WIN32)
    JoinWebSocketReceiveThread(ws_sp);
#endif
    return SX_S_OK;
}

SX_HRESULT HCWebSocketRegisterMockReply(const char* uri_prefix, const char* reply_json) {
    if (!uri_prefix || !reply_json) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ws_mocks.emplace_back(uri_prefix, reply_json);
    return SX_S_OK;
}

}  // extern "C"
