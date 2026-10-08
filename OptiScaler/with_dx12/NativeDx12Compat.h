#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <utility>

namespace NativeDx12Compat
{
using Microsoft::WRL::ComPtr;
// No foreign object layout, private fields, pointer offsets or version-marker spoofing.
// Probe only public COM interfaces. Returned interfaces retain their own reference.
inline constexpr GUID StreamlineNativeObject =
    {0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
inline ComPtr<IUnknown> Identity(IUnknown* object)
{
    ComPtr<IUnknown> current;
    if (!object || FAILED(object->QueryInterface(IID_PPV_ARGS(&current))) || !current) return current;
    // The existing Opt Streamline unwrapping contract, with bounded COM queries
    // and retained references. No native object fields or offsets are read.
    for (unsigned depth=0; depth<4; ++depth)
    {
        ComPtr<IUnknown> native, identity;
        if (FAILED(current->QueryInterface(StreamlineNativeObject,reinterpret_cast<void**>(native.GetAddressOf()))) ||
            !native || FAILED(native->QueryInterface(IID_PPV_ARGS(&identity))) || !identity || identity.Get()==current.Get()) break;
        current=std::move(identity);
    }
    return current;
}
inline bool SameObject(IUnknown* a, IUnknown* b)
{
    auto left=Identity(a),right=Identity(b);
    return left && right && left.Get()==right.Get();
}
struct Recovery
{
    ComPtr<IDXGISwapChain4> output;
    HRESULT directResult = E_NOINTERFACE;
    HRESULT probeResult = E_NOINTERFACE;
    UINT throughInterface = 0;
    bool directProbePerformed = false;
};
inline Recovery RecoverOutput(IDXGISwapChain* shell, ID3D12Device* expectedDevice, HWND expectedWindow, bool legacyShell = false)
{
    Recovery result;
    if (!shell || !expectedDevice || !expectedWindow) { result.probeResult = E_INVALIDARG; return result; }
    // Legacy GIMI can QI the native object then discard that returned reference
    // when rejecting SwapChain4. Do not invoke the known legacy failure path.
    if (!legacyShell)
    {
        ComPtr<IDXGISwapChain4> direct;
        result.directProbePerformed = true;
        result.directResult = shell->QueryInterface(IID_PPV_ARGS(&direct));
        // Existing full-capability proxies remain intact (Streamline/ReShade).
        if (SUCCEEDED(result.directResult) && direct) return result;
    }
    auto validate = [&](IUnknown* view, UINT level) {
        if (legacyShell && view == static_cast<IUnknown*>(shell)) return false;
        ComPtr<IDXGISwapChain4> native;
        result.probeResult = view->QueryInterface(IID_PPV_ARGS(&native));
        if (FAILED(result.probeResult) || !native) return false;
        ComPtr<ID3D12Device> device;
        result.probeResult = native->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(result.probeResult) || !device) return false;
        if (!SameObject(device.Get(), expectedDevice)) { result.probeResult=DXGI_ERROR_INVALID_CALL; return false; }
        HWND actualWindow = nullptr;
        result.probeResult = native->GetHwnd(&actualWindow);
        if (FAILED(result.probeResult)) return false;
        if (actualWindow != expectedWindow) { result.probeResult=DXGI_ERROR_INVALID_CALL; return false; }
        result.output = std::move(native); result.throughInterface = level; return true;
    };
    ComPtr<IDXGISwapChain3> level3;
    if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&level3))) && level3 && validate(level3.Get(), 3)) return result;
    ComPtr<IDXGISwapChain2> level2;
    if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&level2))) && level2) validate(level2.Get(), 2);
    return result;
}
inline constexpr GUID ReShadeNativeObject =
    {0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
// accept() validates the native implementation before any Present hook is installed.
// A legacy shell can return itself for the ReShade IID, so also probe public 3/2 views.
template<class Accept> inline ComPtr<IDXGISwapChain> ResolvePresentTarget(IDXGISwapChain* shell, Accept accept)
{
    ComPtr<IDXGISwapChain> result;
    if (!shell) return result;
    auto from = [&](IDXGISwapChain* view) {
        ComPtr<IDXGISwapChain> native;
        if (SUCCEEDED(view->QueryInterface(ReShadeNativeObject, reinterpret_cast<void**>(native.GetAddressOf()))) &&
            native && native.Get() != shell && accept(native.Get())) { result = std::move(native); return true; }
        if (view != shell && accept(view)) { result = view; return true; }
        return false;
    };
    if (from(shell)) return result;
    ComPtr<IDXGISwapChain3> level3;
    if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&level3))) && level3 && from(level3.Get())) return result;
    ComPtr<IDXGISwapChain2> level2;
    if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&level2))) && level2) from(level2.Get());
    return result;
}
} // namespace NativeDx12Compat
