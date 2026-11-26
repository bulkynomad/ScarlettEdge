#define SCARLETTX_BUILDING_FFX_LOADER 1
#include "scarlettx/middleware/ffx_abi.h"

extern "C" {

ffxReturnCode_t ffxCreateContext(
    ffxContext* context,
    ffxApiHeader* desc,
    const void* memCb) {
    (void)memCb;
    return ffxUpscalerCreateContext(context, desc);
}

ffxReturnCode_t ffxDestroyContext(
    ffxContext* context,
    const void* memCb) {
    (void)memCb;
    return ffxUpscalerDestroyContext(context);
}

ffxReturnCode_t ffxConfigure(
    ffxContext* context,
    const ffxApiHeader* desc) {
    return ffxUpscalerConfigure(context, desc);
}

ffxReturnCode_t ffxQuery(
    ffxContext* context,
    ffxApiHeader* desc) {
    return ffxUpscalerQuery(context, desc);
}

ffxReturnCode_t ffxDispatch(
    ffxContext* context,
    const ffxApiHeader* desc) {
    return ffxUpscalerDispatch(context, desc);
}

}  // extern "C"
