#include "pch.h"
#include "Upscaler_Inputs_Dx11wDx12.h"

#include <wrl/client.h>

#include "MathUtils.h"

#include <with_dx12/with_dx12.h>
#include "shaders/depth_scale/DS_Dx12.h"

using namespace OptiMath;
static DS_Dx12* DepthScaleDx11wDx12 = nullptr;

static bool PrepareFgResourceCache(NVSDK_NGX_Parameter* parameters, UINT64 frameKey)
{
    if (parameters == nullptr)
        return false;

    const auto mask = Dx11WithDx12::ResourceMask::Mv | Dx11WithDx12::ResourceMask::Depth;
    const auto dontUseNtShared = Config::Instance()->DontUseNTShared.value_or_default();
    const auto frameIndex = Dx11WithDx12::GetUpscalerFrameIndex();

    const auto result =
        Dx11WithDx12::PrepareUpscalerResources(parameters, mask, frameIndex, frameKey, dontUseNtShared, false, true);

    if (!result.Success)
    {
        LOG_ERROR("Dx11wDx12 FG input cache preparation failed");
        return false;
    }

    return true;
}

static bool ReusePreparedUpscalerCacheForFg(UINT64 frameKey)
{
    const auto mask = Dx11WithDx12::ResourceMask::Mv | Dx11WithDx12::ResourceMask::Depth;
    const auto resolvedFrameKey = frameKey != 0 ? frameKey : Dx11WithDx12::GetLastPreparedUpscalerFrameId();

    if (!Dx11WithDx12::HasPreparedUpscalerResources(mask, resolvedFrameKey))
    {
        LOG_WARN("Dx11wDx12 FG input cache miss");
        return false;
    }

    return true;
}

void UpscalerInputsDx11wDx12::Init(ID3D11Device* dx11Device, ID3D11DeviceContext* dx11Context, ID3D12Device* dx12Device,
                                   ID3D12CommandQueue* dx12CommandQueue)
{
    if (State::Instance().activeFgInput != FGInput::Upscaler)
        return;

    if (dx12Device != nullptr && dx12CommandQueue != nullptr && !WithDx12::IsInited())
        WithDx12::SetD3D12Objects(dx12Device, dx12CommandQueue, D3D12_COMMAND_LIST_TYPE_DIRECT);

    if (!WithDx12::IsInited() && !WithDx12::PrepareD3D12ForD3D11(dx11Device, D3D_FEATURE_LEVEL_11_0))
    {
        LOG_ERROR("UpscalerInputsDx11wDx12::Init failed to resolve D3D12 device/queue");
        return;
    }

    _dx12Device = WithDx12::GetD3D12Device();
    _dx12CommandQueue = WithDx12::GetD3D12CommandQueue();

    Dx11WithDx12::Init(dx11Device, dx11Context);
}

void UpscalerInputsDx11wDx12::Reset() {}

