#pragma once
#include <dxgi1_4.h>

// A hidden DX11 (possibly one-buffer, sRGB) chain and the visible DX12 flip
// chain have independent buffer counts, formats and creation flags.
struct Dx11FgResizeArgs
{
    UINT bufferCount = 0; // Preserve FG/Streamline's ring, not the game's DX11 count.
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT flags = 0;
};
inline HRESULT ResolveDx11FgResize(const DXGI_SWAP_CHAIN_DESC& real,
                                  const DXGI_SWAP_CHAIN_DESC1& fg,
                                  Dx11FgResizeArgs& out)
{
    if (fg.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD &&
        fg.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) return E_INVALIDARG;
    if (fg.BufferCount < 2 || !real.BufferDesc.Width || !real.BufferDesc.Height) return E_INVALIDARG;
    out.width = real.BufferDesc.Width;
    out.height = real.BufferDesc.Height;
    out.format = real.BufferDesc.Format;
    out.flags = fg.Flags;
    switch (out.format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: out.format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: out.format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM: break;
    default: return E_INVALIDARG;
    }
    return S_OK;
}
inline HRESULT ResizeDx11FgSwapchain(IDXGISwapChain* real, IDXGISwapChain1* fg,
                                     Dx11FgResizeArgs* submitted = nullptr)
{
    if (!real || !fg) return E_POINTER;
    DXGI_SWAP_CHAIN_DESC realDesc {};
    DXGI_SWAP_CHAIN_DESC1 fgDesc {};
    auto hr = real->GetDesc(&realDesc);
    if (FAILED(hr)) return hr;
    hr = fg->GetDesc1(&fgDesc);
    if (FAILED(hr)) return hr;
    Dx11FgResizeArgs args {};
    hr = ResolveDx11FgResize(realDesc, fgDesc, args);
    if (FAILED(hr)) return hr;
    if (submitted) *submitted = args;
    return fg->ResizeBuffers(args.bufferCount, args.width, args.height, args.format, args.flags);
}
