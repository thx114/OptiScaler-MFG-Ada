#include "pch.h"

#include "DLSSG_Dx12.h"
#include "DlssgPausePolicy.h"
#include "Kcd2Hdr.h"
#if defined(OPTISCALER_RTX40_MFG)
#include "MfgUnlock.h"
#endif

#include <hudfix/Hudfix_Dx12.h>
#include <menu/menu_overlay_dx.h>
#include <resource_tracking/ResTrack_dx12.h>

#include <hooks/Reflex_Hooks.h>
#include <hooks/Streamline_Hooks.h>
#include <hooks/DxgiFactory_Hooks.h>

#include <magic_enum.hpp>

#include <DirectXMath.h>

using namespace DirectX;

static int ResolveDlssgRuntimeMaximum(unsigned int nativeMaximum)
{
    const auto safeNativeMaximum = std::max(1u, nativeMaximum);
#if defined(OPTISCALER_RTX40_MFG)
    return static_cast<int>(MfgUnlock::EffectiveMax(safeNativeMaximum));
#else
    return static_cast<int>(safeNativeMaximum);
#endif
}

#if defined(OPTISCALER_RTX40_MFG)
static bool CanDispatchDlssg(MfgUnlock::Failure failure) { return failure != MfgUnlock::Failure::RollbackFailed; }
#endif

static bool CommitDlssgDispatchOptions(sl::Result result, const sl::DLSSGOptions& options,
                                       int& acceptedFramesToInterpolate)
{
    if (result != sl::Result::eOk)
    {
        LOG_ERROR("Couldn't set DLSSG options, error: {}", magic_enum::enum_name(result));
        return false;
    }

    acceptedFramesToInterpolate = static_cast<int>(options.numFramesToGenerate);
    const int acceptedPacing = options.mode == sl::DLSSGMode::eOff ? 0 : acceptedFramesToInterpolate;
    auto& state = State::Instance();
    state.dlssgLastSetMode = options.mode;
    state.dlssgDetectedInterpolationCount = acceptedPacing;
    ReflexHooks::setDlssgFrameCount(acceptedPacing);
    return true;
}

feature_version DLSSG_Dx12::Version()
{
    if (StreamlineProxy::LoadStreamline())
    {
        auto ver = StreamlineProxy::Version();
        return ver;
    }

    return { 0, 0, 0 };
}

HWND DLSSG_Dx12::Hwnd() { return _hwnd; }

bool DLSSG_Dx12::CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                 IDXGISwapChain** swapChain, bool readyToRelease)
{
    Kcd2Hdr::ApplyQuirk();
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == desc->OutputWindow)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("FG swapchain already created for the same output window!");
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              desc->BufferCount, desc->BufferDesc.Width, desc->BufferDesc.Height,
                              desc->BufferDesc.Format, desc->Flags) == S_OK;

            *swapChain = State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating new swapchain without releasing old one,
        // we need to release it to avoid errors
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            ReleaseSwapchain(_hwnd);

            // Not sure why but XeFG sometimes doesn't release the swapchain properly
            // so we force release it here to be able to recreate swapchain for same hwnd
            if (State::Instance().currentRealSwapchain != nullptr)
            {
                UINT release = 0;
                do
                {
                    release = State::Instance().currentRealSwapchain->Release();
                    LOG_DEBUG("Releasing swapchain, ref count: {}", release);
                } while (release > 0);
            }
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    if (StreamlineProxy::Module() == nullptr)
    {
        LOG_ERROR("Streamline proxy can't find sl.interposer.dll!");
        return false;
    }

    if (!StreamlineProxy::IsD3D12Inited())
    {
        if (State::Instance().currentD3D12Device != nullptr &&
            !StreamlineProxy::InitWithD3D12(State::Instance().currentD3D12Device))
        {
            return false;
        }
    }

    _width = desc->BufferDesc.Width;
    _height = desc->BufferDesc.Height;

    IDXGIFactory* slFactory = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &slFactory))
    {
        StreamlineProxy::UpgradeInterface()((void**) &factory);
        DxgiFactoryHooks::HookToDLSSGFactory(factory);
    }
    else
    {
        slFactory->Release();
    }

    StreamlineProxy::SetFeatureLoaded()(sl::kFeatureDLSS_G, true);

    desc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    if (State::Instance().gameName == "KCD2")
    {
        // KCD2 waits on this handle. Declare application ownership so Streamline
        // does not also consume it and stall its flip queue.
        desc->Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        LOG_INFO("KCD2: requesting application-owned frame-latency waitable");
    }

    auto result = S_FALSE;

    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        result = factory->CreateSwapChain(cmdQueue, desc, swapChain);
    }

    if (result != S_OK)
    {
        LOG_ERROR("CreateSwapChain error: {:X}", (UINT) result);
        return false;
    }

#if defined(OPTISCALER_RTX40_MFG)
    MfgUnlock::TryApply();
#endif
    sl::DLSSGState dlssgState {};
    sl::DLSSGOptions dlssgOptions {};
    if (StreamlineProxy::DLSSGGetState()(viewport, dlssgState, &dlssgOptions) == sl::Result::eOk)
    {
        _maxInterpolationCount = ResolveDlssgRuntimeMaximum(dlssgState.numFramesToGenerateMax);
        _runtimeReportedMaxInterpolation = static_cast<int>(dlssgState.numFramesToGenerateMax);
        _runtimeReportedStatus = static_cast<unsigned>(dlssgState.status);
        LOG_INFO("Max supported interpolations: {} status {:X}", dlssgState.numFramesToGenerateMax,
                 _runtimeReportedStatus);

        _supportsDMFG = dlssgState.bIsDynamicMFGSupported == sl::Boolean::eTrue;
    }

    _gameCommandQueue = cmdQueue;
    _swapChain = *swapChain;
    _hwnd = desc->OutputWindow;

    return true;
}

bool DLSSG_Dx12::CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd,
                                  DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                  IDXGISwapChain1** swapChain, bool readyToRelease)
{
    Kcd2Hdr::ApplyQuirk();
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == hwnd)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("FG swapchain already created for the same output window!");
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              desc->BufferCount, desc->Width, desc->Height, desc->Format, desc->Flags) == S_OK;

            *swapChain = (IDXGISwapChain1*) State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating new swapchain without releasing old one,
        // we need to release it to avoid errors
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            ReleaseSwapchain(_hwnd);

            // Not sure why but XeFG sometimes doesn't release the swapchain properly
            // so we force release it here to be able to recreate swapchain for same hwnd
            if (State::Instance().currentRealSwapchain != nullptr)
            {
                UINT release = 0;
                do
                {
                    release = State::Instance().currentRealSwapchain->Release();
                    LOG_DEBUG("Releasing swapchain, ref count: {}", release);
                } while (release > 0);
            }
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    if (StreamlineProxy::Module() == nullptr)
    {
        LOG_ERROR("Streamline proxy can't find sl.interposer.dll!");
        return false;
    }

    if (!StreamlineProxy::IsD3D12Inited())
    {
        if (State::Instance().currentD3D12Device != nullptr &&
            !StreamlineProxy::InitWithD3D12(State::Instance().currentD3D12Device))
        {
            return false;
        }
    }

    _width = desc->Width;
    _height = desc->Height;

    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};

        IDXGIFactory* slFactory = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &slFactory))
        {
            StreamlineProxy::UpgradeInterface()((void**) &factory);
            DxgiFactoryHooks::HookToDLSSGFactory(factory);
        }
        else
        {
            slFactory->Release();
        }

        IDXGIFactory2* factory2 = nullptr;
        if (factory->QueryInterface(IID_PPV_ARGS(&factory2)) != S_OK)
            return false;

        StreamlineProxy::SetFeatureLoaded()(sl::kFeatureDLSS_G, true);

        desc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        if (State::Instance().gameName == "KCD2")
        {
            desc->Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            LOG_INFO("KCD2: requesting application-owned frame-latency waitable");
        }
        auto result = factory2->CreateSwapChainForHwnd(cmdQueue, hwnd, desc, pFullscreenDesc, nullptr, swapChain);

        factory2->Release();
        factory2 = nullptr;

        if (result != S_OK)
        {
            LOG_ERROR("CreateSwapChain error: {:X}", (UINT) result);
            return false;
        }
    }

