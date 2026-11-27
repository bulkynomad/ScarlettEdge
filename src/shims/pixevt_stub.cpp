#include <cstdint>

#if defined(_WIN32)
#  define SCARLETTX_PIX_API __declspec(dllexport)
#  define SCARLETTX_PIX_CALL __stdcall
#else
#  define SCARLETTX_PIX_API __attribute__((visibility("default")))
#  define SCARLETTX_PIX_CALL
#endif

extern "C" {

SCARLETTX_PIX_API void SCARLETTX_PIX_CALL PIXBeginEventOnCommandList(
    void* /*commandList*/, uint64_t /*color*/, const char* /*formatString*/) {}

SCARLETTX_PIX_API void SCARLETTX_PIX_CALL PIXEndEventOnCommandList(
    void* /*commandList*/) {}

SCARLETTX_PIX_API void SCARLETTX_PIX_CALL PIXSetMarkerOnCommandList(
    void* /*commandList*/, uint64_t /*color*/, const char* /*formatString*/) {}

SCARLETTX_PIX_API void SCARLETTX_PIX_CALL PIXBeginEventOnCommandQueue(
    void* /*commandQueue*/, uint64_t /*color*/, const char* /*formatString*/) {}

SCARLETTX_PIX_API void SCARLETTX_PIX_CALL PIXEndEventOnCommandQueue(
    void* /*commandQueue*/) {}

SCARLETTX_PIX_API void SCARLETTX_PIX_CALL PIXSetMarkerOnCommandQueue(
    void* /*commandQueue*/, uint64_t /*color*/, const char* /*formatString*/) {}

SCARLETTX_PIX_API void SCARLETTX_PIX_CALL PIXNotifyWakeFromFenceSignal(
    void* /*eventHandle*/) {}

}  // extern "C"
