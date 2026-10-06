#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <map>
#include <cstring>
#include "DepthDebugMenuRegions.h"

// Diagnostic only. Descriptors are indexed by the owner's fence-protected UI
// slot; constants are embedded in the command list, not a mutable upload buffer.
class FgDepthDebug
{
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    struct Slot { ComPtr<ID3D12DescriptorHeap> srv, rtv; };
    std::array<Slot, 4> slots;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3DBlob> vs, ps;
    std::map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> pipelines;

    bool Init(ID3D12Device* device)
    {
        if (root) return true;
        const char* code = R"(
Texture2D<float> depthTex : register(t0);
cbuffer Params : register(b0) { uint width; uint height; uint left; uint top;
                              float gain; uint inverted; uint enhanced; uint menuCount; float4 menuRects[8]; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V VSMain(uint id : SV_VertexID) {
    V o; o.uv=float2((id<<1)&2,id&2);
    o.pos=float4(o.uv*float2(2,-2)+float2(-1,1),0,1); return o;
}
float4 PSMain(V i) : SV_Target {
    for (uint n=0;n<menuCount;++n) {
        float4 r=menuRects[n];
        if (all(i.uv>=r.xy) && all(i.uv<=r.zw)) discard;
    }
    uint2 p=min(uint2(saturate(i.uv)*float2(width,height)),uint2(width-1,height-1));
    float d=depthTex.Load(int3(p+uint2(left,top),0));
    if (!isfinite(d)) return float4(1,0,1,1);
    if (enhanced != 0) {
        // Classify RAW values before preview inversion. Never modify FG input.
        if (d == 0) return float4(0,0.15,1,1);
        if (d == 1) return float4(1,1,0,1);
        if (d < 0 || d > 1) return float4(1,0,0,1);
        float z = inverted != 0 ? 1-d : d;
        float v = 0.12 + 0.88*saturate((log2(max(z*gain,1e-12))+40)/40);
        return float4(v,v,v,1);
    }
    float v=saturate((inverted != 0 ? 1-d : d)*gain);
    return float4(v,v,v,1);
})";
        ComPtr<ID3DBlob> errors, serialized;
        if (FAILED(D3DCompile(code, std::strlen(code), nullptr, nullptr, nullptr, "VSMain", "vs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors)) ||
            FAILED(D3DCompile(code, std::strlen(code), nullptr, nullptr, nullptr, "PSMain", "ps_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors))) return false;
        D3D12_DESCRIPTOR_RANGE range {};
        range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors=1;
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable={1,&range}; params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants={0,0,40}; params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc {2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        if (FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors))) return false;
        ComPtr<ID3D12RootSignature> ready;
        if (FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&ready)))) return false;
        for (auto& slot : slots) {
            D3D12_DESCRIPTOR_HEAP_DESC hd {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            if (FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&slot.srv)))) return false;
            hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&slot.rtv)))) return false;
        }
        root=ready; return true;
    }
    static void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        if (before==after) return;
        D3D12_RESOURCE_BARRIER b {}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; cmd->ResourceBarrier(1,&b);
    }
public:
    // Caller must fence this slot before reuse and retain source/target until GPU completion.
    bool Draw(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, UINT slotIndex,
              ID3D12Resource* depth, D3D12_RESOURCE_STATES depthState, ID3D12Resource* target,
              UINT width, UINT height, UINT left, UINT top, float gain, bool invert, bool enhanced = false, bool preserveFallbackMenu = false)
    {
        if (!device || !cmd || !depth || !target || slotIndex>=slots.size() || !width || !height) return false;
        const auto dd=depth->GetDesc(), td=target->GetDesc();
        if (UINT64(left)+width>dd.Width || UINT64(top)+height>dd.Height || !Init(device)) return false;
        auto& pipeline=pipelines[td.Format];
        if (!pipeline) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
            pd.pRootSignature=root.Get(); pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
            pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()}; pd.SampleMask=UINT_MAX;
            pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable=TRUE;
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets=1; pd.RTVFormats[0]=td.Format; pd.SampleDesc.Count=1;
            if (FAILED(device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pipeline)))) return false;
        }
        DXGI_FORMAT format=dd.Format;
        if (format==DXGI_FORMAT_R32_TYPELESS || format==DXGI_FORMAT_D32_FLOAT) format=DXGI_FORMAT_R32_FLOAT;
        if (format==DXGI_FORMAT_R24G8_TYPELESS || format==DXGI_FORMAT_D24_UNORM_S8_UINT) format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        if (format==DXGI_FORMAT_R32G8X24_TYPELESS || format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT) format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        if (format==DXGI_FORMAT_R16_TYPELESS || format==DXGI_FORMAT_D16_UNORM) format=DXGI_FORMAT_R16_UNORM;
        auto& slot=slots[slotIndex];
        D3D12_SHADER_RESOURCE_VIEW_DESC sd {}; sd.Format=format; sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(depth,&sd,slot.srv->GetCPUDescriptorHandleForHeapStart());
        device->CreateRenderTargetView(target,nullptr,slot.rtv->GetCPUDescriptorHandleForHeapStart());
        Barrier(cmd,depth,depthState,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(cmd,target,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->SetGraphicsRootSignature(root.Get()); cmd->SetPipelineState(pipeline.Get());
        auto* heap=slot.srv.Get(); cmd->SetDescriptorHeaps(1,&heap);
        cmd->SetGraphicsRootDescriptorTable(0,slot.srv->GetGPUDescriptorHandleForHeapStart());
        struct Params { UINT w,h,l,t; float gain; UINT inverted,enhanced,menuCount;
                        std::array<DepthDebugMenuRegions::Rect,8> menuRects; };
        Params values {width,height,left,top,gain,invert?1u:0u,enhanced?1u:0u,0,{}};
        if (preserveFallbackMenu) {
            const auto menu=DepthDebugMenuRegions::Read(GetTickCount64());
            values.menuCount=menu.count; values.menuRects=menu.rects;
        }
        static_assert(sizeof(Params)==40*sizeof(UINT));
        cmd->SetGraphicsRoot32BitConstants(1,40,&values,0);
        D3D12_VIEWPORT viewport {0,0,float(td.Width),float(td.Height),0,1};
        D3D12_RECT rect {0,0,LONG(td.Width),LONG(td.Height)};
        cmd->RSSetViewports(1,&viewport); cmd->RSSetScissorRects(1,&rect);
        auto rtv=slot.rtv->GetCPUDescriptorHandleForHeapStart(); cmd->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); cmd->DrawInstanced(3,1,0,0);
        Barrier(cmd,target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
        Barrier(cmd,depth,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,depthState);
        return true;
    }
};
