#pragma once

#include <framegen/IFGFeature_Dx12.h>

#include <proxies/Streamline_Proxy.h>

class DLSSG_Dx12 : public virtual IFGFeature_Dx12
{
  private:
    uint32_t _width = 0;
    uint32_t _height = 0;
    std::optional<bool> _haveHudless = std::nullopt;

    sl::ViewportHandle viewport { 0 };
    sl::FrameToken* frameToken = nullptr;

    ID3D12Fence* dlssgFence[BUFFER_COUNT] = {};
    UINT64 lastOptionFrame = 0;

    bool Dispatch();

    // Raw numFramesToGenerateMax exactly as the runtime reported it at swapchain/context
    // creation, before MfgUnlock raises it. -1 until the first successful slDLSSGGetState.
    // Logged per dispatch so a rotated-away startup line cannot hide the runtime's own value.
    int _runtimeReportedMaxInterpolation = -1;

    // Raw DLSSGStatus bitmask from the same query. Non-zero flags name the reason the
    // runtime degrades (no Reflex at runtime, GetCurrentBackBufferIndex not called, ...).
    unsigned _runtimeReportedStatus = 0;

    // Dispatch() runs at present rate and used to resend identical DLSSG/Reflex options every
    // frame (thousands of SetOptions calls per session in HSR, each one a driver round-trip).
    // The runtime only needs a fresh call when a value actually changes, plus a rare keepalive
    // in case a runtime reset silently dropped them. Deactivate() invalidates both caches
    // because it sends eOff outside of Dispatch.
    sl::DLSSGMode _lastDlssgModeSent = sl::DLSSGMode::eOff;
    unsigned int _lastDlssgNumSent = 0;
    float _lastDlssgDynamicTargetSent = 0.0f;
    bool _dlssgOptionsValid = false;
    uint64_t _dlssgOptionsSentAtPresent = 0;

    bool _lastReflexMarkersSent = false;
    bool _reflexOptionsValid = false;
    uint64_t _reflexOptionsSentAtPresent = 0;

  protected:
    void ReleaseObjects() override final;
    void CreateObjects(ID3D12Device* InDevice) override final;

  public:
    // IFGFeature
    const char* Name() override final { return "DLSSG"; };
    feature_version Version() override final;
    HWND Hwnd() override final;

    // IFGFeature_Dx12
    bool CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                         IDXGISwapChain** swapChain, bool readyToRelease) override final;
    bool CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd, DXGI_SWAP_CHAIN_DESC1* desc,
                          DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGISwapChain1** swapChain,
                          bool readyToRelease) override final;

    bool ReleaseSwapchain(HWND hwnd) override final;

    void CreateContext(ID3D12Device* device, FG_Constants& fgConstants) override final;
    void Activate() override final;
    void Deactivate() override final;
    void DestroyFGContext() override final;
    bool Shutdown() override final;

    void EvaluateState(ID3D12Device* device, FG_Constants& fgConstants) override final;

    bool Present() override final;

    bool SetResource(Dx12Resource* inputResource) override final;
    void SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) override final;

    void* FrameGenerationContext() override final;
    void* SwapchainContext() override final;

    DLSSG_Dx12() : IFGFeature_Dx12(), IFGFeature()
    {
        if (StreamlineProxy::Module() == nullptr)
            StreamlineProxy::LoadStreamline();

        if (StreamlineProxy::Module() != nullptr && !StreamlineProxy::IsD3D12Inited() &&
            State::Instance().currentD3D12Device != nullptr)
        {
            StreamlineProxy::InitWithD3D12(State::Instance().currentD3D12Device);
        }
    }

    ~DLSSG_Dx12();

    // Inherited via IFGFeature_Dx12
    bool SetInterpolatedFrameCount(UINT interpolatedFrameCount) override;
};