#if defined(OPTISCALER_RTX40_MFG)
    MfgUnlock::TryApply();
#endif
    sl::DLSSGState dlssgState {};
    sl::DLSSGOptions dlssgOptions {};
    if (StreamlineProxy::DLSSGGetState()(viewport, dlssgState, &dlssgOptions) == sl::Result::eOk)
    {
        _maxInterpolationCount = ResolveDlssgRuntimeMaximum(dlssgState.numFramesToGenerateMax);
        _runtimeReportedMaxInterpolation = static_cast<int>(dlssgState.numFramesToGenerateMax);
        _runtimeReportedStatus = static_cast<unsigned>(dlssgState.status);
        LOG_INFO("Max supported interpolations: {} status {:X}", dlssgState.numFramesToGenerateMax,
                 _runtimeReportedStatus);

        _supportsDMFG = dlssgState.bIsDynamicMFGSupported == sl::Boolean::eTrue;
    }

    _gameCommandQueue = cmdQueue;
    _swapChain = *swapChain;
    _hwnd = hwnd;

    return true;
}

void DLSSG_Dx12::CreateContext(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_DEBUG("");

    if (_device != nullptr)
        return;

    _device = device;
    CreateObjects(device);

    if (_isActive)
    {
        LOG_INFO("FG context recreated while active, pausing");
        State::Instance().fgChanged = true;
        UpdateTarget();
        Deactivate();
    }
}

void DLSSG_Dx12::Activate()
{
    LOG_DEBUG("");

    if (!_isActive)
    {

        UpdateTarget();
        _isActive = true;

        // Streamline's dlfgPresent path checks "is Reflex active?" on the very first present
        // after FG turns on. If DLSSG SetOptions (sent in the next Dispatch) reaches the runtime
        // before Reflex SetOptions does, that first present logs
        // eDLSSGStatusFailReflexNotDetectedAtRuntime and the FG frame is dropped (visible as a
        // flash). Send Reflex first, here, so it is already active when DLSSG starts presenting.
        // Deactivate() invalidated _reflexOptionsValid, so the next Dispatch will still resend it
        // in the normal path; this one just front-loads the first one.
        sl::ReflexOptions reflexConst = {};
        reflexConst.mode = sl::ReflexMode::eLowLatency;
        reflexConst.useMarkersToOptimize = ReflexHooks::gameIsSendingMarkers();
        if (StreamlineProxy::ReflexSetOptions()(reflexConst) == sl::Result::eOk)
        {
            _lastReflexMarkersSent = reflexConst.useMarkersToOptimize;
            _reflexOptionsValid = true;
            _reflexOptionsSentAtPresent = _fgFramePresentId;
        }
    }
}

void DLSSG_Dx12::Deactivate()
{
    LOG_DEBUG("");

    const bool keepAlive = DlssgPausePolicy::KeepRuntimeAlive(
        Config::Instance()->FGDLSSGSoftPause.value_or_default(),
        Config::Instance()->FGEnabled.value_or_default());
    if (DlssgPausePolicy::NeedsDeactivate(_isActive, _runtimeNeedsDisable, keepAlive))
    {
        if (keepAlive)
        {
            // Soft pause: stop dispatching but do NOT send sl::DLSSGMode::eOff. Sending eOff makes
            // Streamline's dlfgPresent release the entire NGX DLSS-G feature ("sl.dlss_g turned off,
            // releasing all resources"), and the next Activate() then rebuilds it via
            // NVSDK_NGX_CreateFeature (~185ms hitch). Keeping the feature alive makes Activate a cheap
            // mode resume. OptiScaler stops feeding SetConstants/SetResource while _isActive==false, so
            // Streamline has no new frame data to interpolate on. Streamline still releases the feature
            // on sl::Shutdown (Shutdown() -> StreamlineProxy::Shutdown).
            LOG_DEBUG("Soft pause: retaining NGX DLSS-G feature (no eOff sent)");
            _runtimeNeedsDisable = true;
        }
        else
        {
            sl::DLSSGOptions options {};
            options.mode = sl::DLSSGMode::eOff;
            options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
            const auto result = StreamlineProxy::DLSSGSetOptions()(viewport, options);
            _runtimeNeedsDisable = result != sl::Result::eOk;
            if (!_runtimeNeedsDisable)
            {
                State::Instance().dlssgLastSetMode = sl::DLSSGMode::eOff;
                State::Instance().dlssgDetectedInterpolationCount = 0;
                ReflexHooks::setDlssgFrameCount(0);
                LOG_INFO("DLSSG runtime disabled: FGEnabled {}, softPause {}, shuttingDown {}",
                         Config::Instance()->FGEnabled.value_or_default(),
                         Config::Instance()->FGDLSSGSoftPause.value_or_default(), State::Instance().isShuttingDown);
            }
            else
                LOG_ERROR("Could not disable DLSSG runtime: {}; will retry", magic_enum::enum_name(result));

            sl::ReflexOptions reflexConst = {};
            reflexConst.mode = sl::ReflexMode::eOff;
            reflexConst.useMarkersToOptimize = false;
            StreamlineProxy::ReflexSetOptions()(reflexConst);
        }

        // The next Activate() must resend both option sets; the caches describe the
        // pre-deactivate state, not the eOff pair sent above.
        _dlssgOptionsValid = false;
        _reflexOptionsValid = false;

        _isActive = false;
    }
}

void DLSSG_Dx12::DestroyFGContext()
{
    Deactivate();
    ReleaseObjects();
}

bool DLSSG_Dx12::Shutdown()
{
    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    DestroyFGContext();

    if (State::Instance().isShuttingDown)
        StreamlineProxy::Shutdown()();

    return true;
}

void DLSSG_Dx12::PauseForInputGap()
{
    // Keep accepting fresh guides; Deactivate/Activate would add another warmup.
    // Do not toggle Streamline eOff/eOn for a short source gap: that transition
    // can serialize the client queue and recreate internal state even with retention.
    _resetAfterInputGap = true;
    if (_inputGapPaused)
        return;

    _dlssgOptionsValid = false;
    _inputGapPaused = true;
    _inputGapPausedAt = GetTickCount64();

    // 振荡检测：退出流程 / 相机切换时游戏会"1 帧一抖"地停 DLSS —— 这种短缺口永远等不到
    // 2 秒阈值，但 DLSSG 在缺口之间持续 pacing 重复帧（6x 下每恢复一次吐 5 张）。
    // 10 秒内第 5 次进缺口 = 振荡，直接升级 hard stop。
    const uint64_t now = GetTickCount64();
    _inputGapRing[_inputGapRingIndex] = now;
    _inputGapRingIndex = (_inputGapRingIndex + 1) % 8;
    int recent = 0;
    for (uint64_t t : _inputGapRing)
        if (t != 0 && now - t <= 10000)
            ++recent;
    if (recent >= 5 && GetTickCount64() >= _inputGapResumeCooldownUntil)
    {
        LOG_INFO("DLSSG input-gap oscillation: {} gaps in 10 s, escalating to hard stop", recent);
        HardStopForInputGap();
    }
    State::Instance().dlssgDetectedInterpolationCount = 0;
    ReflexHooks::setDlssgFrameCount(0);
}

