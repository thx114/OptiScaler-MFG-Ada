#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <utility>

// GPU-only handoff for a stable shared input allocation. Destruction/rebuild
// still requires the owner's CPU retirement path, not merely this GPU wait.
class SharedInputReadFence_Dx11
{
    Microsoft::WRL::ComPtr<ID3D12Device> device12;
    Microsoft::WRL::ComPtr<ID3D11Device5> device11;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> read12;
    Microsoft::WRL::ComPtr<ID3D11Fence> read11;
    UINT64 value = 0;
  public:
    HRESULT Enqueue(ID3D12Device* dev, ID3D12CommandQueue* submittedQueue,
                    ID3D11DeviceContext* producer)
    {
        if (!dev || !submittedQueue || !producer) return E_POINTER;
        Microsoft::WRL::ComPtr<ID3D11Device> rawDevice;
        Microsoft::WRL::ComPtr<ID3D11Device5> currentDevice;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context;
        producer->GetDevice(&rawDevice);
        auto hr = rawDevice.As(&currentDevice);
        if (FAILED(hr)) return hr;
        hr = producer->QueryInterface(IID_PPV_ARGS(&context));
        if (FAILED(hr)) return hr;
        if (read12 && (device12.Get() != dev || device11.Get() != currentDevice.Get() || queue.Get() != submittedQueue))
            return E_INVALIDARG; // Owner falls back to CPU retirement on a context change.
        if (!read12)
        {
            Microsoft::WRL::ComPtr<ID3D12Fence> newRead12;
            Microsoft::WRL::ComPtr<ID3D11Fence> newRead11;
            hr = dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&newRead12));
            if (FAILED(hr)) return hr;
            HANDLE handle = nullptr;
            hr = dev->CreateSharedHandle(newRead12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
            if (FAILED(hr)) return hr;
            hr = currentDevice->OpenSharedFence(handle, IID_PPV_ARGS(&newRead11));
            CloseHandle(handle);
            if (FAILED(hr)) return hr;
            device12 = dev; device11 = currentDevice; queue = submittedQueue;
            read12 = std::move(newRead12); read11 = std::move(newRead11);
        }
        if (read12->GetCompletedValue() == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
        hr = submittedQueue->Signal(read12.Get(), ++value);
        if (FAILED(hr)) return hr;
        // Subsequent D3D11 writes wait on the GPU, while the CPU can keep recording.
        return context->Wait(read11.Get(), value);
    }
};
