#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <iostream>
#include <cassert>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#include "../OptiScaler/with_dx12/ReShadePresentCapture.h"
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if (FAILED(hr)) { std::cerr << "HRESULT " << std::hex << hr << '\n'; std::abort(); } }

// Model ReShade's documented proxy order with real D3D11 resources/native Present.
// This is not the external NR addon and does not load any game/plugin.
class Proxy final : public IDXGISwapChain
{
  public:
    IDXGISwapChain* real;
    ID3D11DeviceContext* context;
    ID3D11RenderTargetView* target = nullptr;
    Proxy(IDXGISwapChain* sc, ID3D11DeviceContext* ctx) : real(sc), context(ctx) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        constexpr GUID unwrap {0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
        if (!out) return E_POINTER;
        if (id == unwrap) { *out = real; real->AddRef(); return S_OK; }
        return real->QueryInterface(id, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return real->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return real->Release(); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID a, UINT b, const void* c) override { return real->SetPrivateData(a,b,c); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID a, const IUnknown* b) override { return real->SetPrivateDataInterface(a,b); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID a, UINT* b, void* c) override { return real->GetPrivateData(a,b,c); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID a, void** b) override { return real->GetParent(a,b); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID a, void** b) override { return real->GetDevice(a,b); }
    HRESULT STDMETHODCALLTYPE Present(UINT interval, UINT flags) override
    {
        if (!(flags & DXGI_PRESENT_TEST))
        {
            const float addonColor[] {0,1,0,1};
            context->ClearRenderTargetView(target, addonColor);
        }
        return real->Present(interval, flags);
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT a, REFIID b, void** c) override { return real->GetBuffer(a,b,c); }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL a, IDXGIOutput* b) override { return real->SetFullscreenState(a,b); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* a, IDXGIOutput** b) override { return real->GetFullscreenState(a,b); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* a) override { return real->GetDesc(a); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT a, UINT b, UINT c, DXGI_FORMAT d, UINT e) override { return real->ResizeBuffers(a,b,c,d,e); }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* a) override { return real->ResizeTarget(a); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** a) override { return real->GetContainingOutput(a); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* a) override { return real->GetFrameStatistics(a); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* a) override { return real->GetLastPresentCount(a); }
};
struct SmokeCopyContext
{
    ID3D11DeviceContext* context;
    ID3D11Texture2D* source;
    ID3D11Texture2D* destination;
    int copies = 0;
};
static bool Copy(void* data)
{
    auto& copy = *static_cast<SmokeCopyContext*>(data);
    copy.context->CopyResource(copy.destination, copy.source);
    ++copy.copies;
    return true;
}
int main()
{
    WNDCLASSW wc {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OptiPresentCaptureSmoke";
    assert(RegisterClassW(&wc));
    HWND window = CreateWindowW(wc.lpszClassName, L"hidden capture test", WS_POPUP,
                                0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
    assert(window);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                           D3D11_SDK_VERSION, &device, nullptr, &context));
    ComPtr<IDXGIDevice> dxgiDevice;
    Check(device.As(&dxgiDevice));
    ComPtr<IDXGIAdapter> adapter;
    Check(dxgiDevice->GetAdapter(&adapter));
    ComPtr<IDXGIFactory2> factory;
    Check(adapter->GetParent(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width=64; desc.Height=64; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> native;
    Check(factory->CreateSwapChainForHwnd(device.Get(),window,&desc,nullptr,nullptr,&native));
    Proxy proxy(native.Get(), context.Get());
    assert(ReShadePresentCapture::Install(native.Get()) == nullptr); // No proxy => no hook.
    ComPtr<IDXGISwapChain> capture;
    capture.Attach(ReShadePresentCapture::Install(&proxy));
    assert(capture);
    auto* installedAgain = ReShadePresentCapture::Install(&proxy);
    assert(installedAgain == capture.Get());
    installedAgain->Release();
    for (int frame=0; frame<4; ++frame)
    {
        ComPtr<ID3D11Texture2D> source;
        Check(native->GetBuffer(0,IID_PPV_ARGS(&source)));
        ComPtr<ID3D11RenderTargetView> target;
        Check(device->CreateRenderTargetView(source.Get(),nullptr,&target));
        proxy.target=target.Get();
        D3D11_TEXTURE2D_DESC textureDesc {};
        source->GetDesc(&textureDesc);
        textureDesc.BindFlags=0; textureDesc.MiscFlags=0;
        textureDesc.Usage=D3D11_USAGE_STAGING; textureDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        Check(device->CreateTexture2D(&textureDesc,nullptr,&staging));
        const float before[] {1,0,0,1};
        context->ClearRenderTargetView(target.Get(),before);
        SmokeCopyContext copy {context.Get(),source.Get(),staging.Get()};
        Copy(&copy); // Baseline is red.
        PresentCapture::Request request {capture.Get(),&copy,Copy};
        {
            PresentCapture::Scope scope(request);
            Check(proxy.Present(0,DXGI_PRESENT_TEST));
            assert(!request.attempted);
            Check(proxy.Present(0,0)); // Fake addon writes green; production native hook captures it.
        }
        assert(request.attempted && request.succeeded && copy.copies==2);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        auto* pixel=static_cast<unsigned char*>(mapped.pData);
        assert(pixel[0]==0 && pixel[1]==255 && pixel[2]==0 && pixel[3]==255);
        context->Unmap(staging.Get(),0);
    }
    // Detach while no call is in flight so this test can cleanly release DXGI.
    Check(HRESULT_FROM_WIN32(DetourTransactionBegin()));
    Check(HRESULT_FROM_WIN32(DetourUpdateThread(GetCurrentThread())));
    Check(HRESULT_FROM_WIN32(DetourDetach(reinterpret_cast<PVOID*>(&ReShadePresentCapture::capturePresentOriginal),
                                         ReShadePresentCapture::CaptureNativePresent)));
    Check(HRESULT_FROM_WIN32(DetourTransactionCommit()));
    capture.Reset(); native.Reset();
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName,wc.hInstance);
    std::cout << "PASS D3D11 WARP: production native hook, TEST gate, repeat install, 4 post-addon green pre-flip readbacks\n";
}
