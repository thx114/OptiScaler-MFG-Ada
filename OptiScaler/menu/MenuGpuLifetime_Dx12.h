#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <utility>

// Match menu command allocator reuse and resize cleanup to the queue that
// submitted the menu, rather than the independent interop copy queue.
class MenuGpuLifetime_Dx12
{
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    HANDLE event = nullptr;
    UINT64 nextValue = 0;
    std::array<UINT64, 8> slots {};
    bool submissionUnproven = false;
    HRESULT Wait(UINT64 value, DWORD timeout) {
        if (!value) return S_OK;
        if (!fence || submissionUnproven) return E_FAIL;
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
        if (completed >= value) return S_OK;
        auto hr = fence->SetEventOnCompletion(value, event);
        if (FAILED(hr)) return hr;
        auto result = WaitForSingleObject(event, timeout);
        return result == WAIT_OBJECT_0 ? S_OK :
            result == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) : HRESULT_FROM_WIN32(GetLastError());
    }
  public:
    MenuGpuLifetime_Dx12() = default;
    MenuGpuLifetime_Dx12(const MenuGpuLifetime_Dx12&) = delete;
    ~MenuGpuLifetime_Dx12() { if (event) CloseHandle(event); }
    HRESULT Initialize(ID3D12Device* device, ID3D12CommandQueue* submittedQueue) {
        if (!device || !submittedQueue) return E_POINTER;
        if (fence && queue.Get() == submittedQueue) return submissionUnproven ? E_FAIL : S_OK;
        if (fence) {
            auto hr = WaitIdle();
            if (FAILED(hr)) return hr;
        }
        Microsoft::WRL::ComPtr<ID3D12Fence> replacement;
        auto hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&replacement));
        if (FAILED(hr)) return hr;
        if (!event) event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!event) return HRESULT_FROM_WIN32(GetLastError());
        fence = std::move(replacement);
        queue = submittedQueue;
        nextValue = 0;
        slots.fill(0);
        submissionUnproven = false;
        return S_OK;
    }
    HRESULT WaitSlot(UINT index, DWORD timeout = 5000) {
        if (index >= slots.size()) return E_INVALIDARG;
        return Wait(slots[index], timeout);
    }
    HRESULT SignalSubmitted(UINT index) {
        if (!queue || !fence || index >= slots.size()) return E_INVALIDARG;
        auto value = ++nextValue;
        auto hr = queue->Signal(fence.Get(), value);
        if (SUCCEEDED(hr)) slots[index] = value;
        else submissionUnproven = true;
        return hr;
    }
    HRESULT WaitIdle(DWORD timeout = 5000) { return Wait(nextValue, timeout); }
    HRESULT Reset() {
        auto hr = WaitIdle();
        if (FAILED(hr)) return hr;
        queue.Reset(); fence.Reset(); slots.fill(0); nextValue = 0;
        submissionUnproven = false;
        return S_OK;
    }
};
