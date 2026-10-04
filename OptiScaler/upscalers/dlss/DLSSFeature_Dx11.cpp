#include <pch.h>
#include "DLSSFeature_Dx11.h"
#include <Config.h>

#include <dxgi.h>

bool DLSSFeatureDx11::InitInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_ERROR("nvngx.dll not loaded!");

        SetInit(false);
        return false;
    }

    NVSDK_NGX_Result nvResult;
    bool initResult = false;

    do
    {
        if (!_dlssInitedDx11)
        {
            _dlssInitedDx11 = NVNGXProxy::InitDx11(Device);

            if (!_dlssInitedDx11)
                return false;

            _moduleLoaded =
                (NVNGXProxy::D3D11_Init_ProjectID() != nullptr || NVNGXProxy::D3D11_Init_Ext() != nullptr) &&
                (NVNGXProxy::D3D11_Shutdown() != nullptr || NVNGXProxy::D3D11_Shutdown1() != nullptr) &&
                (NVNGXProxy::D3D11_GetParameters() != nullptr || NVNGXProxy::D3D11_AllocateParameters() != nullptr) &&
                NVNGXProxy::D3D11_DestroyParameters() != nullptr && NVNGXProxy::D3D11_CreateFeature() != nullptr &&
                NVNGXProxy::D3D11_ReleaseFeature() != nullptr && NVNGXProxy::D3D11_EvaluateFeature() != nullptr;

            // delay between init and create feature
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        LOG_INFO("Creating DLSS feature");

        if (NVNGXProxy::D3D11_CreateFeature() != nullptr)
        {
            ProcessInitParams(InParameters);

            _p_dlssHandle = &_dlssHandle;
            nvResult = NVNGXProxy::D3D11_CreateFeature()(InContext, NVSDK_NGX_Feature_SuperSampling, InParameters,
                                                         &_p_dlssHandle);

            if (nvResult != NVSDK_NGX_Result_Success)
            {
                LOG_ERROR("NVNGXProxy::D3D11_CreateFeature result: {0:X}", (unsigned int) nvResult);
                break;
            }
        }
        else
        {
            LOG_ERROR("NVNGXProxy::D3D11_CreateFeature is nullptr");
            break;
        }

        ReadVersion();

        initResult = true;

    } while (false);

    SetInit(initResult);

    return initResult;
}

