#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <string>
#include <cstdint>
#include <filesystem>
#include "NativeFinalOutput.h"

namespace ReShadeFinalOutput
{
// ReShade 6.8 官方公开导出的 ABI：仅调用创建/更新/销毁，不访问 runtime 布局。
using Create = bool(*)(uint32_t,void*,void*,void*,const char*,void**);
using Update = void(*)(void*);
using Destroy = void(*)(void*);
class Runtime
{
    HMODULE module = nullptr;
    void* runtime = nullptr;
    Update update = nullptr;
    Destroy destroy = nullptr;
    ULONGLONG nextTry = 0;
    bool tried = false, automatic = false;
  public:
    bool HasRuntime() const { return runtime != nullptr; }
    void ForgetDuringShutdown() { runtime = nullptr; }
    void Reset()
    {
        if (runtime && destroy && module && GetModuleHandleW(L"ReShade64.dll") == module) destroy(runtime);
        runtime = nullptr; nextTry = 0; tried = false; automatic = false;
    }
    // 资源初始化仍放在 FG 锁之外；这里只准备，不提前绘制菜单。
    void Prepare(IDXGISwapChain4* swapchain,ID3D12CommandQueue* queue,HWND window) { Present(swapchain,queue,window,false); }
    void Present(IDXGISwapChain4* swapchain,ID3D12CommandQueue* queue,HWND window,bool render=true)
    {
        if (!swapchain || !queue || automatic) return;
        // 只用于配套 GIMI 的透明 DX12 路线；普通 Opti/ReShade 路线保持自动接管。
        HMODULE gimi = GetModuleHandleW(L"d3d11.dll");
        const bool paired=gimi && GetProcAddress(gimi,"XXMIPrivateDx12PassthroughVersion")!=nullptr;
        // Legacy qualification comes from the actual source Present owner, not
        // the ambiguous first module named d3d11.dll (which may be system DX11).
        const bool legacy=NativeFinalOutput::ContainsLegacy(window);
        if (!paired && !legacy) return;
        if (!runtime && GetTickCount64() < nextTry) return;
        if (!runtime)
        {
            // 已有 ReShade 交换链代理时由自动 runtime 渲染，避免重复滤镜。
            constexpr GUID nativeObject = {0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
            IUnknown* native = nullptr;
            if (SUCCEEDED(swapchain->QueryInterface(nativeObject,reinterpret_cast<void**>(&native))) && native)
            {
                native->Release(); automatic = true;
                LOG_INFO("XXMI coexist: final swapchain already has ReShade proxy; automatic runtime retained");
                return;
            }
            module = GetModuleHandleW(L"ReShade64.dll");
            if (!module) {nextTry=GetTickCount64()+1000;return;}
            const auto create = reinterpret_cast<Create>(GetProcAddress(module,"ReShadeCreateEffectRuntime"));
            update = reinterpret_cast<Update>(GetProcAddress(module,"ReShadeUpdateAndPresentEffectRuntime"));
            destroy = reinterpret_cast<Destroy>(GetProcAddress(module,"ReShadeDestroyEffectRuntime"));
            if (!create || !update || !destroy) {nextTry=GetTickCount64()+5000;return;}
            wchar_t executable[MAX_PATH] = {};
            if (!GetModuleFileNameW(nullptr,executable,MAX_PATH)) return;
            const auto utf8Config = (std::filesystem::path(executable).parent_path()/L"ReShade2.ini").u8string();
            const std::string config(reinterpret_cast<const char*>(utf8Config.data()),utf8Config.size());
            HWND actualWindow=nullptr;
            const HRESULT windowResult=swapchain->GetHwnd(&actualWindow);
            if (!tried) LOG_INFO("XXMI coexist: final FG HWND {:X}, expected game HWND {:X}, query HRESULT {:X}",(size_t)actualWindow,(size_t)window,(UINT)windowResult);
            if (legacy && !paired && (FAILED(windowResult) || actualWindow != window))
            {
                if (!tried) LOG_WARN("Rocket/legacy GIMI compatibility: final HWND validation failed; no duplicate/foreign runtime created");
                tried=true;nextTry=GetTickCount64()+5000;return;
            }
            ID3D12Device* device = nullptr;
            const HRESULT deviceResult = swapchain->GetDevice(IID_PPV_ARGS(&device));
            const bool created = SUCCEEDED(deviceResult) && device &&
                create(0xc000u,device,queue,swapchain,config.c_str(),&runtime);
            if (device) device->Release();
            if (!created)
            {
                if (!tried) LOG_WARN("XXMI coexist: explicit final DX12 ReShade runtime creation failed; retry pending, device HRESULT {:X}",(UINT)deviceResult);
                tried=true;nextTry=GetTickCount64()+5000;return;
            }
            LOG_INFO("XXMI coexist: explicit final DX12 ReShade runtime created on FG output, hwnd {:X}, config {}",(size_t)window,config);
        }
        // 官方 UpdateAndPresentEffectRuntime 仅处理效果与提交其命令，
        // 不调用 IDXGISwapChain::Present；真正呈现仍由 Streamline 完成。
        if (render) {
            // 原HoYoShade发布核心保持原字节；配套addon消费效果机会，只画GUI。
            const auto addon=GetModuleHandleW(L"FsrBridgeDepthAddon.addon64");
            using PrepareGui=BOOL (WINAPI *)(void*);
            const auto prepare=addon ? reinterpret_cast<PrepareGui>(GetProcAddress(addon,"HoYoShadePrepareFinalGuiV1")) : nullptr;
            if(prepare) prepare(runtime);
            update(runtime);
        }
    }
};
}
