#define SCARLETTX_BUILDING_LIBHTTPCLIENT 1
#include "scarlettx/net/httpclient_abi.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct HC_CALL {
    std::string method = "GET";
    std::string url;
    std::unordered_map<std::string, std::string> req_headers;
    std::vector<uint8_t> req_body;
    uint32_t timeout_seconds = 30;

    uint32_t status_code = 0;
    SX_HRESULT network_error = SX_S_OK;
    uint32_t platform_error = 0;
    std::vector<uint8_t> resp_body;
    std::string resp_string;
    std::unordered_map<std::string, std::string> resp_headers;
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
std::unordered_map<HC_CALL*, std::unique_ptr<HC_CALL>> g_calls;
std::vector<HttpMockRule> g_mocks;

}  // namespace

extern "C" {

SX_HRESULT HCInitialize(void* args) {
    (void)args;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_initialized = true;
    return SX_S_OK;
}

void HCCleanup(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_calls.clear();
    g_initialized = false;
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
    auto obj = std::make_unique<HC_CALL>();
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
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    call->method = method;
    call->url = url;
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
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    call->req_headers[header_name] = header_value;
    return SX_S_OK;
}

SX_HRESULT HCHttpCallRequestSetRequestBodyBytes(
    HCCallHandle call,
    const uint8_t* request_body_bytes,
    uint32_t request_body_size) {
    if (!call || (!request_body_bytes && request_body_size > 0)) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    call->req_body.assign(request_body_bytes, request_body_bytes + request_body_size);
    return SX_S_OK;
}

SX_HRESULT HCHttpCallRequestSetTimeout(HCCallHandle call, uint32_t timeout_in_seconds) {
    if (!call) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    call->timeout_seconds = timeout_in_seconds;
    return SX_S_OK;
}

SX_HRESULT HCHttpCallRequestGetUrl(HCCallHandle call, const char** method, const char** url) {
    if (!call) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    if (method) *method = call->method.c_str();
    if (url) *url = call->url.c_str();
    return SX_S_OK;
}

SX_HRESULT HCHttpCallRequestGetTimeout(HCCallHandle call, uint32_t* timeout_in_seconds) {
    if (!call || !timeout_in_seconds) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    *timeout_in_seconds = call->timeout_seconds;
    return SX_S_OK;
}

SX_HRESULT HCHttpCallPerformAsync(HCCallHandle call, XAsyncBlock* async_block) {
    if (!call) return SX_E_INVALIDARG;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;

        bool matched = false;
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
                call->resp_headers["Content-Type"] = rule.content_type;
                call->resp_headers["X-ScarlettX-Offline"] = "1";
                matched = true;
                break;
            }
        }
        if (!matched) {
            // Deterministic offline fallback so titles probing telemetry/auth don't hang
            const std::string fallback = "{\"status\":\"offline\",\"emulator\":\"ScarlettX\"}";
            call->status_code = 503;
            call->network_error = SX_S_OK;
            call->platform_error = 0;
            call->resp_string = fallback;
            call->resp_body.assign(fallback.begin(), fallback.end());
            call->resp_headers["Content-Type"] = "application/json";
            call->resp_headers["X-ScarlettX-Offline"] = "1";
        }
    }

    if (async_block) {
        if (async_block->callback) {
            async_block->callback(async_block);
        }
    }
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetStatusCode(HCCallHandle call, uint32_t* status_code) {
    if (!call || !status_code) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    *status_code = call->status_code;
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetNetworkErrorCode(
    HCCallHandle call,
    SX_HRESULT* network_error_code,
    uint32_t* platform_network_error_code) {
    if (!call || !network_error_code) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    *network_error_code = call->network_error;
    if (platform_network_error_code) {
        *platform_network_error_code = call->platform_error;
    }
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetResponseString(HCCallHandle call, const char** response_string) {
    if (!call || !response_string) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    *response_string = call->resp_string.c_str();
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetResponseBodyBytesSize(HCCallHandle call, size_t* buffer_size) {
    if (!call || !buffer_size) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    *buffer_size = call->resp_body.size();
    return SX_S_OK;
}

SX_HRESULT HCHttpCallResponseGetResponseBodyBytes(
    HCCallHandle call,
    size_t buffer_size,
    uint8_t* buffer,
    size_t* buffer_used) {
    if (!call || (!buffer && buffer_size > 0)) return SX_E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    const size_t n = std::min(buffer_size, call->resp_body.size());
    if (n > 0) {
        std::memcpy(buffer, call->resp_body.data(), n);
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
    if (g_calls.find(call) == g_calls.end()) return SX_E_INVALIDARG;
    auto it = call->resp_headers.find(header_name);
    *header_value = (it != call->resp_headers.end()) ? it->second.c_str() : nullptr;
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

}  // extern "C"
