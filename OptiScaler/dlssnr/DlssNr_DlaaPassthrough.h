#pragma once
#include <d3d12.h>

namespace DlssNr
{
// Only skip SR inside the normal pipeline. Its NR guide capture, RCAS and output
// stages must still run. The legacy sharpening creation flag is not an SR requirement.
inline bool UseDlaaPassthrough(bool dx11Bridge, bool dlss, bool nrEnabled, bool finished,
                               bool deferred, bool rayReconstruction, bool sharpeningHandled,
                               unsigned renderWidth, unsigned renderHeight,
                               unsigned targetWidth, unsigned targetHeight)
{
    return dx11Bridge && dlss && nrEnabled && finished && !deferred && !rayReconstruction &&
           sharpeningHandled && renderWidth != 0 && renderHeight != 0 &&
           renderWidth == targetWidth && renderHeight == targetHeight;
}

inline bool CanCopyDlaaColor(const D3D12_RESOURCE_DESC& src, const D3D12_RESOURCE_DESC& dst,
                             unsigned width, unsigned height, unsigned srcX, unsigned srcY,
                             unsigned dstX, unsigned dstY)
{
    return width != 0 && height != 0 && srcX == 0 && srcY == 0 && dstX == 0 && dstY == 0 &&
           src.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           dst.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && src.Format == dst.Format &&
           src.SampleDesc.Count == 1 && dst.SampleDesc.Count == 1 &&
           src.DepthOrArraySize == 1 && dst.DepthOrArraySize == 1 &&
           src.MipLevels == 1 && dst.MipLevels == 1 &&
           src.Width >= width && src.Height >= height && dst.Width >= width && dst.Height >= height;
}
}

