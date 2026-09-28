#include "pch.h"
#include "DlssNr_Dx12_State.h"

auto DlssNr_Dx12::State::Run(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour, ID3D12Resource* depth, ID3D12Resource* motion,
             ID3D12Resource* output, const DlssNrFrameInfo& frame, ID3D12CommandQueue* timingQueue) -> void
{
    std::lock_guard<std::recursive_mutex> nrLock(mutex);
    const Config& cfg = *Config::Instance();

    if (nr.failed || cmdList == nullptr || colour == nullptr || depth == nullptr || motion == nullptr ||
        output == nullptr)
    {
        ReportSkipOnce(nr.failed ? "it already failed this session" : "a resource was missing");
        return;
    }

    ID3D12Resource* target = output;

    // Guard creation and dispatch together: either can record GPU work and alter compute bindings.
    const bool restoreRequired =
        cfg.RestoreComputeSignature.value_or_default() || cfg.RestoreGraphicSignature.value_or_default();
    if (restoreRequired && !frame.IndependentCommands && !D3D12Hooks::CanRestoreRootSignature(cmdList))
    {
        ReportSkipOnce("the upscaler could not restore state this frame");
        return;
    }
    lifetime.Record(cmdList);
    ScopedNrStateEnvelope stateEnvelope(cmdList);

    // A completed upscaler output normally arrives as a UAV. The pre-SR colour input instead arrives
    // readable. Track every transition so both paths return the resource exactly as their caller gave
    // it to us; a pre-SR resource without UAV support is written through a scratch-and-copy fallback.
    const D3D12_RESOURCE_STATES outputArrival =
        frame.PipelineManagedStates ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
        : frame.FinishedPicture     ? (D3D12_RESOURCE_STATES) frame.OutputArrivalState
        : frame.BeforeUpscale ? (!frame.PrivateColorCopy && Config::Instance()->ColorResourceBarrier.has_value()
                                     ? (D3D12_RESOURCE_STATES) Config::Instance()->ColorResourceBarrier.value()
                                     : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        : Config::Instance()->OutputResourceBarrier.has_value()
            ? (D3D12_RESOURCE_STATES) Config::Instance()->OutputResourceBarrier.value()
            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES targetState = outputArrival;
    const auto TransitionTarget = [&](D3D12_RESOURCE_STATES to)
    {
        Barrier(cmdList, target, targetState, to);
        targetState = to;
    };

    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        ReportSkipOnce("the output texture belongs to no D3D12 device");
        return;
    }

    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    const auto active =
        frame.BeforeUpscale
            ? DlssNr::PreSrColorExtent(desc, frame.RenderSubrectWidth, frame.RenderSubrectHeight)
            : std::optional<DlssNr::ColorExtent> { DlssNr::ColorExtent { (unsigned int) desc.Width, desc.Height } };
    if (!active)
    {
        ReportSkipOnce("the pre-SR active colour size is invalid");
        device->Release();
        return;
    }
    const auto width = active->width;
    const auto height = active->height;
    const bool cropColor = frame.BeforeUpscale && (width != desc.Width || height != desc.Height);
    const bool targetSupportsUav = cropColor || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;

    const auto guideDesc = depth->GetDesc();
    const auto motionDesc = motion->GetDesc();
    const auto guides = DlssNr::ResolveGuideRegions(
        { (unsigned int) guideDesc.Width, guideDesc.Height },
        { (unsigned int) motionDesc.Width, motionDesc.Height },
        { frame.RenderSubrectWidth, frame.RenderSubrectHeight }, { frame.OutputWidth, frame.OutputHeight },
        frame.MotionVectorsLowResolution, frame.DepthSubrectBaseX, frame.DepthSubrectBaseY,
        frame.MotionSubrectBaseX, frame.MotionSubrectBaseY);
    if (!guides.depth.valid() || !guides.motion.valid())
    {
        ReportSkipOnce("depth or motion-vector subrect is empty");
        device->Release();
        return;
    }
    const auto guideWidth = guides.depth.width, guideHeight = guides.depth.height;
    const auto motionWidth = guides.motion.width, motionHeight = guides.motion.height;
    const auto depthBaseX = guides.depth.x, depthBaseY = guides.depth.y;
    const auto motionBaseX = guides.motion.x, motionBaseY = guides.motion.y;

    nr.guideWidth = guideWidth;
    nr.guideHeight = guideHeight;
    nr.guideDepthInverted = frame.DepthInverted;

    // The game's own encoding, passed through. Every resource already carries a subrect saying how
    // big it is, so scaling by the resolution ratio on top of that counts it twice -- vectors come
    // out too long and the model warps its history past where the surface went.
    nr.guideMvScaleX = frame.MvScaleX;
    nr.guideMvScaleY = frame.MvScaleY;

    if (frame.Reset)
    {
        nr.reset = true;

        ++resets;

        if (resets <= 3 || resets % 100 == 0)
            LOG_INFO("DLSS-NR: the game asked for a history reset ({} so far)", resets);
    }

    // Guide dimensions can change without rebuilding the model; report changes as they occur.

    const GuideReport guidesNow {
        true,  nr.guideDepthInverted, nr.guideMvScaleX, nr.guideMvScaleY, guideWidth, guideHeight,
        width, (unsigned int) height
    };

    if (!loggedGuides.valid || loggedGuides.depthInverted != guidesNow.depthInverted ||
        loggedGuides.mvScaleX != guidesNow.mvScaleX || loggedGuides.mvScaleY != guidesNow.mvScaleY ||
        loggedGuides.guideW != guidesNow.guideW || loggedGuides.guideH != guidesNow.guideH ||
        loggedGuides.frameW != guidesNow.frameW || loggedGuides.frameH != guidesNow.frameH)
    {
        loggedGuides = guidesNow;
        LOG_INFO("DLSS-NR guides: depth {}, motion vector scale {} x {}, guides {}x{} for a {}x{} frame",
                 nr.guideDepthInverted ? "inverted" : "not inverted", nr.guideMvScaleX, nr.guideMvScaleY,
                 guideWidth, guideHeight, width, height);
    }

    const unsigned int configuredPasses =
        std::clamp(cfg.DlssNrPasses.value_or_default(), 1u,
                   cfg.DlssNrUnlockPasses.value_or_default() ? DlssNr::MaxPassCount : DlssNr::DefaultMaxPassCount);
    passChain = ResolvePassChain(cfg, configuredPasses, width, height);
    const unsigned int requestedPasses = passChain.count;
    if (requestedPasses == 0)
    {
        nr.reset = true;
        modelRunning = false;
        // Successful identity operation: do not replay an older NR result when every layer is disabled.
        ++nr.successfulDispatches;
        device->Release();
        return;
    }
    for (auto& model : nr.models)
        model.AdvanceEpoch(frame.SubmissionEpoch);
    if ((!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device)) || !DlssNr::Proxy::Context::Available())
    {
        nr.failed = true;
        nr.reason = "the NVIDIA NGX driver does not provide Neural Rendering";
        LOG_ERROR("DLSS-NR unavailable: {}", nr.reason);
        device->Release();
        return;
    }

    // Only the model runs at working resolution; source and composition remain at native size.
    const float workScale = passChain.passes[0].scale;
    const auto workWidth = passChain.passes[0].width;
    const auto workHeight = passChain.passes[0].height;
    const auto laterWorkWidth = passChain.passes[requestedPasses - 1].width;
    const auto laterWorkHeight = passChain.passes[requestedPasses - 1].height;
    const bool reduced = workWidth != width || workHeight != height;
    if (!PrepareRunModels(cmdList, device, frame, desc, { width, height }, { workWidth, workHeight },
                          { laterWorkWidth, laterWorkHeight }, workScale, requestedPasses))
    {
        device->Release();
        return;
    }
    // The parameter adapter already combined the HDR flag with the active color format.
    const bool isHdrBuffer = frame.ColourIsLinearHdr;

    if (!reportedHdr || reportedHdrValue != isHdrBuffer || reportedBefore != frame.BeforeUpscale)
    {
        reportedHdr = true;
        reportedHdrValue = isHdrBuffer;
        reportedBefore = frame.BeforeUpscale;
        LOG_INFO("DLSS-NR {} SR: the game's DLSS colour space is {} so the colour transform is {}",
                 frame.BeforeUpscale ? "before" : "after", isHdrBuffer ? "linear HDR" : "already tone-mapped",
                 isHdrBuffer ? "on" : "off");
    }

    const bool haveCodec = shader.IsInit();

    if (!haveCodec)
    {
        nr.failed = true;
        nr.reason = "the colour codec would not compile";
        LOG_ERROR("DLSS-NR unavailable: {}", nr.reason);
        device->Release();
        return;
    }

    // Advance capture scheduling only once the codec and models are ready.
    ++frames;
    TickNrRetired(frame.SubmissionEpoch);
    CheckCaptureTrigger();

    if (captureWriteAtFrame != 0 && frames >= captureWriteAtFrame)
    {
        captureWriteAtFrame = 0;
        const auto captureDir = Util::DllPath().remove_filename() / "dlssnr-capture";
        const auto written = captureFrames.write(captureDir);

        if (!written.empty())
            LOG_INFO("DLSS-NR wrote matched before/after frames to {}", written);
    }

    // Paper white maps the frame into the model's display-referred proxy.

    ResTrack_Dx12::HookLateNrQueue(device);
    if (gpuTime == nullptr)
        gpuTime = std::make_unique<DlssNrGpuTime>(device, "total");

    if (ngxTime == nullptr)
        ngxTime = std::make_unique<DlssNrGpuTime>(device, "model");

    if (gpuTime != nullptr)
        gpuTime->Start(cmdList);

    // Copy just the live image, not the stale right/bottom margins. Do this only after model
    // creation/pending-submission early returns, and inside the measured GPU interval. The compact
    // texture lets every existing codec/compare/hold/capture path use unmodified pixel coordinates.
    ID3D12Resource* const gameColor = target;
    if (cropColor)
    {
        TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmdList, nr.activeColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        DlssNr::CopyActiveColor(cmdList, nr.activeColor, gameColor, *active);
        TransitionTarget(outputArrival);
        Barrier(cmdList, nr.activeColor, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        target = nr.activeColor;
        targetState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    const auto FinishColor = [&](bool copyBack)
    {
        if (cropColor)
        {
            if (copyBack)
            {
                TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cmdList, gameColor, outputArrival, D3D12_RESOURCE_STATE_COPY_DEST);
                DlssNr::CopyActiveColor(cmdList, gameColor, target, *active);
                Barrier(cmdList, gameColor, D3D12_RESOURCE_STATE_COPY_DEST, outputArrival);
            }
            TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            TransitionTarget(outputArrival);
        }
    };

    EncodeContext encoded { cmdList, device, target, targetState, frame, workScale, targetSupportsUav };
    EncodeInput(encoded);
    targetState = encoded.targetState;
    const auto whitePoint = encoded.whitePoint;
    auto* exposureTex = encoded.exposureTex;
    const auto useGameExposure = encoded.useGameExposure;
    const auto exposurePreMul = encoded.exposurePreMul;
    auto* modelInput = encoded.modelInput;

    // Read the exposure scan's candidates on the pass's own command list, once a frame.
    DlssNr::ExposureScan::Tick(device, cmdList, frame.SubmissionEpoch);

    ID3D12Resource* depthIn = ReadableGuide(device, cmdList, depth, &nr.depthClone);
    ID3D12Resource* motionIn = ReadableGuide(device, cmdList, motion, &nr.motionClone);

    if (depthIn == nullptr || motionIn == nullptr)
    {
        nr.reset = true;
        ReportSkipOnce("the game's depth or motion vectors could not be made readable this frame");
        FinishColor(false);
        device->Release();
        return;
    }

    // The vectors were scaled to full-frame pixels; each pass reprojects at its own working size.
    const auto mvToWorkX = [&](unsigned int passW) { return width != 0 ? (float) passW / (float) width : 1.0f; };
    const auto mvToWorkY = [&](unsigned int passH) { return height != 0 ? (float) passH / (float) height : 1.0f; };

    if (ngxTime != nullptr)
        ngxTime->Start(cmdList);

    // Count only a contiguous set of ready, separate feature histories. A failed extra creation never
    // falls back to reusing the main feature: that tells one temporal model several frames elapsed in
    // one game frame and makes its history fight the later layers.
    unsigned int effectivePasses = 1;
    if (nr.passScratch != nullptr)
    {
        for (unsigned int pass = 1; pass < requestedPasses; ++pass)
        {
            if (!nr.models[pass].Ready(frame.SubmissionEpoch))
                break;
            ++effectivePasses;
        }
    }

    {

        if (loggedConfigured != configuredPasses || loggedEffective != effectivePasses)
        {
            loggedConfigured = configuredPasses;
            loggedEffective = effectivePasses;
            LOG_INFO("DLSS-NR model passes: configured {}, effective {}", configuredPasses, effectivePasses);
        }
    }

    // Keep the encoded base immutable; ping-pong model outputs and compose the final delta once.
    ID3D12Resource* passInput = modelInput;
    ID3D12Resource* passOutput = nr.output;
    ID3D12Resource* finalAnswer = nullptr;
    bool outputReadable = false;
    bool scratchReadable = false;
    bool clampReadable = false;
    bool clampFailed = false;

    const auto MakeModelReadable = [&](ID3D12Resource* resource)
    {
        bool& readable = resource == nr.output      ? outputReadable
                         : resource == nr.passClamp ? clampReadable
                                                    : scratchReadable;
        if (!readable)
        {
            Barrier(cmdList, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            readable = true;
        }
    };

    const auto MakeModelWritable = [&](ID3D12Resource* resource)
    {
        bool& readable = resource == nr.output      ? outputReadable
                         : resource == nr.passClamp ? clampReadable
                                                    : scratchReadable;
        if (readable)
        {
            Barrier(cmdList, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            readable = false;
        }
    };

    int result = NVSDK_NGX_Result_Success;
    const bool enlargementReset = nr.reset;
    bool compositionSucceeded = false;
    unsigned int completedPasses = 0;
    unsigned int answerWidth = workWidth, answerHeight = workHeight;

    for (unsigned int pass = 0; pass < effectivePasses && result == NVSDK_NGX_Result_Success; ++pass)
    {
        // Each enabled layer evaluates at its own content extent.
        const auto passW = passChain.passes[pass].width;
        const auto passH = passChain.passes[pass].height;
        MakeModelWritable(passOutput);
        bool evaluated = false;
        result = static_cast<int>(nr.models[pass].Run(
            cmdList, device, passInput, depthIn, motionIn, passOutput, passW, passH, guideWidth,
            guideHeight, motionWidth, motionHeight, depthBaseX, depthBaseY, motionBaseX, motionBaseY,
            nr.guideDepthInverted, nr.reset, nr.guideMvScaleX * mvToWorkX(passW), nr.guideMvScaleY * mvToWorkY(passH),
            ModelSettings(cfg, pass), frame.SubmissionEpoch, &evaluated));
        modelRunning = evaluated && result == NVSDK_NGX_Result_Success;
        if (!evaluated)
            break;

        if (result != NVSDK_NGX_Result_Success)
            break;

        finalAnswer = passOutput;
        MakeModelReadable(finalAnswer);
        ++completedPasses;
        answerWidth = passW;
        answerHeight = passH;

        const float blend = passChain.passes[pass].blend;
        if (blend < 1.0f)
        {
            // Never read/write the same raster: passInput may be passClamp. Blend into the spare answer.
            auto* mixed = passOutput == nr.output ? nr.passScratch : nr.output;
            if (!mixed) { clampFailed = true; finalAnswer = nullptr; break; }
            MakeModelWritable(mixed);
            DlssNrConstants mix {};
            mix.Mode = DlssNrMode_ClampProxy;
            mix.Width = passW;
            mix.Height = passH;
            mix.SourceContentWidth = mix.ModelContentWidth = passW;
            mix.SourceContentHeight = mix.ModelContentHeight = passH;
            mix.ClampMerge = 1.0f - blend;
            if (!shader.DispatchPass(cmdList, mix, finalAnswer, nullptr, passInput, nullptr, nullptr, mixed,
                                     nullptr, nullptr))
            { clampFailed = true; finalAnswer = nullptr; break; }
            MakeModelReadable(mixed);
            finalAnswer = mixed;
        }

        if (pass + 1 < effectivePasses)
        {
            // Resample the blended result into the next enabled layer's input extent.
            // This stage no longer injects the original game frame into every boundary.
            const auto nextW = passChain.passes[pass + 1].width;
            const auto nextH = passChain.passes[pass + 1].height;
            MakeModelWritable(nr.passClamp);
            DlssNrConstants clamp {};
            clamp.Mode = DlssNrMode_ClampProxy;
            clamp.Width = nextW;
            clamp.Height = nextH;
            clamp.ClampMerge = 0.0f; // per-layer blend was applied above, against this layer's input
            // The chain rasters are allocated at max(work, later); the region that holds this
            // pass's answer is only passW x passH, and the shader resamples from that region.
            clamp.SourceContentWidth = passW;
            clamp.SourceContentHeight = passH;
            if (!shader.DispatchPass(cmdList, clamp, finalAnswer, nullptr, nr.colorCopy, nullptr, nullptr, nr.passClamp,
                                     nullptr, nullptr))
            {
                // Keep this frame's last valid answer; later histories skipped a frame.
                clampFailed = true;
                effectivePasses = pass + 1;
                break;
            }
            MakeModelReadable(nr.passClamp);
            passInput = nr.passClamp;
            passOutput = passOutput == nr.output ? nr.passScratch : nr.output;

        }
    }

    if (ngxTime != nullptr)
        ngxTime->End(cmdList);

    nr.reset = clampFailed || finalAnswer == nullptr;

    // Supersampling probe: report the model working ABOVE native so a test log tells us whether NGX even
    // accepts a super-native evaluate and what it returns. Once per working-size change, or on any error.
    if (workWidth > width || workHeight > height)
    {

        if (lastSuper != workWidth || result != 1)
        {
            lastSuper = workWidth;
            LOG_INFO("DLSS-NR SUPERSAMPLE: model at {}x{} = {:.2f}x native {}x{}, evaluate result {} ({})",
                     workWidth, workHeight, (float) workWidth / (float) width, width, height, result,
                     NgxResultName((unsigned int) result));
        }
    }

    if (result == NVSDK_NGX_Result_Success && finalAnswer != nullptr)
    {
        // Resolve takes the difference between what the model returned and what it was shown, and adds
        // that back to the frame. At strength zero the result is what the upscaler produced, exactly, and
        // anything the model left alone is untouched rather than round-tripped through the curve.
        auto resolveParams = MakeResolveConstants(encoded, effectivePasses);
        {
            // The answer raster is shared at max(work, later) but only the last pass's working
            // region carries this frame's content; the resolve samples that region, not the padding.
            const auto answerW = answerWidth;
            const auto answerH = answerHeight;
            resolveParams.ModelContentWidth = answerW;
            resolveParams.ModelContentHeight = answerH;
        }

        // Downsample the model answer to native for composition; fall back to the working-size pair.
        // The final answer is NPSR and the native output rests in UAV.
        bool superDownOk = false;
        if (answerWidth == workWidth && answerHeight == workHeight &&
            passChain.maxWidth == workWidth && passChain.maxHeight == workHeight &&
            workScale > 1.0f && nr.superDown != nullptr && nr.outputNative != nullptr &&
            nr.superDown->DispatchResources(cmdList, finalAnswer, nr.outputNative))
        {
            Barrier(cmdList, nr.outputNative, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            superDownOk = true;
            resolveParams.ModelContentWidth = width;
            resolveParams.ModelContentHeight = height;
        }

        ID3D12Resource* resolveProxy = superDownOk ? nr.colorCopy : modelInput;
        ID3D12Resource* resolveAnswer = superDownOk ? nr.outputNative : finalAnswer;
        bool enlargementReady = true;
        if (cfg.DlssNrTransfer.value_or_default() == 2 && reduced)
        {
            auto* enlarged = EnlargeMatchedResidual(cmdList, device, modelInput, finalAnswer, depthIn, motionIn,
                                                    frame, resolveParams, enlargementReset, timingQueue);
            enlargementReady = enlarged != nullptr;
            if (enlarged)
            {
                resolveAnswer = enlarged;
                resolveParams.Transfer = 2;
                resolveParams.ModelContentWidth = width;
                resolveParams.ModelContentHeight = height;
            }
            if (enlarged && resolveParams.DebugView == 2)
            { resolveAnswer = finalAnswer; resolveParams.Transfer = 1; } // Inspect the actual model answer.
        }
        else
        {
            ReleaseEnlarger();
            enlargementStatus.clear();
        }

        // Resolve pre-SR inputs without UAV support through an owned scratch and copy-back.
        ID3D12Resource* resolveOriginal = targetSupportsUav ? nr.hdrCopy : target;
        ID3D12Resource* resolveTarget =
            targetSupportsUav ? target : nr.hdrCopy;

        if (targetSupportsUav)
        {
            TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        // Diagnostic: dump the whole pass chain whenever any part of it changes. Later-pass
        // resolution bugs are otherwise invisible -- the compose report only tracks work size.
        struct ChainReport
        {
            unsigned w = 0, h = 0, workW = 0, workH = 0, laterW = 0, laterH = 0;
            unsigned eff = 0, done = 0, contentW = 0, contentH = 0;
            unsigned ansTexW = 0, ansTexH = 0, proxyTexW = 0, proxyTexH = 0;
            int answerSlot = -1, proxySlot = -1;
            bool operator!=(const ChainReport& o) const
            {
                return w != o.w || h != o.h || workW != o.workW || workH != o.workH || laterW != o.laterW ||
                       laterH != o.laterH || eff != o.eff || done != o.done || contentW != o.contentW ||
                       contentH != o.contentH || ansTexW != o.ansTexW || ansTexH != o.ansTexH ||
                       proxyTexW != o.proxyTexW || proxyTexH != o.proxyTexH || answerSlot != o.answerSlot ||
                       proxySlot != o.proxySlot;
            }
        };
        static ChainReport loggedChain;
        ChainReport chainNow { width,          height,           workWidth,        workHeight,
                               laterWorkWidth, laterWorkHeight,  effectivePasses,  completedPasses,
                               resolveParams.ModelContentWidth, resolveParams.ModelContentHeight,
                               resolveAnswer ? (unsigned) resolveAnswer->GetDesc().Width : 0u,
                               resolveAnswer ? (unsigned) resolveAnswer->GetDesc().Height : 0u,
                               resolveProxy ? (unsigned) resolveProxy->GetDesc().Width : 0u,
                               resolveProxy ? (unsigned) resolveProxy->GetDesc().Height : 0u,
                               resolveAnswer == nr.output ? 0 : (resolveAnswer == nr.passScratch ? 1 : (resolveAnswer == nr.outputNative ? 2 : (resolveAnswer == nr.passClamp ? 3 : 4))),
                               resolveProxy == nr.colorCopy ? 0 : (resolveProxy == nr.colorSmall ? 1 : (resolveProxy == modelInput ? 2 : 3)) };
        if (loggedChain != chainNow)
        {
            LOG_INFO("DLSS-NR chain: native {}x{}, work {}x{}, later {}x{}, effective {}, completed {}, "
                     "answerSlot {} ({}x{}), proxySlot {} ({}x{}), modelContent {}x{}",
                     chainNow.w, chainNow.h, chainNow.workW, chainNow.workH, chainNow.laterW, chainNow.laterH,
                     chainNow.eff, chainNow.done, chainNow.answerSlot, chainNow.ansTexW, chainNow.ansTexH,
                     chainNow.proxySlot, chainNow.proxyTexW, chainNow.proxyTexH, chainNow.contentW, chainNow.contentH);
            loggedChain = chainNow;
        }

        const bool resolved = enlargementReady && shader.DispatchPass(cmdList, resolveParams, resolveProxy, resolveAnswer,
                                                  resolveOriginal, motionIn, exposureTex, resolveTarget, nullptr);
        compositionSucceeded = resolved;

        if (resolved && !targetSupportsUav)
        {
            Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            const D3D12_RESOURCE_STATES priorTargetState = targetState;
            TransitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyResource(target, nr.hdrCopy);
            TransitionTarget(priorTargetState);
            Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        else if (!targetSupportsUav)
        {
            // The resolve target was made writable even while private DLSS was
            // warming up. Restore it before the common end-of-frame transition.
            Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        MakeModelWritable(nr.output);
        if (nr.passScratch != nullptr)
            MakeModelWritable(nr.passScratch);

        if (superDownOk)
            Barrier(cmdList, nr.outputNative, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // Schedule matched proxy/output capture for delayed readback.
        if (captureFrames.isActive())
        {
            captureFrames.record(cmdList, device, nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 target, targetState);

            if (captureFrames.readyToWrite() && captureWriteAtFrame == 0)
                captureWriteAtFrame = frames + 8;
        }
    }
    else if (result != NVSDK_NGX_Result_Success)
    {
        nr.failed = true;
        nr.reason = "the model refused to run";

        LOG_ERROR("DLSS-NR evaluate returned 0x{:X} ({}); use Retry to recreate the model", (uint32_t) result,
                  NgxResultName((unsigned int) result));
    }

    // Restore all intermediate surfaces to the UAV state expected by the next frame.
    MakeModelWritable(nr.output);
    if (nr.passScratch != nullptr)
        MakeModelWritable(nr.passScratch);

    Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (nr.passClamp != nullptr)
        MakeModelWritable(nr.passClamp);

    // Failed evaluations leave the game's original image intact. A successful copy-back writes
    // only the active rectangle and restores both resources before DLSS consumes the image.
    FinishColor(compositionSucceeded);
    if (compositionSucceeded)
        ++nr.successfulDispatches;

    EndGpuTiming(cmdList, timingQueue);

    // Restore guide clones to COPY_DEST for the next frame's refresh.
    if (depthIn == nr.depthClone)
        Barrier(cmdList, nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (motionIn == nr.motionClone)
        Barrier(cmdList, nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (reduced && nr.colorSmall != nullptr)
        Barrier(cmdList, nr.colorSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Leave the staging copy as the next frame expects to find it.
    Barrier(cmdList, nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    device->Release();
}