void UpscalerInputsDx11wDx12::UpscaleStart(NVSDK_NGX_Parameter* InParameters, IFeature_Dx11* feature)
{
    if (InParameters == nullptr || feature == nullptr)
        return;

    // FSR Camera values
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float cameraVFov = 0.0f;
    float meterFactor = 0.0f;
    float mvScaleX = 0.0f;
    float mvScaleY = 0.0f;
    float jitterX = 0.0f;
    float jitterY = 0.0f;

    float tempCameraNear = 0.0f;
    float tempCameraFar = 0.0f;

    auto& cfg = *Config::Instance();
    const auto& ngxParams = *InParameters;

    ngxParams.Get(OptiKeys::FSR_NearPlane, &tempCameraNear);
    ngxParams.Get(OptiKeys::FSR_FarPlane, &tempCameraFar);

    if (!cfg.FsrUseFsrInputValues.value_or_default() || (tempCameraNear == 0.0f && tempCameraFar == 0.0f))
    {
        if (feature->DepthInverted())
        {
            cameraFar = cfg.FsrCameraNear.value_or_default();
            cameraNear = cfg.FsrCameraFar.value_or_default();
        }
        else
        {
            cameraFar = cfg.FsrCameraFar.value_or_default();
            cameraNear = cfg.FsrCameraNear.value_or_default();
        }
    }
    else
    {
        cameraNear = tempCameraNear;
        cameraFar = tempCameraFar;
    }

    if (!cfg.FsrUseFsrInputValues.value_or_default() ||
        ngxParams.Get(OptiKeys::FSR_CameraFovVertical, &cameraVFov) != NVSDK_NGX_Result_Success)
    {
        if (cfg.FsrVerticalFov.has_value())
            cameraVFov = GetRadiansFromDeg(cfg.FsrVerticalFov.value());
        else if (cfg.FsrHorizontalFov.value_or_default() > 0.0f)
        {
            const float hFovRad = GetRadiansFromDeg(cfg.FsrHorizontalFov.value());
            cameraVFov =
                GetVerticalFovFromHorizontal(hFovRad, (float) feature->TargetWidth(), (float) feature->TargetHeight());
        }
        else
            cameraVFov = GetRadiansFromDeg(60);
    }

    if (!cfg.FsrUseFsrInputValues.value_or_default())
        ngxParams.Get(OptiKeys::FSR_ViewSpaceToMetersFactor, &meterFactor);

    State::Instance().lastFsrCameraFar = cameraFar;
    State::Instance().lastFsrCameraNear = cameraNear;

    if (State::Instance().activeFgInput != FGInput::Upscaler || _dx12Device == nullptr)
        return;

    auto fg = State::Instance().currentFG;

    if (fg == nullptr)
        return;

    if (!feature->IsWithDx12() && !fg->RetireSharedInputReads())
    {
        LOG_ERROR("Native DX11 FG cache reuse refused: previous GPU input reads have not retired");
        return;
    }

    FG_Constants fgConstants {};
    fgConstants.displayWidth = feature->DisplayWidth();
    fgConstants.displayHeight = feature->DisplayHeight();

    if (feature->IsHdr())
        fgConstants.flags |= FG_Flags::Hdr;

    if (feature->DepthInverted())
        fgConstants.flags |= FG_Flags::InvertedDepth;

    if (feature->JitteredMV())
        fgConstants.flags |= FG_Flags::JitteredMVs;

    if (!feature->LowResMV())
        fgConstants.flags |= FG_Flags::DisplayResolutionMVs;

    if (Config::Instance()->FGAsync.value_or_default())
        fgConstants.flags |= FG_Flags::Async;

    fg->EvaluateState(_dx12Device, fgConstants);

    int reset = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Reset, &reset);

    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);

    fg->StartNewFrame();

    auto aspectRatio = (float) feature->DisplayWidth() / (float) feature->DisplayHeight();
    fg->SetCameraValues(cameraNear, cameraFar, cameraVFov, aspectRatio, meterFactor);
    fg->SetFrameTimeDelta(State::Instance().lastFGFrameTime);
    fg->SetMVScale(mvScaleX, mvScaleY);
    fg->SetJitter(jitterX, jitterY);
    fg->SetReset(reset);
    fg->SetInterpolationRect(feature->DisplayWidth(), feature->DisplayHeight());

    if (State::Instance().isShuttingDown || !fg->IsActive() || !Config::Instance()->FGEnabled.value_or_default() ||
        State::Instance().currentSwapchain == nullptr)
    {
        return;
    }

    // Keep EvaluateState and StartNewFrame above the active check: the ten-frame
    // activation delay advances on input frames even while FG is inactive.
    // Only guide copies and their shared-fence flush need an active consumer.
    if (feature->IsWithDx12())
    {
        const auto cacheFrameKey = Dx11WithDx12::GetLastPreparedUpscalerFrameId();
        if (!ReusePreparedUpscalerCacheForFg(cacheFrameKey))
            return;
    }
    else if (!PrepareFgResourceCache(InParameters, Dx11WithDx12::NextUpscalerFrameId()))
    {
        LOG_ERROR("Dx11wDx12 FG input cache preparation failed");
        return;
    }

    if (fg->Mutex.getOwner() == 2)
    {
        LOG_TRACE("Waiting for present!");
        fg->Mutex.lock(4);
        fg->Mutex.unlockThis(4);
    }

    auto& cache = Dx11WithDx12::GetUpscalerResourceCache();
    const auto frameIndex = fg->GetIndex();

    LOG_DEBUG("(FG Dx11wDx12) using cached inputs for fgUpscaledImage[{}], frame: {}", frameIndex, fg->FrameCount());

    ID3D12Resource* paramVelocity = cache.Mv.Dx12Resource;
    ID3D12Resource* paramDepth = cache.Depth.Dx12Resource;

    auto cmdList = fg->GetUICommandList();

    if (paramVelocity != nullptr)
    {
        Dx12Resource setResource {};
        setResource.type = FG_ResourceType::Velocity;
        setResource.cmdList = cmdList;
        setResource.resource = paramVelocity;
        setResource.state = (D3D12_RESOURCE_STATES) Config::Instance()->MVResourceBarrier.value_or(
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        setResource.validity = FG_ResourceValidity::ValidNow;

        if (feature->LowResMV())
        {
            setResource.width = feature->RenderWidth();
            setResource.height = feature->RenderHeight();
        }
        else
        {
            setResource.width = feature->TargetWidth();
            setResource.height = feature->TargetHeight();
        }

        fg->SetResource(&setResource);
    }

    if (paramDepth != nullptr)
    {
        auto done = false;

        if (Config::Instance()->FGEnableDepthScale.value_or_default())
        {
            if (DepthScaleDx11wDx12 == nullptr)
                DepthScaleDx11wDx12 = new DS_Dx12("Depth Scale Dx11wDx12", _dx12Device);

            if (DepthScaleDx11wDx12->CreateBufferResource(_dx12Device, paramDepth, feature->DisplayWidth(),
                                                          feature->DisplayHeight(),
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                DepthScaleDx11wDx12->Buffer() != nullptr)
            {
                DepthScaleDx11wDx12->SetBufferState(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                if (DepthScaleDx11wDx12->Dispatch(cmdList, paramDepth, DepthScaleDx11wDx12->Buffer()))
                {
                    Dx12Resource setResource {};
                    setResource.type = FG_ResourceType::Depth;
                    setResource.cmdList = cmdList;
                    setResource.resource = DepthScaleDx11wDx12->Buffer();
                    setResource.width = feature->RenderWidth();
                    setResource.height = feature->RenderHeight();
                    setResource.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    setResource.validity = FG_ResourceValidity::JustTrackCmdlist;

                    fg->SetResource(&setResource);
                    done = true;
                }
            }
        }

        if (!done)
        {
            Dx12Resource setResource {};
            setResource.type = FG_ResourceType::Depth;
            setResource.cmdList = cmdList;
            setResource.resource = paramDepth;
            setResource.width = feature->RenderWidth();
            setResource.height = feature->RenderHeight();
            setResource.state = (D3D12_RESOURCE_STATES) Config::Instance()->DepthResourceBarrier.value_or(
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            setResource.validity = FG_ResourceValidity::ValidNow;

            fg->SetResource(&setResource);
        }
    }

    // HudlessColor via final-scene handoff (route B for Genshin DX11 FG)。
    // 原神 DX11 路径下 Dx11wDx12SC 只桥接 backbuffer（含 HUD 合成后画面），
    // 从不 set HudlessColor → DLSSG 拿不到 pre-HUD color tag → 插值帧 HUD 拖影。
    // 桥侧 OptiScalerSubmitFinalSceneD3D11 已把 post-scene/pre-UI 的最终 color 帧
    // 入库到 State::finalSceneSrv（ID3D11ShaderResourceView，owned texture，MiscFlags=0）。
    // 这里取其 underlying ID3D11Texture2D，经 Dx11WithDx12::CopyTextureFrom11To12
    // 共享/拷贝到 D3D12（与 Depth/MV 同机制：源非 shared 时建 shared 克隆并拷贝），
    // 作为 FG_ResourceType::HudlessColor 喂给 DLSSG。
    if (Config::Instance()->FGEnabled.value_or_default() && !Config::Instance()->FGDisableHudless.value_or_default())
    {
        auto& state = State::Instance();
        ID3D11ShaderResourceView* finalSceneSrv = state.finalSceneSrv;
        if (finalSceneSrv != nullptr && state.finalSceneWidth != 0)
        {
            Microsoft::WRL::ComPtr<ID3D11Resource> sceneResource;
            finalSceneSrv->GetResource(&sceneResource);
            if (sceneResource != nullptr)
            {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> sceneTexture;
                if (SUCCEEDED(sceneResource.As(&sceneTexture)) && sceneTexture != nullptr)
                {
                    Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C shared {};
                    const bool dontUseNtShared = Config::Instance()->DontUseNTShared.value_or_default();
                    if (Dx11WithDx12::CopyTextureFrom11To12(sceneTexture.Get(), &shared, true, false, dontUseNtShared) &&
                        shared.Dx12Resource != nullptr)
                    {
                        Dx12Resource setResource {};
                        setResource.type = FG_ResourceType::HudlessColor;
                        setResource.cmdList = cmdList;
                        setResource.resource = shared.Dx12Resource;
                        setResource.width = state.finalSceneWidth;
                        setResource.height = state.finalSceneHeight;
                        setResource.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        setResource.validity = FG_ResourceValidity::JustTrackCmdlist;
                        setResource.frameIndex = fg->GetIndexWillBeDispatched();

                        fg->SetResource(&setResource);

                        static std::atomic_uint64_t hudlessLogCount { 0 };
                        if (hudlessLogCount.fetch_add(1, std::memory_order_relaxed) < 8)
                            LOG_INFO("(FG Dx11wDx12) HudlessColor set from finalSceneSrv {}x{}, frame: {}",
                                     state.finalSceneWidth, state.finalSceneHeight, fg->FrameCount());
                    }
                    else
                    {
                        static std::atomic_uint64_t failLogCount { 0 };
                        if (failLogCount.fetch_add(1, std::memory_order_relaxed) < 4)
                            LOG_WARN("(FG Dx11wDx12) finalSceneSrv CopyTextureFrom11To12 failed, frame: {}", fg->FrameCount());
                    }
                }
            }
        }
    }

    LOG_DEBUG("(FG Dx11wDx12) D3D11 input preparation done, frame: {}", fg->FrameCount());
}

void UpscalerInputsDx11wDx12::UpscaleEnd(NVSDK_NGX_Parameter* InParameters, IFeature_Dx11* feature)
{
    if (InParameters == nullptr || feature == nullptr)
        return;

    auto fg = State::Instance().currentFG;

    if (fg == nullptr || State::Instance().activeFgInput != FGInput::Upscaler || _dx12Device == nullptr)
        return;

    if (fg->IsActive() && Config::Instance()->FGEnabled.value_or_default() &&
        State::Instance().currentSwapchain != nullptr)
        LOG_DEBUG("(FG Dx11wDx12) running, frame: {}", feature->FrameCount());
}
