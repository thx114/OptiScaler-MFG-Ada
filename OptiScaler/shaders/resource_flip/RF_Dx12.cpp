#include "pch.h"
#include "RF_Dx12.h"

#include "RF_Common.h"

#include <Config.h>
#include <State.h>

#include "precompiled/RF_Shader.h"

bool RF_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* OutResource,
                       UINT64 width, UINT height, bool velocity, UINT frameSlot, D3D12_RESOURCE_STATES inputState)
{
    if (!_init || _device == nullptr || InCmdList == nullptr || InResource == nullptr || OutResource == nullptr)
        return false;

    const auto inDesc = InResource->GetDesc();
    const auto outDesc = OutResource->GetDesc();
    if (frameSlot >= RF_NUM_OF_HEAPS || width == 0 || height == 0 || width > inDesc.Width ||
        height > inDesc.Height || width > outDesc.Width || height > outDesc.Height)
        return false;

    LOG_DEBUG("[{0}] Start!", _name);

    ScopedGpuTime_Dx12 scopedGpuTime(GpuTime.get(), InCmdList);

    // The owner waits this FG slot's UI fence before recording. Never use an
    // independent two-entry ring: FG permits four slots in flight.
    FrameDescriptorHeap& currentHeap = _frameHeaps[frameSlot];

    CreateShaderResourceView(_device, InResource, currentHeap.GetSrvCPU(0));
    CreateUnorderedAccessView(_device, OutResource, currentHeap.GetUavCPU(0), 0);

    RFConstants constants {};

    constants.height = height - 1;
    constants.width = static_cast<UINT>(width - 1);
    constants.offset = Config::Instance()->FGResourceFlipOffset.value_or_default() ? inDesc.Height - height : 0;
    constants.velocity = velocity ? 1 : 0;

    LOG_DEBUG("Width: {}, Height: {}, Offset", constants.width, constants.height, constants.offset);

    if (!CreateConstantsBuffer(_device, _frameConstants[frameSlot], constants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);

    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    // Dispatch the tagged extent, not the current feature's MV resolution (depth
    // can remain low-resolution while motion vectors are full-resolution).
    const UINT dispatchWidth = static_cast<UINT>((width + InNumThreadsX - 1) / InNumThreadsX);
    const UINT dispatchHeight = (height + InNumThreadsY - 1) / InNumThreadsY;
    if (inputState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(InResource, inputState,
                                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        InCmdList->ResourceBarrier(1, &barrier);
    }
    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);
    auto written = CD3DX12_RESOURCE_BARRIER::UAV(OutResource);
    InCmdList->ResourceBarrier(1, &written);
    if (inputState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(InResource,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, inputState);
        InCmdList->ResourceBarrier(1, &barrier);
    }

    return true;
}

RF_Dx12::RF_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    if (!SetupRootSignature(InDevice, 1, 1, 1))
    {
        LOG_ERROR("Failed to setup root signature");
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(RFConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (auto& constants : _frameConstants)
    {
        auto result = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ,
                                                        nullptr, IID_PPV_ARGS(&constants));

        if (result != S_OK)
        {
            LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
            return;
        }
    }

    if (!CreateComputePipeline(InDevice, &_pipelineState, RF_cso, sizeof(RF_cso), rfCode.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, RF_NUM_OF_HEAPS);
}

RF_Dx12::~RF_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < RF_NUM_OF_HEAPS; i++)
    {
        _frameHeaps[i].ReleaseHeaps();
        SAFE_RELEASE(_frameConstants[i]);
    }
}