void DLSSG_Dx12::HardStopForInputGap()
{
    // Soft-pause escalation (see FGDLSSGSoftPauseHardStopMs). The soft pause keeps the NGX
    // feature alive, but the DLSSG runtime's present pacing keeps running: every game present
    // without fresh input is answered with repeats of the last generated frames. On MFG that is
    // a continuous stream of duplicate presents, and the external DLSS5 NR hook on the FG
    // swapchain processes every repeat -- NR's own output is already in the backbuffer, so each
    // repeat is NR applied on top of NR. A gap this long is a pause/menu, not a transient stall,
    // so release the runtime for real. NewFrame() re-activates on the next source frame; the
    // one-time NVSDK_NGX_CreateFeature rebuild (~185ms) lands where the hitch is invisible.
    const uint64_t pausedFor = _inputGapPausedAt != 0 ? GetTickCount64() - _inputGapPausedAt : 0;
    LOG_INFO("DLSSG input-gap hard stop: gap lasted {} ms, releasing DLSS-G runtime (MFG duplicates end)",
             pausedFor);

    sl::DLSSGOptions options {};
    options.mode = sl::DLSSGMode::eOff;
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    const auto result = StreamlineProxy::DLSSGSetOptions()(viewport, options);
    if (result == sl::Result::eOk)
    {
        State::Instance().dlssgLastSetMode = sl::DLSSGMode::eOff;
        State::Instance().dlssgDetectedInterpolationCount = 0;
        ReflexHooks::setDlssgFrameCount(0);
        _runtimeNeedsDisable = false;
    }
    else
    {
        // Leave _runtimeNeedsDisable set so Deactivate() retries the eOff.
        _runtimeNeedsDisable = true;
        LOG_ERROR("DLSSG hard-stop SetOptions failed: {}; will retry the eOff", magic_enum::enum_name(result));
    }

    sl::ReflexOptions reflexConst = {};
    reflexConst.mode = sl::ReflexMode::eOff;
    reflexConst.useMarkersToOptimize = false;
    StreamlineProxy::ReflexSetOptions()(reflexConst);

    // The next Activate() must resend both option sets.
    _dlssgOptionsValid = false;
    _reflexOptionsValid = false;
    _inputGapPaused = false;
    _inputGapPausedAt = 0;
    ZeroMemory(_inputGapRing, sizeof(_inputGapRing));
    _inputGapRingIndex = 0;
    _inputGapResumeCooldownUntil = 0;
    _resetAfterInputGap = true;

    // Same terminal state as the PausePresentGap path: pass-through presents, and NewFrame()
    // (which fires with the next fresh source frame) re-activates.
    _isActive = false;
    _waitingNewFrameData = true;
    _resumeShortWarmup = true;
}

bool DLSSG_Dx12::GapDispatchCurrentFrame()
{
    static unsigned int gapFailLogged = 0;
    auto fail = [](const char* why) {
        if (gapFailLogged < 5)
        {
            ++gapFailLogged;
            LOG_WARN("DLSSG gap-dispatch unavailable: {}", why);
        }
        return false;
    };

    if (_swapChain == nullptr || _device == nullptr)
        return fail("no swapchain/device");

    const int fIndex = GetIndex();

    if (!_resourceReady[fIndex].contains(FG_ResourceType::Depth) || !_resourceReady[fIndex].at(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].contains(FG_ResourceType::Velocity) || !_resourceReady[fIndex].at(FG_ResourceType::Velocity))
        return fail("guides not ready");

    Microsoft::WRL::ComPtr<IDXGISwapChain3> chain;
    Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer;
    if (FAILED(_swapChain->QueryInterface(IID_PPV_ARGS(&chain))) ||
        FAILED(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer))) || backbuffer == nullptr)
        return fail("backbuffer unavailable");

    const auto desc = backbuffer->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1)
        return fail("unexpected backbuffer shape");

    if (_gapHudless != nullptr)
    {
        const auto oldDesc = _gapHudless->GetDesc();
        if (oldDesc.Width != desc.Width || oldDesc.Height != desc.Height || oldDesc.Format != desc.Format)
        {
            _gapHudless->Release();
            _gapHudless = nullptr;
        }
    }
    if (_gapHudless == nullptr)
    {
        D3D12_RESOURCE_DESC copyDesc = desc;
        copyDesc.MipLevels = 1;
        copyDesc.Flags = (copyDesc.Flags | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &
                         ~(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copyDesc, D3D12_RESOURCE_STATE_COMMON,
                                                   nullptr, IID_PPV_ARGS(&_gapHudless))))
            return fail("copy texture allocation failed");
        _gapHudless->SetName(L"dlssg-gap-hudless");
    }

    auto* cmdList = GetSCCommandList(fIndex);
    if (cmdList == nullptr)
        return fail("no command list");

    D3D12_RESOURCE_BARRIER toDest = {};
    toDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toDest.Transition.pResource = _gapHudless;
    toDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    toDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    toDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &toDest);
    cmdList->CopyResource(_gapHudless, backbuffer.Get());
    toDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cmdList->ResourceBarrier(1, &toDest);

    _noUi[fIndex] = true;
    _noHudless[fIndex] = false;

    // 帧槽里留着游戏最后一帧的 hudless（可能仍是 ValidNow 状态，会触发
    // SetResource 的 "slot 已被 ValidNow 占用" 拒绝）；重置为非 ValidNow 再提交。
    auto& slot = _frameResources[fIndex][FG_ResourceType::HudlessColor];
    slot = Dx12Resource {};
    slot.validity = FG_ResourceValidity::UntilPresent;

    Dx12Resource res = {};
    res.type = FG_ResourceType::HudlessColor;
    res.frameIndex = fIndex;
    res.resource = _gapHudless;
    res.validity = FG_ResourceValidity::UntilPresentFromDispatch;
    res.state = D3D12_RESOURCE_STATE_COMMON;
    res.width = (UINT) desc.Width;
    res.height = desc.Height;
    if (!SetResource(&res))
        return fail("SetResource rejected");

    _frameCount++;
    _reset[fIndex] = 1;
    if (!Dispatch())
    {
        _frameCount--;
        return fail("dispatch failed");
    }
    return true;
}

bool DLSSG_Dx12::SubmitFreshFrame(bool freshSource)
{
    ++_fgFramePresentId;
    if (freshSource)
    {
        _inputGapMisses = 0;
        const bool submitted = Dispatch();
        if (submitted)
            return true;
    }
    else
    {
        ++_inputGapMisses;
    }

    // HSR occasionally omits one or two guide submissions during a camera/UI
    // transition. Treat those as ordinary repeated presents. Resetting DLSSG
    // history for every such transient is the source of visible periodic hitching.
    constexpr unsigned int kInputGapGracePresents = 3;
    if (Config::Instance()->FGEnabled.value_or_default() &&
        _inputGapMisses >= kInputGapGracePresents)
        PauseForInputGap();
    return false;
}