bool DLSSFeatureDx11::EvaluateInternal(ID3D11DeviceContext* InDeviceContext, NVSDK_NGX_Parameter* InParameters)
{
    if (!_moduleLoaded)
    {
        LOG_ERROR("nvngx.dll or _nvngx.dll is not loaded!");
        return false;
    }

    NVSDK_NGX_Result nvResult;

    if (NVNGXProxy::D3D11_EvaluateFeature() != nullptr)
    {
        ProcessEvaluateParams(InParameters);

        auto evaluateNative = [&] {
            return NVNGXProxy::D3D11_EvaluateFeature()(InDeviceContext, _p_dlssHandle, InParameters, NULL);
        };
        bool converted = false;
        HRESULT prepareResult = E_INVALIDARG;
        if (Config::Instance()->DlssNativeScreenSpaceGuides.value_or_default())
        {
            if (!nativeScreenSpaceGuides)
                nativeScreenSpaceGuides = std::make_unique<NativeScreenSpaceGuides_Dx11>();
            prepareResult = nativeScreenSpaceGuides->Prepare(Device, InDeviceContext, InParameters,
                RenderWidth(), RenderHeight(), TargetWidth(), TargetHeight());
            converted = SUCCEEDED(prepareResult);
            if (nativeScreenSpaceDiagFrame++ % 300 == 0)
            {
                if (converted)
                {
                    D3D11_TEXTURE2D_DESC depth {}, motion {}, output {};
                    nativeScreenSpaceGuides->Converted(NativeScreenSpaceGuides_Dx11::Depth)->GetDesc(&depth);
                    nativeScreenSpaceGuides->Converted(NativeScreenSpaceGuides_Dx11::Motion)->GetDesc(&motion);
                    nativeScreenSpaceGuides->Converted(NativeScreenSpaceGuides_Dx11::Output)->GetDesc(&output);
                    LOG_INFO("NR NativeScreenSpaceGuides active: native DX11 NGX; depth {} -> {} {}x{} fmt {}, motion {} -> {} {}x{} fmt {}, output {}x{}, MV.Scale.Y {} -> {}, jitterY {} -> {}",
                        static_cast<void*>(nativeScreenSpaceGuides->Original(NativeScreenSpaceGuides_Dx11::Depth)),
                        static_cast<void*>(nativeScreenSpaceGuides->Converted(NativeScreenSpaceGuides_Dx11::Depth)),
                        depth.Width, depth.Height, static_cast<unsigned>(depth.Format),
                        static_cast<void*>(nativeScreenSpaceGuides->Original(NativeScreenSpaceGuides_Dx11::Motion)),
                        static_cast<void*>(nativeScreenSpaceGuides->Converted(NativeScreenSpaceGuides_Dx11::Motion)),
                        motion.Width, motion.Height, static_cast<unsigned>(motion.Format), output.Width, output.Height,
                        nativeScreenSpaceGuides->OriginalMvY(), -nativeScreenSpaceGuides->OriginalMvY(),
                        nativeScreenSpaceGuides->OriginalJitterY(), -nativeScreenSpaceGuides->OriginalJitterY());
                }
                else
                    LOG_WARN("NR NativeScreenSpaceGuides bypassed: {} HRESULT {:X}; native inputs retained",
                        nativeScreenSpaceGuides->Reason(), static_cast<unsigned>(prepareResult));
            }
        }
        if (converted)
            nvResult = nativeScreenSpaceGuides->Evaluate(InDeviceContext, InParameters,
                !nativeScreenSpaceActiveLastFrame, evaluateNative);
        else if (nativeScreenSpaceActiveLastFrame)
        {
            unsigned oldReset = 0;
            int signedReset = 0;
            bool resetIsSigned = false;
            if (InParameters->Get(NVSDK_NGX_Parameter_Reset, &oldReset) != NVSDK_NGX_Result_Success)
            {
                resetIsSigned = InParameters->Get(NVSDK_NGX_Parameter_Reset, &signedReset) == NVSDK_NGX_Result_Success;
                oldReset = static_cast<unsigned>(signedReset);
            }
            if (resetIsSigned) InParameters->Set(NVSDK_NGX_Parameter_Reset, 1);
            else InParameters->Set(NVSDK_NGX_Parameter_Reset, 1u);
            nvResult = evaluateNative();
            if (resetIsSigned) InParameters->Set(NVSDK_NGX_Parameter_Reset, signedReset);
            else InParameters->Set(NVSDK_NGX_Parameter_Reset, oldReset);
        }
        else
            nvResult = evaluateNative();
        if (nvResult == NVSDK_NGX_Result_Success)
            nativeScreenSpaceActiveLastFrame = converted;

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_EvaluateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }

        LOG_TRACE("_EvaluateFeature ok!");
    }
    else
    {
        LOG_ERROR("_EvaluateFeature is nullptr");
        return false;
    }

    return true;
}

DLSSFeatureDx11::DLSSFeatureDx11(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx11(InHandleId, InParameters), DLSSFeature(InHandleId, InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_INFO("nvngx.dll not loaded, now loading");
        NVNGXProxy::InitNVNGX();
    }

    LOG_INFO("binding complete!");
}

DLSSFeatureDx11::~DLSSFeatureDx11()
{
    if (State::Instance().isShuttingDown)
        return;

    if (NVNGXProxy::D3D11_ReleaseFeature() != nullptr && _p_dlssHandle != nullptr)
        NVNGXProxy::D3D11_ReleaseFeature()(_p_dlssHandle);
}
