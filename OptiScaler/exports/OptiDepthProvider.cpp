#include "pch.h"
#include "OptiDepthProvider.h"
#include "../with_dx12/dx11_with_dx12.h"
#include "../with_dx12/with_dx12.h"
#include "../Config.h"
extern "C" BOOL WINAPI _OptiScalerGetDepthFrame(OptiScalerDepthFrame* frame) { if (!frame || frame->structSize < sizeof(OptiScalerDepthFrame)) return FALSE; *frame={}; frame->structSize=sizeof(OptiScalerDepthFrame); auto& d=Dx11WithDx12::GetUpscalerResourceCache().Depth; auto* dev=WithDx12::GetD3D12Device(); auto* q=WithDx12::GetD3D12CommandQueue(); if(!d.Dx12Resource||!dev||!q||!d.Desc.Width||!d.Desc.Height) return FALSE; d.Dx12Resource->AddRef(); dev->AddRef(); q->AddRef(); frame->resource=d.Dx12Resource; frame->device=dev; frame->queue=q; frame->frameId=d.LastPreparedFrame; frame->width=d.Desc.Width; frame->height=d.Desc.Height; frame->subrectWidth=d.Desc.Width; frame->subrectHeight=d.Desc.Height; frame->format=d.Desc.Format; frame->depthInverted=Config::Instance()->DepthInverted.value_or(false)?1u:0u; return TRUE; }
extern "C" void WINAPI _OptiScalerReleaseDepthFrame(OptiScalerDepthFrame* frame) { if(!frame) return; if(frame->resource) frame->resource->Release(); if(frame->device) frame->device->Release(); if(frame->queue) frame->queue->Release(); *frame={}; frame->structSize=sizeof(OptiScalerDepthFrame); }


namespace {
thread_local uint32_t g_external_present_depth = 0;
}

bool OptiScalerIsExternalPresent()
{
    return g_external_present_depth != 0;
}

extern "C" void WINAPI _OptiScalerBeginExternalPresent()
{
    ++g_external_present_depth;
}

extern "C" void WINAPI _OptiScalerEndExternalPresent()
{
    if (g_external_present_depth != 0)
        --g_external_present_depth;
}