bool DLSSG_Dx12::Dispatch()
{
    LOG_FUNC();

#if defined(OPTISCALER_RTX40_MFG)
    MfgUnlock::TryApply();
    if (!CanDispatchDlssg(MfgUnlock::LastFailure()))
    {
        LOG_ERROR("DLSSG dispatch refused after an incomplete MFG patch rollback");
        return false;
    }
#endif
    _maxInterpolationCount = ResolveDlssgRuntimeMaximum(_maxInterpolationCount);

    UINT64 willDispatchFrame = 0;
    auto fIndex = GetDispatchIndex(willDispatchFrame);
    if (fIndex < 0)
        return false;

    if (!IsActive() || IsPaused())
        return false;

    LOG_DEBUG("_frameCount: {}, willDispatchFrame: {}, fIndex: {}", _frameCount, willDispatchFrame, fIndex);

    if (!_resourceReady[fIndex].contains(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].contains(FG_ResourceType::Velocity) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Velocity))
    {
        LOG_WARN("Depth or Velocity is not ready, skipping");
        return false;
    }

    auto& state = State::Instance();

    int requestedFramesToInterpolate = Config::Instance()->FGDLSSGInterpolationCount.value_or_default();
    LOG_DEBUG("Dispatch interpolation diag: config {}, max {}", requestedFramesToInterpolate, _maxInterpolationCount);
    if (requestedFramesToInterpolate > _maxInterpolationCount)
    {
        requestedFramesToInterpolate = _maxInterpolationCount;
        LOG_WARN("Requested interpolation count is higher than max supported, using max: {}", _maxInterpolationCount);
    }

    if (_framesToInterpolate != requestedFramesToInterpolate)
    {
        LOG_INFO("Interpolation count requested {} -> {}", _framesToInterpolate, requestedFramesToInterpolate);
    }

    sl::DLSSGOptions options {};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = requestedFramesToInterpolate;
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;

    if (Config::Instance()->FGDLSSGForceDMFG.value_or_default())
    {
        options.mode = sl::DLSSGMode::eDynamic;
        options.dynamicTargetFrameRate = Config::Instance()->FGDLSSGFramerateTargetDMFG.value_or_default();
    }

    StreamlineHooks::applyMenuDlssgInterlock(options, true);

    // Only touch the runtime when something changed, plus a keepalive every 600 presents
    // (~20 s at HSR's 30 fps base) in case the runtime dropped the options on its own.
    constexpr uint64_t kOptionsKeepalivePresents = 600;
    const bool dlssgOptionsChanged =
        !_dlssgOptionsValid || options.mode != _lastDlssgModeSent ||
        options.numFramesToGenerate != _lastDlssgNumSent ||
        (options.mode == sl::DLSSGMode::eDynamic && options.dynamicTargetFrameRate != _lastDlssgDynamicTargetSent);
    const bool dlssgKeepaliveDue =
        _dlssgOptionsValid && (_fgFramePresentId - _dlssgOptionsSentAtPresent) >= kOptionsKeepalivePresents;

    sl::Result dlssgSetOptionsResult = sl::Result::eOk;
    if (dlssgOptionsChanged || dlssgKeepaliveDue)
    {
        LOG_INFO("SetOptions before: mode {} num {} structVer {} extFG {} keepalive {}",
                 magic_enum::enum_name(options.mode), options.numFramesToGenerate, options.structVersion,
                 State::Instance().externalFrameGeneration, dlssgKeepaliveDue && !dlssgOptionsChanged);
        dlssgSetOptionsResult = StreamlineProxy::DLSSGSetOptions()(viewport, options);
        // "num" is our request, not the runtime's acceptance. The runtime clamps
        // numFramesToGenerate to its own numFramesToGenerateMax and still returns eOk, so a
        // request above the effective maximum silently drops back to 2x. Reading it as
        // acceptance here is what made a 2x session look like a 5x one in the logs.
        LOG_INFO("SetOptions after: result {} requestedNum {} runtimeMax {} runtimeReportedMax {} status {:X}",
                 magic_enum::enum_name(dlssgSetOptionsResult), options.numFramesToGenerate,
                 _maxInterpolationCount, _runtimeReportedMaxInterpolation, _runtimeReportedStatus);

        if (dlssgSetOptionsResult == sl::Result::eOk)
        {
            // A later failure must turn the runtime off again, even while resuming.
            _inputGapPaused = false;
            _inputGapPausedAt = 0;
            _inputGapResumeCooldownUntil = GetTickCount64() + 5000;
            _lastDlssgModeSent = options.mode;
            _lastDlssgNumSent = options.numFramesToGenerate;
            _lastDlssgDynamicTargetSent = options.dynamicTargetFrameRate;
            _dlssgOptionsValid = true;
            _dlssgOptionsSentAtPresent = _fgFramePresentId;
        }
        else
        {
            _dlssgOptionsValid = false; // retry on the next dispatch
        }
    }

    if (!CommitDlssgDispatchOptions(dlssgSetOptionsResult, options, _framesToInterpolate))
        return false;

    const bool markers = ReflexHooks::gameIsSendingMarkers();
    const bool reflexChanged = !_reflexOptionsValid || markers != _lastReflexMarkersSent;
    const bool reflexKeepaliveDue =
        _reflexOptionsValid && (_fgFramePresentId - _reflexOptionsSentAtPresent) >= kOptionsKeepalivePresents;

    if (reflexChanged || reflexKeepaliveDue)
    {
        sl::ReflexOptions reflexConst = {};
        reflexConst.mode = sl::ReflexMode::eLowLatency;
        reflexConst.useMarkersToOptimize = markers;

        auto reflexSetOptionsResult = StreamlineProxy::ReflexSetOptions()(reflexConst);

        if (reflexSetOptionsResult != sl::Result::eOk)
        {
            LOG_ERROR("Couldn't set Reflex options, error: {}", magic_enum::enum_name(reflexSetOptionsResult));
            _reflexOptionsValid = false;
        }
        else
        {
            _lastReflexMarkersSent = markers;
            _reflexOptionsValid = true;
            _reflexOptionsSentAtPresent = _fgFramePresentId;
        }
    }

    if (!_haveHudless.has_value())
    {
        _haveHudless = IsUsingHudless(fIndex);
    }

    if (!_noHudless[fIndex])
    {
        auto res = &_frameResources[fIndex][FG_ResourceType::HudlessColor];
        if (res->validity != FG_ResourceValidity::ValidNow)
        {
            res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
            res->frameIndex = fIndex;
            SetResource(res);
        }
    }

    if (!_noDistortionField[fIndex])
    {
        auto res = &_frameResources[fIndex][FG_ResourceType::Distortion];
        if (res->validity != FG_ResourceValidity::ValidNow)
        {
            res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
            res->frameIndex = fIndex;
            SetResource(res);
        }
    }

    sl::Constants constData = {};

    if (IsInfiniteDepth() && _cameraFar[fIndex] > _cameraNear[fIndex])
        _cameraFar[fIndex] = std::numeric_limits<float>::infinity();
    else if (IsInfiniteDepth() && _cameraNear[fIndex] > _cameraFar[fIndex])
        _cameraNear[fIndex] = std::numeric_limits<float>::infinity();

    if (_cameraPosition[fIndex][0] != 0.0f || _cameraPosition[fIndex][1] != 0.0f || _cameraPosition[fIndex][2] != 0.0f)
    {
        constData.cameraPos.x = _cameraPosition[fIndex][0];
        constData.cameraPos.y = _cameraPosition[fIndex][1];
        constData.cameraPos.z = _cameraPosition[fIndex][2];

        constData.cameraUp.x = _cameraUp[fIndex][0];
        constData.cameraUp.y = _cameraUp[fIndex][1];
        constData.cameraUp.z = _cameraUp[fIndex][2];

        constData.cameraFwd.x = _cameraForward[fIndex][0];
        constData.cameraFwd.y = _cameraForward[fIndex][1];
        constData.cameraFwd.z = _cameraForward[fIndex][2];

        constData.cameraRight.x = _cameraRight[fIndex][0];
        constData.cameraRight.y = _cameraRight[fIndex][1];
        constData.cameraRight.z = _cameraRight[fIndex][2];
    }
    else
    {
        constData.cameraPos = { 0.0f, 0.0f, 0.0f };
        constData.cameraUp = { 0.0f, 0.0f, 1.0f };
        constData.cameraRight = { 0.0f, 1.0f, 0.0f };
        constData.cameraFwd = { 1.0f, 0.0f, 0.0f };
        constData.cameraPinholeOffset = { 0.0f, 0.0f };

        XMMATRIX cameraViewToClip {};

        // XMMatrixPerspectiveFovRH will fail if input values are incorrect
        if (_cameraNear[fIndex] > 0.f && _cameraFar[fIndex] > 0.f &&
            !XMScalarNearEqual(_cameraVFov[fIndex], 0.0f, 0.00001f) &&
            !XMScalarNearEqual(_cameraAspectRatio[fIndex], 0.0f, 0.00001f))
        {
            if (XMScalarNearEqual(_cameraNear[fIndex], _cameraFar[fIndex], 0.00001f))
                _cameraFar[fIndex]++;

            cameraViewToClip = XMMatrixPerspectiveFovRH(_cameraVFov[fIndex], _cameraAspectRatio[fIndex],
                                                        _cameraNear[fIndex], _cameraFar[fIndex]);
        }
        else
        {
            LOG_WARN("Can't calculate projectionMatrix");
        }

        XMMATRIX clipToCameraView = XMMatrixInverse(nullptr, cameraViewToClip);

        auto prev = XMMatrixIdentity();

        // Convert to sl::float4x4 for Streamline
        XMFLOAT4X4 temp;
        XMStoreFloat4x4(&temp, cameraViewToClip);
        memcpy(&constData.cameraViewToClip, &temp, sizeof(sl::float4x4));
        XMStoreFloat4x4(&temp, clipToCameraView);
        memcpy(&constData.clipToCameraView, &temp, sizeof(sl::float4x4));

        XMStoreFloat4x4(&temp, prev);
        memcpy(&constData.clipToLensClip, &temp, sizeof(sl::float4x4));
        memcpy(&constData.clipToPrevClip, &temp, sizeof(sl::float4x4));
        memcpy(&constData.prevClipToClip, &temp, sizeof(sl::float4x4));
    }

    constData.cameraAspectRatio = _cameraAspectRatio[fIndex];
    constData.cameraFOV = _cameraVFov[fIndex];
    constData.cameraNear = _cameraNear[fIndex];
    constData.cameraFar = _cameraFar[fIndex];

    constData.jitterOffset.x = _jitterX[fIndex];
    constData.jitterOffset.y = _jitterY[fIndex];

    {
        auto mv = GetResource(FG_ResourceType::Velocity, fIndex);

        if (!mv)
        {
            LOG_ERROR("Motion vectors missing for: {}", fIndex);

            return false;
        }

        constData.mvecScale.x = _mvScaleX[fIndex] / (float) mv->width;
        constData.mvecScale.y = _mvScaleY[fIndex] / (float) mv->height;
    }

    // LOG_DEBUG("MvRes: {}x{}, Games MvScale : {}x{}, SL MvScale: {}x{}", mv->width, mv->height, _mvScaleX[fIndex],
    //           _mvScaleY[fIndex], constData.mvecScale.x, constData.mvecScale.y);

    // if (State::Instance().currentFeature != nullptr)
    //{
    //     auto fResX = State::Instance().currentFeature->RenderWidth();
    //     auto fResY = State::Instance().currentFeature->RenderHeight();

    //    LOG_DEBUG("Feature LowResMV: {} RenderRes : {}x{}, CMvScale: {}x{}",
    //              State::Instance().currentFeature->LowResMV(), fResX, fResY, 1.0f / (float) fResX,
    //              1.0f / (float) fResY);
    //}

    if (!Config::Instance()->FGSkipReset.value_or_default())
        constData.reset = _reset[fIndex] != 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    else
        constData.reset = sl::Boolean::eFalse;

    // A source gap invalidates FG history even if game-reset overrides are enabled.
    if (_resetAfterInputGap)
        constData.reset = sl::Boolean::eTrue;

    constData.depthInverted = IsInvertedDepth() ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constData.cameraMotionIncluded = sl::Boolean::eTrue;
    constData.motionVectors3D = sl::Boolean::eFalse;
    // constData.motionVectorsInvalidValue = 0.0f;
    constData.orthographicProjection = sl::Boolean::eFalse;
    constData.motionVectorsDilated = IsLowResMV() ? sl::Boolean::eFalse : sl::Boolean::eTrue;
    constData.motionVectorsJittered = IsJitteredMVs() ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    auto frameId = static_cast<uint32_t>(willDispatchFrame);

    auto tokenResult = StreamlineProxy::GetNewFrameToken()(frameToken, &frameId);
    if (tokenResult != sl::Result::eOk)
    {
        LOG_ERROR("GetNewFrameToken error: {} ({})", magic_enum::enum_name(tokenResult), (UINT) tokenResult);

        state.fgChanged = true;
        UpdateTarget();
        Deactivate();

        return false;
    }

    auto result = StreamlineProxy::SetConstants()(constData, *frameToken, viewport);
    if (result != sl::Result::eOk)
    {
        LOG_ERROR("SetConstants error: {} ({})", magic_enum::enum_name(result), (UINT) result);

        state.fgChanged = true;
        UpdateTarget();
        Deactivate();

        return false;
    }

    if (_resetAfterInputGap)
    {
        LOG_INFO("DLSSG input-gap resume: source={}, fresh guides, history reset", willDispatchFrame);
        _resetAfterInputGap = false;
    }
    _inputGapMisses = 0;

    LOG_DEBUG("Result: Ok");

    return true;
}

