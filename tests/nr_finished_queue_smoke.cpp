// A native FG present must not wait for render work gated by that same present.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include "../OptiScaler/dlssnr/DlssNr_FinishedReady.h"

using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 call failed"); }
static void Expect(bool yes, const char* why) { if (!yes) throw std::runtime_error(why); }

int main() try
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    D3D12_COMMAND_QUEUE_DESC desc {};
    ComPtr<ID3D12CommandQueue> render, present;
    Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&render)));
    Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&present)));
    ComPtr<ID3D12Fence> presentGate, inputReady;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentGate)));
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&inputReady)));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Expect(event != nullptr, "Could not create completion event");
    for (UINT64 frame = 1; frame <= 16; ++frame)
    {
        // Model the next render submission waiting for the current native FG presentation.
        Check(render->Wait(presentGate.Get(), frame));
        Check(render->Signal(inputReady.Get(), frame));
        const auto completed = inputReady->GetCompletedValue();
        const bool crossQueueReady = DlssNr::FinishedInputReady(false, completed, frame);
        const bool sameQueueReady = DlssNr::FinishedInputReady(true, completed, frame);
        const bool olderInputReady = DlssNr::FinishedInputReady(false, completed, frame - 1);
        // Do not enqueue present->Wait(inputReady, frame): that would deadlock these queues.
        Check(present->Signal(presentGate.Get(), frame));
        Check(inputReady->SetEventOnCompletion(frame, event));
        const auto result = WaitForSingleObject(event, 5000);
        if (result != WAIT_OBJECT_0) presentGate->Signal(frame); // cleanup even on regression failure
        Expect(result == WAIT_OBJECT_0, "Present/render queues stalled");
        Expect(!crossQueueReady, "Accepted future render input on the presentation queue");
        Expect(sameQueueReady, "Rejected ordered same-queue input");
        Expect(olderInputReady, "Rejected the previous completed input");
        Expect(DlssNr::FinishedInputReady(false, inputReady->GetCompletedValue(), frame),
               "Completed cross-queue input remained unavailable");
    }
    // Model the bridge's already-established render -> interop -> present chain.
    // Keep render deliberately unfinished on the CPU while accepting ordered input.
    ComPtr<ID3D12CommandQueue> interop;
    ComPtr<ID3D12Fence> startGate, bridgeReady, composeDone;
    Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&interop)));
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&startGate)));
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&bridgeReady)));
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&composeDone)));
    for (UINT64 frame = 17; frame <= 32; ++frame)
    {
        Check(render->Wait(startGate.Get(), frame));
        Check(render->Signal(inputReady.Get(), frame));
        DlssNr::FinishedBridgeHandoff handoff;
        handoff.Prepare(true, frame);
        Check(interop->Wait(inputReady.Get(), frame));
        handoff.Commit(true, frame);
        Check(interop->Signal(bridgeReady.Get(), frame));
        Check(present->Wait(bridgeReady.Get(), frame));
        const bool accepted = DlssNr::FinishedInputReady(false, inputReady->GetCompletedValue(), frame,
                                                        handoff.ordered);
        Check(present->Signal(composeDone.Get(), frame));
        const bool premature = composeDone->GetCompletedValue() >= frame;
        Check(startGate->Signal(frame)); // release GPU gate even on assertion failure
        Check(composeDone->SetEventOnCompletion(frame, event));
        Expect(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "Ordered bridge queues stalled");
        Expect(accepted, "Rejected a proven GPU-ordered bridge input");
        Expect(!premature, "Presentation bypassed the bridge dependency");
        Expect(inputReady->GetCompletedValue() >= frame, "Composition overtook input preparation");
    }
    CloseHandle(event);
    Expect(!DlssNr::FinishedInputReady(false, UINT64_MAX, 1), "Accepted a removed device");
    Expect(!DlssNr::FinishedInputReady(true, UINT64_MAX, 1), "Same-queue bypassed device removal");
    puts("PASS finished-picture queue readiness: native FG dependency, same-queue order, GPU-ordered bridge, device removal");
    return 0;
}
catch (const std::exception& e) { puts(e.what()); return 1; }
