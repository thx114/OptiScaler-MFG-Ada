#pragma once

#include "PresentCapture.h"
#include <dxgi.h>
#include <detours/detours.h>
#include <mutex>

// LOG_INFO/LOG_WARN are supplied by the host (OptiScaler or the smoke-test logger).
namespace ReShadePresentCapture
{
using NativePresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
inline NativePresent capturePresentOriginal = nullptr;
inline void* capturePresentAddress = nullptr;
inline std::mutex captureInstallMutex;

inline HRESULT STDMETHODCALLTYPE CaptureNativePresent(IDXGISwapChain* swapchain, UINT interval, UINT flags)
{
    if (PresentCapture::current != nullptr)
        PresentCapture::current->TryCopy(swapchain, (flags & DXGI_PRESENT_TEST) != 0);
    return capturePresentOriginal(swapchain, interval, flags);
}

inline IDXGISwapChain* Install(IDXGISwapChain* proxy)
{
    // ReShade 6.8.0 source/com_utils.hpp and dxgi/dxgi_swapchain.cpp:
    // on_present (including addon processing) precedes _orig->Present.
    constexpr GUID unwrappedObject = {
        0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
    IDXGISwapChain* native = nullptr;
    if (proxy == nullptr || FAILED(proxy->QueryInterface(unwrappedObject, (void**)&native)) || native == nullptr)
        return nullptr;
    if (native == proxy)
    {
        native->Release();
        return nullptr;
    }

    const auto address = (*reinterpret_cast<void***>(native))[8];
    std::lock_guard lock(captureInstallMutex);
    if (capturePresentAddress != nullptr)
    {
        if (capturePresentAddress == address)
            return native;
        LOG_WARN("FG companion capture: different native Present implementation; retaining legacy copy");
        native->Release();
        return nullptr;
    }

    capturePresentOriginal = reinterpret_cast<NativePresent>(address);
    LONG result = DetourTransactionBegin();
    if (result == NO_ERROR)
    {
        result = DetourUpdateThread(GetCurrentThread());
        if (result == NO_ERROR)
            result = DetourAttach(reinterpret_cast<PVOID*>(&capturePresentOriginal), CaptureNativePresent);
        if (result == NO_ERROR)
            result = DetourTransactionCommit();
        else
            DetourTransactionAbort();
    }
    if (result != NO_ERROR)
    {
        LOG_WARN("FG companion capture: native Present hook failed {}, retaining legacy copy", result);
        capturePresentOriginal = nullptr;
        native->Release();
        return nullptr;
    }
    capturePresentAddress = address;
    LOG_INFO("FG companion capture: ReShade post-addon / pre-flip DX11 capture installed");
    return native;
}

}