void* DLSSG_Dx12::FrameGenerationContext() { return (void*) 0x13371337; }

void* DLSSG_Dx12::SwapchainContext() { return (void*) 0x23372337; }

DLSSG_Dx12::~DLSSG_Dx12() { Shutdown(); }

bool DLSSG_Dx12::SetInterpolatedFrameCount(UINT interpolatedFrameCount) { return true; }

void DLSSG_Dx12::EvaluateState(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_FUNC();

    OwnedLockGuard lock(Mutex, 555);

    auto& state = State::Instance();

    // If needed hooks are missing or XeFG proxy is not inited or FG swapchain is not created
    if (!StreamlineProxy::LoadStreamline() || state.currentFGSwapchain == nullptr)
        return;

    if (state.isShuttingDown)
    {
        return;
    }

    _constants = fgConstants;

    // Do not create/activate DLSSG before the Upscaler input feature exists.
    // On the DX11 FSR2 handoff path the FG swapchain may appear before
    // OptiScaler has a current upscaler feature; creating DLSSG in that
    // window leaves the first Present without valid upscaled resources.
    if (Config::Instance()->FGEnabled.value_or_default() &&
        State::Instance().activeFgInput == FGInput::Upscaler &&
        State::Instance().currentFeature == nullptr)
    {
        if (_device != nullptr)
            Deactivate();
        LOG_DEBUG("FG waiting for active upscaler feature");
        return;
    }

    // If FG Enabled from menu
    if (Config::Instance()->FGEnabled.value_or_default())
    {
        if (_device == nullptr)
        {
            // Create it again
            CreateContext(device, fgConstants);
        }
        else if (state.fgChanged)
        {
            LOG_DEBUG("FGChanged");
            Deactivate();

            // Pause for 10 frames
            UpdateTarget();
        }

        if (State::Instance().activeFgInput == FGInput::Upscaler && !IsPaused() && !IsActive())
            Activate();
    }
    else
    {
        LOG_DEBUG("!FGEnabled");
        Deactivate();

        state.clearCapturedHudlesses = true;
        Hudfix_Dx12::ResetCounters();
    }

    if (state.fgChanged)
    {
        LOG_DEBUG("FGchanged");

        state.fgChanged = false;

        Hudfix_Dx12::ResetCounters();

        // Pause for 10 frames
        UpdateTarget();

        // Release FG mutex
        if (Mutex.getOwner() == 2)
            Mutex.unlockThis(2);
    }

    state.scChanged = false;
}

