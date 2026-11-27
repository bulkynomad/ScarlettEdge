// Real-title proxy path: the output window of ID3D12CommandQueue::PresentX.
// The window runs on its own message-pumping thread and must never block the
// presenting thread, even while the window thread is stalled inside a modal
// WndProc dispatch. Verified deterministically via Win32 events + D3D12 fences
// (zero Sleep loops, zero monitor-hijacking fullscreen toggles).
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "sx_check.h"

#include <windows.h>

#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    void** vtable = *reinterpret_cast<void***>(object);
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}

constexpr int kRelease = 2;
constexpr int kDevCreateCommandQueue = 8;
constexpr int kDevCreateCommittedResource = 27;
constexpr int kDevCreateFence = 36;
constexpr int kQueueSignal = 14;
constexpr int kQueuePresentX = 30;
constexpr int kFenceGetCompletedValue = 8;
constexpr int kFenceSetEventOnCompletion = 9;

constexpr UINT kMsgEnterSyntheticModal = WM_APP + 42;

WNDPROC g_orig_wndproc = nullptr;
HANDLE g_modal_entered = nullptr;
HANDLE g_modal_release = nullptr;
HANDLE g_modal_exited = nullptr;

LRESULT CALLBACK ModalTestSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kMsgEnterSyntheticModal) {
        SetEvent(g_modal_entered);
        WaitForSingleObject(g_modal_release, 10000);
        SetEvent(g_modal_exited);
        return 0;
    }
    return CallWindowProcA(g_orig_wndproc, hwnd, msg, wp, lp);
}

HWND FindPresentWindow() {
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExA(nullptr, hwnd, "ScarlettXPresentWindowClass", nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) return hwnd;
    }
    return nullptr;
}

bool InMenuMode(DWORD thread_id) {
    GUITHREADINFO info = {};
    info.cbSize = sizeof(info);
    return GetGUIThreadInfo(thread_id, &info) && (info.flags & GUI_INMENUMODE) != 0;
}

HANDLE g_presents_done = nullptr;
DWORD WINAPI Watchdog(void*) {
    if (WaitForSingleObject(g_presents_done, 15000) == WAIT_TIMEOUT) {
        std::fprintf(stderr, "test_d3d12_present_window: PresentX did not return while the window thread was blocked\n");
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 1);
    }
    return 0;
}

}  // namespace

int main() {
    _putenv_s("SCARLETTX_REAL_TITLE", "1");
    _putenv_s("SCARLETTX_HEADLESS", "0");

    void* device = nullptr;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = 1;
    const IID device_iid = __uuidof(ID3D12Device);
    SX_REQUIRE(D3D12XboxCreateDevice(nullptr, &params, &device_iid, &device) >= 0);
    SX_REQUIRE(device != nullptr);

    void* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    SX_REQUIRE(Call(device, kDevCreateCommandQueue, &qdesc, static_cast<void*>(nullptr), &queue) == 0);
    SX_REQUIRE(queue != nullptr);

    void* fence = nullptr;
    SX_REQUIRE(Call(device, kDevCreateFence, uintptr_t{0}, uintptr_t{0}, static_cast<void*>(nullptr), &fence) == 0);
    SX_REQUIRE(fence != nullptr);
    HANDLE fence_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SX_REQUIRE(fence_ev != nullptr);

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 256;
    td.Height = 144;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    void* frame = nullptr;
    SX_REQUIRE(Call(device, kDevCreateCommittedResource, &hp, uintptr_t{0}, &td,
                    static_cast<uintptr_t>(D3D12_RESOURCE_STATE_COMMON), static_cast<void*>(nullptr),
                    static_cast<void*>(nullptr), &frame) == 0);
    SX_REQUIRE(frame != nullptr);

    uint64_t plane[8] = {};
    plane[1] = 1;
    plane[2] = reinterpret_cast<uint64_t>(frame);

    SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);

    const HWND hwnd = FindPresentWindow();
    SX_REQUIRE(hwnd != nullptr);
    // Keep the test window off-screen without stealing focus or hijacking any monitor.
    SetWindowPos(hwnd, HWND_BOTTOM, -32000, -32000, 256, 144, SWP_NOACTIVATE | SWP_NOZORDER);

    const DWORD window_thread = GetWindowThreadProcessId(hwnd, nullptr);
    SX_REQUIRE(window_thread != 0);
    SX_REQUIRE(window_thread != GetCurrentThreadId());

    // Alt or F10 alone does not put the window into menu mode.
    DWORD_PTR result = 0;
    SX_REQUIRE(SendMessageTimeoutA(hwnd, WM_SYSCOMMAND, SC_KEYMENU, 0, SMTO_ABORTIFHUNG, 5000, &result) != 0);
    SX_REQUIRE(!InMenuMode(window_thread));

    // Closing the window is not a way to take the output away from the title.
    SX_REQUIRE(SendMessageTimeoutA(hwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 5000, &result) != 0);
    SX_REQUIRE(IsWindow(hwnd));

    // Deterministically block the window thread inside a modal WndProc wait and verify
    // PresentX + GPU queue fence signaling proceed without deadlocking on window_thread.
    g_modal_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_modal_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_modal_exited = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SX_REQUIRE(g_modal_entered && g_modal_release && g_modal_exited);

    g_orig_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ModalTestSubclassProc)));
    SX_REQUIRE(g_orig_wndproc != nullptr);

    SX_REQUIRE(PostMessageA(hwnd, kMsgEnterSyntheticModal, 0, 0) != 0);
    SX_REQUIRE(WaitForSingleObject(g_modal_entered, 5000) == WAIT_OBJECT_0);
    SX_REQUIRE(WaitForSingleObject(g_modal_exited, 0) == WAIT_TIMEOUT);

    g_presents_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SX_REQUIRE(g_presents_done != nullptr);
    const HANDLE watchdog = CreateThread(nullptr, 0, &Watchdog, nullptr, 0, nullptr);
    SX_REQUIRE(watchdog != nullptr);

    for (uint64_t i = 1; i <= 3; ++i) {
        SX_REQUIRE(Call(queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr)) == 0);
        SX_REQUIRE(Call(queue, kQueueSignal, fence, static_cast<uintptr_t>(i)) == 0);
        if (static_cast<uint64_t>(Call(fence, kFenceGetCompletedValue)) < i) {
            SX_REQUIRE(Call(fence, kFenceSetEventOnCompletion, static_cast<uintptr_t>(i),
                            reinterpret_cast<uintptr_t>(fence_ev)) == 0);
            SX_REQUIRE(WaitForSingleObject(fence_ev, 5000) == WAIT_OBJECT_0);
        }
    }

    // Window thread remained blocked throughout all three PresentX + fence completions.
    SX_REQUIRE(WaitForSingleObject(g_modal_exited, 0) == WAIT_TIMEOUT);

    SetEvent(g_presents_done);
    WaitForSingleObject(watchdog, INFINITE);
    CloseHandle(watchdog);
    CloseHandle(g_presents_done);

    SetEvent(g_modal_release);
    SX_REQUIRE(WaitForSingleObject(g_modal_exited, 5000) == WAIT_OBJECT_0);
    SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));

    CloseHandle(g_modal_entered);
    CloseHandle(g_modal_release);
    CloseHandle(g_modal_exited);
    CloseHandle(fence_ev);

    Call(frame, kRelease);
    Call(fence, kRelease);

    // The window goes away with the queue.
    SX_REQUIRE(Call(queue, kRelease) == 0);
    SX_REQUIRE(!IsWindow(hwnd));
    Call(device, kRelease);

    std::printf("test_d3d12_present_window: OK\n");
    return 0;
}
