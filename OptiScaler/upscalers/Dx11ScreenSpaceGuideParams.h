#pragma once
#include "Dx11ScreenSpaceGuides.h"
#include <nvsdk_ngx.h>

// Validate the entire input set before recording any conversion. Scalar exposure is unchanged.
inline HRESULT PrepareDx11ScreenSpaceGuides(Dx11ScreenSpaceGuides& guides, ID3D12Device* device,
    UINT slot, NVSDK_NGX_Parameter* parameters, ID3D12Resource* const (&sources)[5],
    unsigned rw, unsigned rh, unsigned tw, unsigned th, float& mvY, float& jitterY)
{
    unsigned w = 0, h = 0;
    if (parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &w) == NVSDK_NGX_Result_Success && w) rw = w;
    if (parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &h) == NVSDK_NGX_Result_Success && h) rh = h;
    auto full = [](ID3D12Resource* r, unsigned width, unsigned height) {
        if (!r) return false;
        auto d = r->GetDesc();
        return d.Width == width && d.Height == height;
    };
    if (!full(sources[0], rw, rh) || !full(sources[2], rw, rh) || !full(sources[4], tw, th) ||
        (!full(sources[1], rw, rh) && !full(sources[1], tw, th)) ||
        (sources[3] && !full(sources[3], rw, rh))) return E_INVALIDARG;
    const char* bases[] = {
        NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
        NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X,
        NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y };
    for (auto name : bases) {
        unsigned base = 0;
        parameters->Get(name, &base);
        if (base) return E_INVALIDARG;
    }
    if (parameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvY) != NVSDK_NGX_Result_Success ||
        parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY) != NVSDK_NGX_Result_Success) return E_INVALIDARG;
    auto hr = guides.Init(device);
    for (UINT role = 0; SUCCEEDED(hr) && role < Dx11ScreenSpaceGuides::RoleCount; ++role)
        if (sources[role]) hr = guides.Prepare(slot, static_cast<Dx11ScreenSpaceGuides::Role>(role), sources[role]);
    return hr;
}
