#pragma once

#include <cstddef>
#include <cstdint>

#include "scarlettx/gdk/xgameruntime_abi.h"

#if defined(_WIN32)
#if defined(SCARLETTX_BUILDING_LIBHTTPCLIENT)
#define SCARLETTX_HC_API __declspec(dllexport)
#else
#define SCARLETTX_HC_API __declspec(dllimport)
#endif
#else
#define SCARLETTX_HC_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef XGameResult SX_HRESULT;
#ifndef SX_S_OK
#define SX_S_OK XGR_S_OK
#endif
#ifndef SX_E_INVALIDARG
#define SX_E_INVALIDARG XGR_E_INVALIDARG
#endif

typedef struct HC_CALL* HCCallHandle;

SCARLETTX_HC_API SX_HRESULT HCInitialize(void* args);
SCARLETTX_HC_API void HCCleanup(void);
SCARLETTX_HC_API uint32_t HCIsInitialized(void);
SCARLETTX_HC_API SX_HRESULT HCGetLibVersion(const char** version);

SCARLETTX_HC_API SX_HRESULT HCHttpCallCreate(HCCallHandle* call);
SCARLETTX_HC_API SX_HRESULT HCHttpCallCloseHandle(HCCallHandle call);
SCARLETTX_HC_API SX_HRESULT HCHttpCallRequestSetUrl(
    HCCallHandle call,
    const char* method,
    const char* url);
SCARLETTX_HC_API SX_HRESULT HCHttpCallRequestSetHeader(
    HCCallHandle call,
    const char* header_name,
    const char* header_value,
    uint32_t allow_tracing);
SCARLETTX_HC_API SX_HRESULT HCHttpCallRequestSetRequestBodyBytes(
    HCCallHandle call,
    const uint8_t* request_body_bytes,
    uint32_t request_body_size);
SCARLETTX_HC_API SX_HRESULT HCHttpCallRequestSetTimeout(
    HCCallHandle call,
    uint32_t timeout_in_seconds);
SCARLETTX_HC_API SX_HRESULT HCHttpCallRequestGetUrl(
    HCCallHandle call,
    const char** method,
    const char** url);
SCARLETTX_HC_API SX_HRESULT HCHttpCallRequestGetTimeout(
    HCCallHandle call,
    uint32_t* timeout_in_seconds);

// Executes the HTTP call (against registered mocks or offline synthetic handler)
// and completes the provided XAsyncBlock (if non-null).
SCARLETTX_HC_API SX_HRESULT HCHttpCallPerformAsync(
    HCCallHandle call,
    XAsyncBlock* async_block);

SCARLETTX_HC_API SX_HRESULT HCHttpCallResponseGetStatusCode(
    HCCallHandle call,
    uint32_t* status_code);
SCARLETTX_HC_API SX_HRESULT HCHttpCallResponseGetNetworkErrorCode(
    HCCallHandle call,
    SX_HRESULT* network_error_code,
    uint32_t* platform_network_error_code);
SCARLETTX_HC_API SX_HRESULT HCHttpCallResponseGetResponseString(
    HCCallHandle call,
    const char** response_string);
SCARLETTX_HC_API SX_HRESULT HCHttpCallResponseGetResponseBodyBytesSize(
    HCCallHandle call,
    size_t* buffer_size);
SCARLETTX_HC_API SX_HRESULT HCHttpCallResponseGetResponseBodyBytes(
    HCCallHandle call,
    size_t buffer_size,
    uint8_t* buffer,
    size_t* buffer_used);
SCARLETTX_HC_API SX_HRESULT HCHttpCallResponseGetHeader(
    HCCallHandle call,
    const char* header_name,
    const char** header_value);

// Deterministic offline mock registration for ScarlettX tests & offline play
SCARLETTX_HC_API SX_HRESULT HCHttpCallRegisterMock(
    const char* method,
    const char* url_prefix,
    uint32_t status_code,
    const uint8_t* body,
    uint32_t body_size,
    const char* content_type);
SCARLETTX_HC_API void HCHttpCallClearMocks(void);

// ---------------------------------------------------------------------------
// HCWebSocket* (GDK 2022-2026 Xbox Live RTA & PlayFab WebSocket Transport)
// ---------------------------------------------------------------------------
typedef struct HC_WEBSOCKET* HCWebSocketHandle;

typedef void(SCARLETTX_GDK_CALLBACK HCWebSocketMessageFunction)(
    HCWebSocketHandle websocket,
    const char* incoming_body_string,
    void* function_context);

typedef void(SCARLETTX_GDK_CALLBACK HCWebSocketBinaryMessageFunction)(
    HCWebSocketHandle websocket,
    const uint8_t* payload_bytes,
    uint32_t payload_size,
    void* function_context);

typedef void(SCARLETTX_GDK_CALLBACK HCWebSocketCloseEventFunction)(
    HCWebSocketHandle websocket,
    uint32_t close_status,
    void* function_context);

SCARLETTX_HC_API SX_HRESULT HCWebSocketCreate(
    HCWebSocketHandle* websocket,
    HCWebSocketMessageFunction* message_func,
    HCWebSocketBinaryMessageFunction* binary_message_func,
    HCWebSocketCloseEventFunction* close_func,
    void* function_context);

SCARLETTX_HC_API SX_HRESULT HCWebSocketSetHeader(
    HCWebSocketHandle websocket,
    const char* header_name,
    const char* header_value);

SCARLETTX_HC_API SX_HRESULT HCWebSocketConnectAsync(
    const char* uri,
    const char* subprotocol,
    HCWebSocketHandle websocket,
    XAsyncBlock* async_block);

SCARLETTX_HC_API SX_HRESULT HCWebSocketSendMessageAsync(
    HCWebSocketHandle websocket,
    const char* message,
    XAsyncBlock* async_block);

SCARLETTX_HC_API SX_HRESULT HCWebSocketSendBinaryMessageAsync(
    HCWebSocketHandle websocket,
    const uint8_t* payload_bytes,
    uint32_t payload_size,
    XAsyncBlock* async_block);

SCARLETTX_HC_API SX_HRESULT HCWebSocketDisconnect(
    HCWebSocketHandle websocket);

SCARLETTX_HC_API SX_HRESULT HCWebSocketCloseHandle(
    HCWebSocketHandle websocket);

SCARLETTX_HC_API SX_HRESULT HCWebSocketRegisterMockReply(
    const char* uri_prefix,
    const char* reply_json);

#ifdef __cplusplus
}
#endif
