#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <cwchar>

namespace gimi_interop
{
// 只覆盖 OptiScaler 自建的 DX12 输出交换链，线程局部状态不会影响
// 其他线程的原生 DX11 游戏设备、ReShade 或其他 D3D11On12 使用者。
inline thread_local unsigned private_dx12_output_depth = 0;
class ScopedPrivateDx12Output
{
  public:
    ScopedPrivateDx12Output() { ++private_dx12_output_depth; }
    ~ScopedPrivateDx12Output() { --private_dx12_output_depth; }
    ScopedPrivateDx12Output(const ScopedPrivateDx12Output&) = delete;
    ScopedPrivateDx12Output& operator=(const ScopedPrivateDx12Output&) = delete;
};
inline bool IsMigotoCaller(void* address)
{
    if (private_dx12_output_depth == 0 || address == nullptr) return false;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) return false;
    // 使用 3DMigoto Loader 特有导出区别同名系统 DLL，不依赖硬编码 RVA。
    if (!GetProcAddress(module,"CBTProc") || !GetProcAddress(module,"D3D11CreateDevice")) return false;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(module,path,MAX_PATH)) return false;
    const wchar_t* slash = std::wcsrchr(path,static_cast<wchar_t>(92));
    return _wcsicmp(slash ? slash+1 : path,L"d3d11.dll") == 0;
}
inline bool RejectPrivateWarningOn12(void* caller, IUnknown* device, UINT flags,
                                      const D3D_FEATURE_LEVEL* levels, UINT levelCount,
                                      IUnknown* const* queues, UINT queueCount, UINT nodeMask,
                                      ID3D11Device** output, ID3D11DeviceContext** context,
                                      D3D_FEATURE_LEVEL* selected)
{
    // Exact legacy warning-layer request inside OUR private output scope only.
    // Do not disable general D3D11On12, RTSS, game or ReShade requests.
    if (!IsMigotoCaller(caller) || !device || flags || levels || levelCount ||
        !queues || queueCount != 1 || !queues[0] || nodeMask || !output || !context)
        return false;
    IUnknown* nativeDevice=nullptr; IUnknown* nativeQueue=nullptr;
    const HRESULT deviceResult=device->QueryInterface(__uuidof(ID3D12Device),reinterpret_cast<void**>(&nativeDevice));
    const HRESULT queueResult=queues[0]->QueryInterface(__uuidof(ID3D12CommandQueue),reinterpret_cast<void**>(&nativeQueue));
    const bool eligible=SUCCEEDED(deviceResult) && nativeDevice && SUCCEEDED(queueResult) && nativeQueue;
    if(nativeDevice) nativeDevice->Release();
    if(nativeQueue) nativeQueue->Release();
    if(!eligible) return false;
    *output=nullptr;*context=nullptr;
    if(selected) *selected=static_cast<D3D_FEATURE_LEVEL>(0);
    return true;
}
inline bool RejectWarningDevice(void* caller, ID3D11Device** device, ID3D11DeviceContext** context,
                                D3D_FEATURE_LEVEL* level)
{
    if (!IsMigotoCaller(caller)) return false;
    // GIMI 为 DX12 交换链创建的 On12 设备仅用于“不支持 DX12”警告层；
    // 请求失败时其源码透明返回原始交换链，游戏的 DX11 模型替换仍保留。
    if (device) *device = nullptr;
    if (context) *context = nullptr;
    if (level) *level = static_cast<D3D_FEATURE_LEVEL>(0);
    return true;
}
}
