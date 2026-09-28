#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>

// Depth conversion can run AFTER native DLSS restores the game's output bindings.
// A writable DSV overlapping our SRV makes CSSetShaderResources silently bind NULL.
// Preserve every state this pass changes; do not clear the game's whole context.
class DepthTransferBindings
{
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    ID3D11DeviceContext* context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11DepthStencilView> dsv;
    std::array<ID3D11ClassInstance*, 256> instances {};
    UINT instanceCount = 256;
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs {};
    UINT rtvCount = 0;
    bool detached = false;
public:
    DepthTransferBindings(ID3D11DeviceContext* ctx, ID3D11Resource* source) : context(ctx)
    {
        context->CSGetShader(&shader, instances.data(), &instanceCount);
        context->CSGetShaderResources(0, 1, &srv);
        context->CSGetUnorderedAccessViews(0, 1, &uav);
        context->OMGetRenderTargets(0, nullptr, &dsv);
        if (dsv)
        {
            ComPtr<ID3D11Resource> bound;
            dsv->GetResource(&bound);
            ComPtr<IUnknown> boundIdentity, sourceIdentity;
            bound.As(&boundIdentity);
            source->QueryInterface(IID_PPV_ARGS(&sourceIdentity));
            if (boundIdentity && boundIdentity.Get() == sourceIdentity.Get())
            {
                context->OMGetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), nullptr);
                // Preserve OM UAV slots too: eight trailing null RTVs would overlap them.
                for (UINT i = 0; i < rtvs.size(); ++i) if (rtvs[i]) rtvCount = i + 1;
                context->OMSetRenderTargetsAndUnorderedAccessViews(rtvCount, rtvs.data(), nullptr,
                    0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
                detached = true;
            }
        }
    }
    bool DetachedDepth() const { return detached; }
    ~DepthTransferBindings()
    {
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ID3D11UnorderedAccessView* nullUav = nullptr;
        context->CSSetShaderResources(0, 1, &nullSrv);
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        if (detached)
            context->OMSetRenderTargetsAndUnorderedAccessViews(rtvCount, rtvs.data(), dsv.Get(),
                0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
        auto* savedSrv = srv.Get(); auto* savedUav = uav.Get();
        context->CSSetShaderResources(0, 1, &savedSrv);
        context->CSSetUnorderedAccessViews(0, 1, &savedUav, nullptr);
        context->CSSetShader(shader.Get(), instances.data(), instanceCount);
        for (auto* rtv : rtvs) if (rtv) rtv->Release();
        for (auto* instance : instances) if (instance) instance->Release();
    }
    DepthTransferBindings(const DepthTransferBindings&) = delete;
    DepthTransferBindings& operator=(const DepthTransferBindings&) = delete;
};
