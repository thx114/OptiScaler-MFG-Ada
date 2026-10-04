#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <utility>
#include <cstring>

// Isolated from OptiFG.ResourceFlip: preserve all channels and only reverse rows.
inline constexpr char Dx11ScreenSpaceFlipShader[] = R"(
cbuffer Extent : register(b0) { uint width; uint height; };
Texture2D<float4> source : register(t0);
RWTexture2D<float4> destination : register(u0);
[numthreads(16, 16, 1)]
void CSMain(uint3 p : SV_DispatchThreadID) {
    if (p.x < width && p.y < height)
        destination[p.xy] = source.Load(int3(p.x, height - 1 - p.y, 0));
}
)";

// The bridge waits its allocator fence before Prepare/Flip reuse a slot.
// Every resource enters/leaves a pass in COMMON, like the shared bridge textures.
class Dx11ScreenSpaceGuides
{
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
  public:
    enum Role : UINT { Color, Motion, Depth, Mask, Output, RoleCount };
    static constexpr UINT SlotCount = 2;
  private:
    Ptr<ID3D12Device> device;
    Ptr<ID3D12RootSignature> root;
    Ptr<ID3D12PipelineState> pipeline;
    Ptr<ID3D12DescriptorHeap> heaps[SlotCount][RoleCount];
    Ptr<ID3D12Resource> textures[SlotCount][RoleCount];
    UINT descriptorSize = 0;

    static DXGI_FORMAT ViewFormat(DXGI_FORMAT format)
    {
        switch (format) {
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        default: return format;
        }
    }
    static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        list->ResourceBarrier(1, &b);
    }
  public:
    HRESULT Init(ID3D12Device* dev)
    {
        if (pipeline) return S_OK;
        device = dev;
        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        for (auto& range : ranges) {
            range.NumDescriptors = 1;
            range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        }
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, 2 };
        D3D12_ROOT_SIGNATURE_DESC desc {};
        desc.NumParameters = 2;
        desc.pParameters = params;
        Ptr<ID3DBlob> signature, errors, shader;
        auto hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors);
        if (FAILED(hr)) return hr;
        hr = dev->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(root.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) return hr;
        hr = D3DCompile(Dx11ScreenSpaceFlipShader, std::strlen(Dx11ScreenSpaceFlipShader),
                        "Dx11ScreenSpaceFlip", nullptr, nullptr, "CSMain", "cs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
        if (FAILED(hr)) return hr;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
        pso.pRootSignature = root.Get();
        pso.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        hr = dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline));
        descriptorSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return hr;
    }
    HRESULT Prepare(UINT slot, Role role, ID3D12Resource* source)
    {
        if (!pipeline || slot >= SlotCount || role >= RoleCount || !source) return E_INVALIDARG;
        auto desc = source->GetDesc();
        // Cropped, multisample, array and depth/stencil-packed resources need a separate implementation.
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
            desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || desc.Width == 0 || desc.Height == 0 ||
            (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) return E_INVALIDARG;
        desc.Format = ViewFormat(desc.Format);
        // The shader uses float loads/stores. Integer and packed depth/stencil views
        // cannot use this conversion even if the device supports typed UAV stores.
        switch (desc.Format) {
        case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            break;
        default: return E_INVALIDARG;
        }
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support { desc.Format };
        auto hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
        if (FAILED(hr)) return hr;
        if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ||
            !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) return E_INVALIDARG;
        if (!heaps[slot][role]) {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.NumDescriptors = 2;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heaps[slot][role]));
            if (FAILED(hr)) return hr;
        }
        auto& texture = textures[slot][role];
        if (texture) {
            auto previous = texture->GetDesc();
            if (previous.Width == desc.Width && previous.Height == desc.Height && previous.Format == desc.Format)
                return S_OK;
        }
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES properties {};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        Ptr<ID3D12Resource> replacement;
        hr = device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&replacement));
        if (SUCCEEDED(hr)) texture = std::move(replacement);
        return hr;
    }
    ID3D12Resource* Get(UINT slot, Role role) const { return textures[slot][role].Get(); }
    void Flip(ID3D12GraphicsCommandList* list, UINT slot, Role role,
              ID3D12Resource* source, ID3D12Resource* dest)
    {
        auto desc = source->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = ViewFormat(desc.Format);
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = ViewFormat(dest->GetDesc().Format);
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        auto* heap = heaps[slot][role].Get();
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateShaderResourceView(source, &srv, cpu);
        cpu.ptr += descriptorSize;
        device->CreateUnorderedAccessView(dest, nullptr, &uav, cpu);
        list->SetDescriptorHeaps(1, &heap);
        list->SetComputeRootSignature(root.Get());
        list->SetPipelineState(pipeline.Get());
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        UINT extent[2] { static_cast<UINT>(desc.Width), desc.Height };
        list->SetComputeRoot32BitConstants(1, 2, extent, 0);
        Barrier(list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list, dest, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->Dispatch((extent[0] + 15) / 16, (extent[1] + 15) / 16, 1);
        Barrier(list, dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Barrier(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
};