void DLSSG_Dx12::ReleaseObjects()
{
    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_uiCommandAllocator[i]);
        SAFE_RELEASE(_uiCommandList[i]);
        SAFE_RELEASE(_scCommandAllocator[i]);
        SAFE_RELEASE(_scCommandList[i]);
        SAFE_RELEASE(dlssgFence[i]);

        // Reset command list state
        _scCommandListResetted[i] = false;
        _scAllocatorFenceValues[i] = 0;

        _uiCommandListResetted[i] = false;
        _uiAllocatorFenceValues[i] = 0;
    }

    _renderUI.reset();
    _hudlessCompare.reset();
    _depthDebug.reset();
    _mvFlip.reset();
    _depthFlip.reset();
}

void DLSSG_Dx12::CreateObjects(ID3D12Device* InDevice)
{
    _device = InDevice;

    if (_uiCommandAllocator[0] != nullptr)
        return;

    LOG_DEBUG("");

    do
    {
        HRESULT result;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* cmdList = nullptr;
        ID3D12CommandQueue* cmdQueue = nullptr;

        // FG
        for (size_t i = 0; i < BUFFER_COUNT; i++)
        {
            // Reset command list state
            _scCommandListResetted[i] = false;
            _scAllocatorFenceValues[i] = 0;

            _uiCommandListResetted[i] = false;
            _uiAllocatorFenceValues[i] = 0;

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_uiCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _uiCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _uiCommandAllocator[i]->SetName(std::format(L"_uiCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandAllocator[i], (IUnknown**) &allocator))
                _uiCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _uiCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_uiCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _uiCommandList[i]->SetName(std::format(L"_uiCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandList[i], (IUnknown**) &cmdList))
                _uiCommandList[i] = cmdList;

            result = _uiCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_uiCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_uiFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_uiFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create UI fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_uiFenceEvent == nullptr)
            {
                _uiFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_uiFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for UI fence failed");
                    break;
                }
            }

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_scCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _scCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _scCommandAllocator[i]->SetName(std::format(L"_scCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandAllocator[i], (IUnknown**) &allocator))
                _scCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _scCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_scCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _scCommandList[i]->SetName(std::format(L"_scCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandList[i], (IUnknown**) &cmdList))
                _scCommandList[i] = cmdList;

            result = _scCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_scCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_scFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_scFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create SC fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_scFenceEvent == nullptr)
            {
                _scFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_scFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for SC fence failed");
                    break;
                }
            }
        }

    } while (false);
}

bool DLSSG_Dx12::Present()
{
    const bool inputGapProtection =
        State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 &&
        State::Instance().activeFgInput == FGInput::Upscaler &&
        Config::Instance()->FGDLSSGSoftPause.value_or_default();
    const bool freshSource = _frameCount > _lastDispatchedFrame;
    // Do not replay pre-gap queued history when a new source finally arrives.
    if (inputGapProtection && freshSource && _resetAfterInputGap)
        _lastDispatchedFrame = _frameCount - 1;
    const bool drawSourceResources = !inputGapProtection || freshSource;
    auto fIndex = inputGapProtection && !freshSource ? GetIndex() : GetIndexWillBeDispatched();


    // 输入缺口实时直通：无新 DLSS 输入时把当前 FG backbuffer 当作 HudlessColor
    // 立即 dispatch（每帧 reset）。DLSSG 永不释放：菜单实时、无陈旧重复帧、
    // 返回游戏零重建；无 guide 的菜单 present 由插件侧 Required 拒绝 NR。
    bool gapDispatched = false;
    if (Config::Instance()->FGDLSSGGapDispatch.value_or_default() &&
        inputGapProtection && !freshSource && !_waitingNewFrameData && IsActive() && !IsPaused())
        gapDispatched = GapDispatchCurrentFrame();
    LOG_DEBUG("fIndex: {}", fIndex);

    if (drawSourceResources && Config::Instance()->FGDrawUIOverFG.value_or_default())
    {
        auto ui = GetResource(FG_ResourceType::UIColor, fIndex);
        if (ui && (ui->validity == FG_ResourceValidity::UntilPresent ||
                   ui->validity == FG_ResourceValidity::JustTrackCmdlist ||
                   ui->validity == FG_ResourceValidity::UntilPresentFromDispatch))
        {
            LOG_DEBUG("UI[{}] resource: {:X}, copy: {}", fIndex, (size_t) ui->resource, (size_t) ui->copy);
            if (_renderUI.get() == nullptr)
            {
                _renderUI = std::make_unique<RUI_Dx12>("RenderUI", _device,
                                                       Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
            }
            else
            {
                if (Config::Instance()->FGUIPremultipliedAlpha.value_or_default() != _renderUI->IsPreMultipliedAlpha())
                {
                    LOG_INFO("UI premultiplied alpha changed, recreating RenderUI");
                    _renderUI = std::make_unique<RUI_Dx12>(
                        "RenderUI", _device, Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
                }
                else if (_renderUI->IsInit())
                {
                    auto commandList = GetSCCommandList(fIndex);
                    _renderUI->Dispatch((IDXGISwapChain3*) _swapChain, commandList, ui->GetResource(), ui->state);
                }
            }
        }
        else if (!ui)
        {
            LOG_WARN("UI resource is nullptr");
        }
    }

    if (drawSourceResources && IsActive() && !IsPaused())
    {
        if (State::Instance().fgHudlessCompare)
        {
            auto hudless = GetResource(FG_ResourceType::HudlessColor, fIndex);
            if (hudless && (hudless->validity == FG_ResourceValidity::UntilPresent ||
                            hudless->validity == FG_ResourceValidity::JustTrackCmdlist ||
                            hudless->validity == FG_ResourceValidity::UntilPresentFromDispatch))
            {
                LOG_DEBUG("Hudless[{}] resource: {:X}, copy: {}", fIndex, (size_t) hudless->resource,
                          (size_t) hudless->copy);
                if (_hudlessCompare.get() == nullptr)
                {
                    _hudlessCompare = std::make_unique<HC_Dx12>("HudlessCompare", _device);
                }
                else
                {
                    if (_hudlessCompare->IsInit())
                    {
                        auto commandList = GetSCCommandList(fIndex);
                        _hudlessCompare->Dispatch((IDXGISwapChain3*) _swapChain, commandList, hudless->GetResource(),
                                                  hudless->state);
                    }
                }
            }
            else if (!hudless)
            {
                LOG_WARN("Hudless resource is nullptr");
            }
        }
    }

    auto& debugState = State::Instance();
    debugState.fgDepthDebugAvailable = false;
    if (drawSourceResources && debugState.fgDepthDebug && IsActive() && !IsPaused() && _swapChain != nullptr)
    {
        auto depth = GetResource(FG_ResourceType::Depth, fIndex);
        Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
        Microsoft::WRL::ComPtr<ID3D12Resource> target;
        if (depth && SUCCEEDED(_swapChain->QueryInterface(IID_PPV_ARGS(&swapchain))) &&
            SUCCEEDED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&target))))
        {
            auto cmd = GetUICommandList(fIndex);
            if (_depthDebug == nullptr) _depthDebug = std::make_unique<FgDepthDebug>();
            debugState.fgDepthDebugAvailable = _depthDebug->Draw(_device, cmd, fIndex, depth->GetResource(),
                depth->state, target.Get(), static_cast<UINT>(depth->width), depth->height,
                depth->left, depth->top, debugState.fgDepthDebugGain, debugState.fgDepthDebugInvert,
                debugState.fgDepthDebugEnhanced);
            static unsigned debugLogCounter = 0;
            if (debugLogCounter++ % 120 == 0 && depth->GetResource())
            {
                const auto desc = depth->GetResource()->GetDesc();
                LOG_INFO("FG depth preview: recorded={} slot={} resource={:X} format={} texture={}x{} extent={}x{} offset={},{} enhanced={} invert={} gain={}",
                    debugState.fgDepthDebugAvailable, fIndex, reinterpret_cast<size_t>(depth->GetResource()),
                    static_cast<unsigned>(desc.Format), desc.Width, desc.Height, depth->width, depth->height,
                    depth->left, depth->top, debugState.fgDepthDebugEnhanced,
                    debugState.fgDepthDebugInvert, debugState.fgDepthDebugGain);
            }
        }
    }

    bool result = false;

    // if (IsActive() && !IsPaused())
    {
        if (_uiCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _uiCommandList[{}]: {:X}", fIndex, (size_t) _uiCommandList[fIndex]);
            auto closeResult = _uiCommandList[fIndex]->Close();

            if (closeResult == S_OK)
                _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[fIndex]);
            else
                LOG_ERROR("_uiCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            _gameCommandQueue->Signal(_uiFence, _uiAllocatorFenceValues[fIndex]);

            _uiCommandListResetted[fIndex] = false;
        }

        if (_scCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _scCommandList[{}]: {:X}", fIndex, (size_t) _scCommandList[fIndex]);
            auto closeResult = _scCommandList[fIndex]->Close();

            if (closeResult == S_OK)
                _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_scCommandList[fIndex]);
            else
                LOG_ERROR("_scCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            _scCommandListResetted[fIndex] = false;
        }
    }

    if (inputGapProtection)
    {
        if (gapDispatched)
            return true;
        // Soft-pause escalation: a gap long enough is a pause/menu, not a stall. While the
        // soft pause keeps the runtime alive, DLSSG paces repeats of the last generated
        // frames (MFG duplicates) and every repeat is re-processed by the external DLSS5
        // NR hook. After the deadline, release the runtime so presents pass through plain.
        const int hardStopMs = Config::Instance()->FGDLSSGSoftPauseHardStopMs.value_or_default();
        // 恢复后的 5 秒冷静期内不升级：返回游戏初期游戏自身会卡（流送/编译），
        // 阈值会连环误杀形成 stop/rebuild 级联（表现为 2s+1s+0.5s 的反复 NR）。
        const bool cooldown = GetTickCount64() < _inputGapResumeCooldownUntil;
        if (!freshSource && !cooldown && hardStopMs > 0 && _inputGapPaused && !_waitingNewFrameData &&
            _inputGapPausedAt != 0 && GetTickCount64() - _inputGapPausedAt >= (uint64_t) hardStopMs)
            HardStopForInputGap();
        return SubmitFreshFrame(freshSource);
    }

    // Pause FG (and release its resources via Deactivate) only after this many presents have gone by
    // without a new source frame. See Config.h FGDLSSGPausePresentGap for why the default is raised
    // for HSR DX11: a sub-second game-thread stall (camera cut / ultimate) must not trigger the
    // ~185ms NVSDK_NGX_CreateFeature rebuild that happens on resume.
    const UINT64 pausePresentGap = (UINT64)Config::Instance()->FGDLSSGPausePresentGap.value_or_default();
    if ((_fgFramePresentId - _lastFGFramePresentId) > pausePresentGap && IsActive() && !_waitingNewFrameData)
    {
        LOG_DEBUG("Pausing FG (present gap {} > {})", _fgFramePresentId - _lastFGFramePresentId, pausePresentGap);
        Deactivate();
        _waitingNewFrameData = true;
        return false;
    }

    _fgFramePresentId++;

    return Dispatch();
}

bool DLSSG_Dx12::SetResource(Dx12Resource* inputResource)
{
    if (inputResource == nullptr || inputResource->resource == nullptr ||
        (inputResource->type != FG_ResourceType::UIColor && (!IsActive() || IsPaused())))
    {
        return false;
    }

    // For late sent SL resources
    // we use provided frame index
    auto fIndex = inputResource->frameIndex;
    if (fIndex < 0)
        fIndex = GetIndex();

    auto& type = inputResource->type;

    std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);

    if (type == FG_ResourceType::HudlessColor)
    {
        if (Config::Instance()->FGDisableHudless.value_or_default())
            return false;

        // Making a copy if it's just valid now to be able to use it later
        if (State::Instance().fgHudlessCompare && inputResource->validity == FG_ResourceValidity::ValidNow)
            inputResource->validity = FG_ResourceValidity::ValidButMakeCopy;

        if (!_noHudless[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }

        if (!_noHudless[fIndex] && Config::Instance()->FGOnlyAcceptFirstHudless.value_or_default() &&
            inputResource->validity != FG_ResourceValidity::UntilPresentFromDispatch)
        {
            return false;
        }
    }

    if (type == FG_ResourceType::UIColor)
    {
        if (Config::Instance()->FGDisableUI.value_or_default())
            return false;

        // Making a copy if it's just valid now
        if (Config::Instance()->FGDrawUIOverFG.value_or_default() &&
            inputResource->validity == FG_ResourceValidity::ValidNow)
        {
            inputResource->validity = FG_ResourceValidity::ValidButMakeCopy;
        }

        if (!_noUi[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }
    }

    if (type == FG_ResourceType::Distortion)
    {
        if (!_noDistortionField[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }
    }

    if ((type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity) && _frameResources[fIndex].contains(type))
    {
        return false;
    }

    if (inputResource->cmdList == nullptr && inputResource->validity == FG_ResourceValidity::ValidNow)
    {
        LOG_ERROR("{}, validity == ValidNow but cmdList is nullptr!", magic_enum::enum_name(type));
        return false;
    }

    if (type == FG_ResourceType::Distortion)
    {
        LOG_TRACE("Distortion field is not supported by XeFG");
        return false;
    }

    auto fResource = &_frameResources[fIndex][type];
    fResource->type = type;
    fResource->frameIndex = fIndex;
    fResource->state = inputResource->state;
    fResource->validity = inputResource->validity;
    fResource->resource = inputResource->resource;
    fResource->top = inputResource->top;
    fResource->left = inputResource->left;
    fResource->width = inputResource->width;
    fResource->height = inputResource->height;
    fResource->cmdList = inputResource->cmdList;

    auto willFlip = State::Instance().activeFgInput == FGInput::Upscaler &&
                    Config::Instance()->FGResourceFlip.value_or_default() &&
                    (type == FG_ResourceType::Velocity || type == FG_ResourceType::Depth);

    // Resource flipping
    if (willFlip && _device != nullptr)
        FlipResource(fResource);

    // We usually don't copy any resources for DLSSG, the ones with this tag are the exception
    if (inputResource->cmdList != nullptr && fResource->validity == FG_ResourceValidity::ValidButMakeCopy)
    {
        LOG_DEBUG("Making a resource copy of: {}", magic_enum::enum_name(type));

        ID3D12Resource* copyOutput = nullptr;

        if (_resourceCopy[fIndex].contains(type))
            copyOutput = _resourceCopy[fIndex][type];

        if (!CopyResource(inputResource->cmdList, inputResource->resource, &copyOutput, inputResource->state))
        {
            LOG_ERROR("{}, CopyResource error!", magic_enum::enum_name(type));
            return false;
        }

        _resourceCopy[fIndex][type] = copyOutput;
        _resourceCopy[fIndex][type]->SetName(std::format(L"_resourceCopy[{}][{}]", fIndex, (UINT) type).c_str());
        fResource->copy = copyOutput;
        fResource->state = D3D12_RESOURCE_STATE_COPY_DEST;

        fResource->validity = FG_ResourceValidity::UntilPresent;
    }

    if (type == FG_ResourceType::UIColor)
        _noUi[fIndex] = false;
    else if (type == FG_ResourceType::Distortion)
        _noDistortionField[fIndex] = false;
    else if (type == FG_ResourceType::HudlessColor)
        _noHudless[fIndex] = false;

    if ((type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity) ||
        (fResource->validity != FG_ResourceValidity::UntilPresent &&
         fResource->validity != FG_ResourceValidity::JustTrackCmdlist))
    {
        fResource->validity = (fResource->validity != FG_ResourceValidity::ValidNow || willFlip)
                                  ? FG_ResourceValidity::UntilPresent
                                  : FG_ResourceValidity::ValidNow;

        if (type == FG_ResourceType::HudlessColor)
        {
            static DXGI_FORMAT lastFormat[BUFFER_COUNT] = {};
            auto desc = fResource->GetResource()->GetDesc();

            if (lastFormat[fIndex] != DXGI_FORMAT_UNKNOWN && lastFormat[fIndex] != desc.Format)
            {
                if (fResource->resource == _gapHudless)
                {
                    // 缺口直通纹理：格式跟随 FG backbuffer，不代表游戏换 hudless 格式
                    lastFormat[fIndex] = desc.Format;
                }
                else
                {
                    State::Instance().fgChanged = true;
                    return false;
                }
            }

            lastFormat[fIndex] = desc.Format;
        }

        sl::Resource resource {};
        resource.height = fResource->height;
        resource.native = fResource->GetResource();
        resource.state = fResource->state;
        resource.type = sl::ResourceType::eTex2d;
        resource.width = (uint32_t) fResource->width;

        sl::ResourceTag resourceTag {};
        resourceTag.resource = &resource;

        switch (fResource->type)
        {
        case FG_ResourceType::Depth:
            resourceTag.type = sl::kBufferTypeDepth;
            break;

        case FG_ResourceType::HudlessColor:
            resourceTag.type = sl::kBufferTypeHUDLessColor;
            break;

        case FG_ResourceType::UIColor:
            resourceTag.type = sl::kBufferTypeUIColorAndAlpha;
            break;

        case FG_ResourceType::Velocity:
            resourceTag.type = sl::kBufferTypeMotionVectors;
            break;

        default:
            return false;
        }

        resourceTag.lifecycle = fResource->validity == FG_ResourceValidity::UntilPresent
                                    ? ::sl::ResourceLifecycle::eValidUntilPresent
                                    : sl::ResourceLifecycle::eOnlyValidNow;

        resourceTag.extent.left = fResource->left;
        resourceTag.extent.top = fResource->top;
        resourceTag.extent.width = (uint32_t) fResource->width;
        resourceTag.extent.height = fResource->height;

        int indexDiff = GetIndex() - fIndex;
        if (indexDiff < 0)
            indexDiff += BUFFER_COUNT;

        // We will us UI color later with Render UI
        {
            auto frameId = static_cast<uint32_t>(_frameCount - indexDiff);

            auto tokenResult = StreamlineProxy::GetNewFrameToken()(frameToken, &frameId);
            if (tokenResult != sl::Result::eOk)
            {
                LOG_ERROR("GetNewFrameToken error: {} ({})", magic_enum::enum_name(tokenResult), (UINT) tokenResult);
                return false;
            }

            auto result = StreamlineProxy::SetTagForFrame()(*frameToken, viewport, &resourceTag, 1, fResource->cmdList);
            LOG_DEBUG("SetTagForFrame, frameId: {}, type: {} result: {} ({})", frameId, magic_enum::enum_name(type),
                      magic_enum::enum_name(result), (int32_t) result);

            if (result != sl::Result::eOk)
            {
                State::Instance().fgChanged = true;
                UpdateTarget();
                Deactivate();

                return false;
            }
        }

        // Potentially we don't need to restore but do it just to be safe
        if (inputResource->state == D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            ResourceBarrier(inputResource->cmdList, inputResource->resource, D3D12_RESOURCE_STATE_COPY_DEST,
                            inputResource->state);
        }

        SetResourceReady(type, fIndex);
    }

    LOG_TRACE("_frameResources[{}][{}]: {:X}", fIndex, magic_enum::enum_name(type), (size_t) fResource->GetResource());

    return true;
}

void DLSSG_Dx12::SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) { _gameCommandQueue = queue; }

bool DLSSG_Dx12::ReleaseSwapchain(HWND hwnd)
{
    if (hwnd != _hwnd || _hwnd == NULL)
        return false;

    LOG_DEBUG("");

    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        if (Mutex.getOwner() == 1)
        {
            LOG_WARN("Skipping Mutex we are already in ReleaseSwapchain");
            return true;
        }

        LOG_TRACE("Waiting Mutex 1, current: {}", Mutex.getOwner());
        Mutex.lock(1);
        LOG_TRACE("Accuired Mutex: {}", Mutex.getOwner());
    }

    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    // if (_fgContext != nullptr)
    //     DestroyFGContext();

    // if (!State::Instance().isShuttingDown)
    //{
    //     if (_swapChainContext != nullptr)
    //         DestroySwapchainContext();

    //    _swapChainContext = nullptr;
    //    State::Instance().currentFGSwapchain = nullptr;
    //}

    ReleaseObjects();

    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        LOG_TRACE("Releasing Mutex: {}", Mutex.getOwner());
        Mutex.unlockThis(1);
    }

    return true;
}
