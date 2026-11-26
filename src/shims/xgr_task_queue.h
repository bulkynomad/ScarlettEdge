#pragma once

// Internal interface of the xgameruntime task-queue / async engine.
// The guest-visible XTaskQueue*/XAsync* entry points live in xgr_task_queue.cpp.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "scarlettx/gdk/xgameruntime_abi.h"

namespace xgr {

// State of an async operation implemented by the HLE itself (XUser, XGameSave, ...).
struct HleAsyncState {
    std::mutex mutex;
    XGameResult status = XGR_E_PENDING;
    bool completed = false;
    bool canceled = false;
    size_t result_size = 0;
    std::vector<uint8_t> result_payload;
    void* result_handle = nullptr;
    XAsyncBlock* block = nullptr;
    std::function<void(HleAsyncState*)> work_fn;
};

// Starts `work_fn` on the work port of the block's queue. `work_fn` must call
// CompleteHleAsync. A non-zero result size keeps the state attached to the block
// until ReleaseHleAsyncResult, mirroring an XAsync payload that awaits XAsyncGetResult.
XGameResult BeginHleAsync(XAsyncBlock* async, std::function<void(HleAsyncState*)> work_fn);
void CompleteHleAsync(HleAsyncState* state, XGameResult hr, size_t result_size = 0);
std::shared_ptr<HleAsyncState> GetHleAsyncState(XAsyncBlock* async);
void ReleaseHleAsyncResult(XAsyncBlock* async);

void EnsureProcessTaskQueue();
bool IsTaskQueueHandle(void* handle);

// Terminates the process queue and stops the engine's threads. They restart on demand.
void ShutdownTaskSystem();

}  // namespace xgr
