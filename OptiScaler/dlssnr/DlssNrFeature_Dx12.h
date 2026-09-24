#pragma once

#include "DlssNr_Status.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <string>
#include <shaders/dlssnr/DlssNr_Common.h>
#include <nvsdk_ngx.h>

namespace DlssNr
{
inline constexpr unsigned int MaxPassCount = 30;
inline constexpr unsigned int DefaultMaxPassCount = 3;

// Public callbacks route through registered upscaler owners. They do not own GPU state.
std::string FinishedPictureStatus();
bool WaitForFinishedPicture();
void FinishedPictureResetCommandList(ID3D12CommandList* cmd);
void FinishedPictureSubmitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
void ApplyToFinishedPicture(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue);
// FG pre-present hand-off: the backbuffer is the frame DLSSG is about to interpolate from, so the
// compose is valid even with the DLSSG output active -- generated frames inherit the NR look.
void ApplyToFinishedPictureFg(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue);
// Dx11wDx12SC::Present hand-off, before the FG swapchain's own Present.
void ApplyToFinishedPictureBridge(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue);
// True when the bridge already applied NR for the frame currently in the FG present hook.
bool ConsumeBridgeAppliedEpoch();
void ApplyToStreamlinePicture(IDXGISwapChain* swapchain, ID3D12Resource* picture, ID3D12CommandQueue* queue);
void ApplyToFinishedPictureDx11(IDXGISwapChain* swapchain);
void FinishedPictureColorSpace(IDXGISwapChain* swapchain, DXGI_COLOR_SPACE_TYPE colorSpace);

// Suggested exposure calibration and steadiness; the user chooses whether to apply it.
struct CalibrationReading
{
    float suggestion = 0.0f;
    float steadiness = 0.0f;
    unsigned long long samples = 0;
    bool usable = false;
    const char* why = "";
};
CalibrationReading Calibration();
std::string DeferredDlssStatus();
void Shutdown();
} // namespace DlssNr
