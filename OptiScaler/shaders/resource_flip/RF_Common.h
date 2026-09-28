#pragma once

#include "SysUtils.h"

struct alignas(256) RFConstants
{
    UINT width;
    UINT height;
    UINT offset;
    UINT velocity;
};

inline static std::string rfCode = R"(
cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint offset;
    uint velocity;
};

// Input texture
Texture2D<float3> SourceTexture : register(t0);

// Output texture
RWTexture2D<float3> DestinationTexture : register(u0);

// Compute shader thread group size
[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x > width || dispatchThreadID.y > height)
        return;

    uint2 sourceCoord = uint2(dispatchThreadID.x, dispatchThreadID.y + offset);
    uint2 pixelCoord = uint2(dispatchThreadID.x, height - dispatchThreadID.y);
    
    if (velocity == 0)
    {
        DestinationTexture[pixelCoord] = SourceTexture[sourceCoord];
        return;
    }
    
    float3 srcColor = SourceTexture.Load(int3(sourceCoord, 0));
    DestinationTexture[pixelCoord] = float3(srcColor.r, -srcColor.g, 0);
}
)";
