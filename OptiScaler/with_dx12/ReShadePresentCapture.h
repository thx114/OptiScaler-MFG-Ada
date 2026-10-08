#pragma once

#include "PresentCapture.h"
#include "NativeDx12Compat.h"
#include <cwchar>
#include <string>
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
    // A legacy GIMI shell can return itself for the native IID. Reach the
    // public 3/2 interface first and validate the actual system DXGI Present.
    auto selected = NativeDx12Compat::ResolvePresentTarget(proxy, [](IDXGISwapChain* candidate) {
        const auto entry = (*reinterpret_cast<void***>(candidate))[8];
        HMODULE owner = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(entry), &owner)) return false;
        wchar_t actual[MAX_PATH] = {}, system[MAX_PATH] = {};
        if (!GetModuleFileNameW(owner, actual, MAX_PATH) || !GetSystemDirectoryW(system, MAX_PATH)) return false;
        std::wstring expected(system); expected += L"\\dxgi.dll";
        return _wcsicmp(actual, expected.c_str()) == 0;
    });
    if (!selected) return nullptr;
    IDXGISwapChain* native = selected.Detach();

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
    LOG_INFO("FG companion capture: system DXGI post-wrapper / pre-flip DX11 capture installed (public interface discovery)");
    return native;
}

}
