#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <dxgiformat.h>
#include <cstdint>
struct OptiScalerDepthFrame { ID3D12Resource* resource=nullptr; ID3D12Device* device=nullptr; ID3D12CommandQueue* queue=nullptr; uint64_t frameId=0; uint32_t width=0,height=0,subrectLeft=0,subrectTop=0,subrectWidth=0,subrectHeight=0; DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN; uint32_t state=D3D12_RESOURCE_STATE_COMMON, depthInverted=0; uint32_t structSize=sizeof(OptiScalerDepthFrame); };
extern "C" __declspec(dllimport) BOOL WINAPI OptiScalerGetDepthFrame(OptiScalerDepthFrame* frame);
extern "C" __declspec(dllimport) void WINAPI OptiScalerReleaseDepthFrame(OptiScalerDepthFrame* frame);
extern "C" __declspec(dllimport) void WINAPI OptiScalerBeginExternalPresent();
extern "C" __declspec(dllimport) void WINAPI OptiScalerEndExternalPresent();

// True only on the calling thread between BeginExternalPresent and EndExternalPresent.
// This is a transient ABI used by external frame-generation producers to prevent
// the wrapped Present path from applying a second finished-picture pass.
bool OptiScalerIsExternalPresent();
