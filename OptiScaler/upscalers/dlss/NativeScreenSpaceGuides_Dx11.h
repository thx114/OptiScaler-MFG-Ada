#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

// Native DX11 NGX capture and Present use different row coordinates in Genshin.
// Keep SR's complete contract consistent while exposing screen-space guides to NR.
class NativeScreenSpaceGuides_Dx11
{
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
  public:
    enum Role : unsigned { Color, Depth, Motion, Mask, Output, RoleCount };
    inline static constexpr const char* Names[RoleCount] = {
        NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_Depth, NVSDK_NGX_Parameter_MotionVectors,
        NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, NVSDK_NGX_Parameter_Output };
    inline static constexpr char Shader[] = R"(
cbuffer Extent : register(b0) { uint width; uint height; uint2 reserved; };
Texture2D<float4> source : register(t0);
RWTexture2D<float4> destination : register(u0);
[numthreads(16,16,1)]
void CSMain(uint3 p : SV_DispatchThreadID) {
    if(p.x < width && p.y < height)
        destination[p.xy] = source.Load(int3(p.x, height - 1 - p.y, 0));
})";
  private:
    struct Surface {
        Ptr<ID3D11Texture2D> texture;
        Ptr<ID3D11ShaderResourceView> read;
        Ptr<ID3D11UnorderedAccessView> write;
    };
    Ptr<ID3D11Device> device;
    Ptr<ID3D11ComputeShader> shader;
    Ptr<ID3D11Buffer> constants;
    Surface converted[RoleCount], copyBack, rawOutput;
    Ptr<ID3D11ShaderResourceView> inputs[RoleCount];
    std::array<Ptr<ID3D11Resource>, RoleCount> originals;
    float mvY = 1, jitterY = 0;
    const char* reason = "not prepared";

    // Never keep the game's resources or their SRVs between evaluate calls:
    // an original Output can be a swapchain buffer, which must be released before ResizeBuffers.
    void ReleaseBorrowedInputs() {
        for(auto& input : inputs) input.Reset();
        for(auto& original : originals) original.Reset();
    }

    static DXGI_FORMAT ReadFormat(DXGI_FORMAT format) {
        switch(format) {
        case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default: return format;
        }
    }
    static bool FloatView(DXGI_FORMAT format) {
        switch(format) {
        case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM: return true;
        default: return false;
        }
    }
    HRESULT Init(ID3D11Device* dev) {
        if(shader && device.Get()==dev) return S_OK;
        device = dev;
        Ptr<ID3DBlob> code, errors;
        auto hr = D3DCompile(Shader, std::strlen(Shader), "NativeScreenSpaceGuides_Dx11", nullptr, nullptr,
                             "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if(FAILED(hr)) return hr;
        hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                      shader.ReleaseAndGetAddressOf());
        if(FAILED(hr)) return hr;
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth=16; desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        return dev->CreateBuffer(&desc,nullptr,constants.ReleaseAndGetAddressOf());
    }
    HRESULT Ensure(Surface& surface, D3D11_TEXTURE2D_DESC desc, DXGI_FORMAT view) {
        if(surface.texture) {
            D3D11_TEXTURE2D_DESC old {}; surface.texture->GetDesc(&old);
            if(old.Width==desc.Width && old.Height==desc.Height && old.Format==desc.Format) return S_OK;
        }
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        desc.Usage=D3D11_USAGE_DEFAULT; desc.CPUAccessFlags=desc.MiscFlags=0;
        Surface replacement;
        auto hr=device->CreateTexture2D(&desc,nullptr,&replacement.texture);
        if(FAILED(hr)) return hr;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format=view; srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels=1;
        hr=device->CreateShaderResourceView(replacement.texture.Get(),&srv,&replacement.read);
        if(FAILED(hr)) return hr;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format=view; uav.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
        hr=device->CreateUnorderedAccessView(replacement.texture.Get(),&uav,&replacement.write);
        if(SUCCEEDED(hr)) surface=std::move(replacement);
        return hr;
    }
    // A pass changes only CS t0/u0/b0 and the shader. Restore class instances as well.
    void Flip(ID3D11DeviceContext* context, ID3D11ShaderResourceView* input,
              ID3D11UnorderedAccessView* output, unsigned width, unsigned height) {
        Ptr<ID3D11ComputeShader> previousShader;
        Ptr<ID3D11ShaderResourceView> previousSrv;
        Ptr<ID3D11UnorderedAccessView> previousUav;
        Ptr<ID3D11Buffer> previousCb;
        ID3D11ClassInstance* instances[256] {}; UINT count=256;
        context->CSGetShader(&previousShader,instances,&count);
        context->CSGetShaderResources(0,1,&previousSrv);
        context->CSGetUnorderedAccessViews(0,1,&previousUav);
        context->CSGetConstantBuffers(0,1,&previousCb);
        ID3D11ShaderResourceView* nullSrv=nullptr; ID3D11UnorderedAccessView* nullUav=nullptr;
        context->CSSetShaderResources(0,1,&nullSrv);
        context->CSSetUnorderedAccessViews(0,1,&nullUav,nullptr);
        unsigned extent[4]={width,height,0,0}; context->UpdateSubresource(constants.Get(),0,nullptr,extent,0,0);
        auto* cb=constants.Get(); context->CSSetConstantBuffers(0,1,&cb);
        context->CSSetShader(shader.Get(),nullptr,0);
        context->CSSetShaderResources(0,1,&input); context->CSSetUnorderedAccessViews(0,1,&output,nullptr);
        context->Dispatch((width+15)/16,(height+15)/16,1);
        context->CSSetShaderResources(0,1,&nullSrv); context->CSSetUnorderedAccessViews(0,1,&nullUav,nullptr);
        auto* oldSrv=previousSrv.Get(); auto* oldUav=previousUav.Get(); auto* oldCb=previousCb.Get();
        context->CSSetConstantBuffers(0,1,&oldCb);
        context->CSSetShaderResources(0,1,&oldSrv); context->CSSetUnorderedAccessViews(0,1,&oldUav,nullptr);
        context->CSSetShader(previousShader.Get(),instances,count);
        for(UINT i=0;i<count;i++) if(instances[i]) instances[i]->Release();
    }
  public:
    const char* Reason() const { return reason; }
    ID3D11Resource* Original(Role role) const { return originals[role].Get(); }
    ID3D11Texture2D* Converted(Role role) const { return converted[role].texture.Get(); }
    float OriginalMvY() const { return mvY; }
    float OriginalJitterY() const { return jitterY; }

    HRESULT Prepare(ID3D11Device* dev, ID3D11DeviceContext* context, NVSDK_NGX_Parameter* params,
                    unsigned rw, unsigned rh, unsigned tw, unsigned th) {
        ReleaseBorrowedInputs();
        struct CleanupFailedPrepare {
            NativeScreenSpaceGuides_Dx11& owner; bool committed=false;
            ~CleanupFailedPrepare() { if(!committed) owner.ReleaseBorrowedInputs(); }
        } cleanup{*this};
        reason="context/device";
        if(!dev || !context || !params || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return E_INVALIDARG;
        Ptr<ID3D11Device> contextDevice; context->GetDevice(&contextDevice);
        if(contextDevice.Get()!=dev) return E_INVALIDARG;
        if(device && device.Get()!=dev) return E_INVALIDARG;
        reason="metadata";
        if(params->Get(NVSDK_NGX_Parameter_MV_Scale_Y,&mvY)!=NVSDK_NGX_Result_Success ||
           params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y,&jitterY)!=NVSDK_NGX_Result_Success ||
           !std::isfinite(mvY) || !std::isfinite(jitterY)) return E_INVALIDARG;
        unsigned frameW=0,frameH=0;
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,&frameW);
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,&frameH);
        if(frameW) rw=frameW; if(frameH) rh=frameH;
        reason="nonzero subrect origin";
        const char* origins[]={NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
            NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
            NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
            NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X,NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y,
            NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y};
        for(auto* name:origins) { unsigned base=0; params->Get(name,&base); if(base) return E_INVALIDARG; }
        std::array<D3D11_TEXTURE2D_DESC,RoleCount> descriptions {};
        std::array<Ptr<ID3D11Resource>,RoleCount> newOriginals;
        for(unsigned role=0;role<RoleCount;role++) {
            ID3D11Resource* resource=nullptr;
            if(params->Get(Names[role],&resource)!=NVSDK_NGX_Result_Success) {
                void* pointer=nullptr; params->Get(Names[role],&pointer); resource=static_cast<ID3D11Resource*>(pointer);
            }
            reason="missing required input";
            if(!resource) { if(role==Mask) continue; return E_INVALIDARG; }
            newOriginals[role]=resource;
            Ptr<ID3D11Texture2D> texture;
            if(FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return E_INVALIDARG;
            auto& d=descriptions[role]; texture->GetDesc(&d);
            reason="extent/mip/array/sample";
            if(d.MipLevels!=1 || d.ArraySize!=1 || d.SampleDesc.Count!=1 || !d.Width || !d.Height) return E_INVALIDARG;
            if(role==Output) { if(d.Width!=tw || d.Height!=th) return E_INVALIDARG; }
            else if(role==Motion) { if((d.Width!=rw||d.Height!=rh)&&(d.Width!=tw||d.Height!=th)) return E_INVALIDARG; }
            else if(d.Width!=rw || d.Height!=rh) return E_INVALIDARG;
            reason="input format";
            auto view=ReadFormat(d.Format);
            bool depthPlane=role==Depth && (view==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS||view==DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
            if(!FloatView(view) && !depthPlane && !(role==Output && d.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) return E_INVALIDARG;
        }
        reason="shader initialization";
        auto hr=Init(dev); if(FAILED(hr)) return hr;
        for(unsigned role=0;role<RoleCount;role++) {
            if(!newOriginals[role]) { inputs[role].Reset(); continue; }
            auto d=descriptions[role]; auto view=ReadFormat(d.Format);
            reason="input SRV";
            D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
            srv.Format=view; srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels=1;
            // Native output need not expose an SRV; conversion writes a separate clone.
            if(role!=Output && (!inputs[role] || originals[role].Get()!=newOriginals[role].Get())) {
                inputs[role].Reset();
                hr=dev->CreateShaderResourceView(newOriginals[role].Get(),&srv,&inputs[role]);
                if(FAILED(hr)) return hr;
            }
            if(role==Depth) { d.Format=DXGI_FORMAT_R32_FLOAT; view=d.Format; }
            reason="conversion texture/UAV";
            if(role==Output && d.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                // Preserve the format seen by NGX/NR. SRGB cannot have a UAV;
                // raw copy through typeless storage avoids gamma conversion on return.
                bool recreate=!converted[role].texture;
                if(!recreate) {
                    D3D11_TEXTURE2D_DESC old {}; converted[role].texture->GetDesc(&old);
                    recreate=old.Width!=d.Width || old.Height!=d.Height || old.Format!=d.Format;
                }
                if(recreate) {
                    Surface replacement;
                    auto originalDesc=d;
                    originalDesc.Usage=D3D11_USAGE_DEFAULT;
                    originalDesc.CPUAccessFlags=originalDesc.MiscFlags=0;
                    hr=dev->CreateTexture2D(&originalDesc,nullptr,&replacement.texture);
                    if(FAILED(hr)) return hr;
                    converted[role]=std::move(replacement);
                }
                d.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;
                view=DXGI_FORMAT_R8G8B8A8_UNORM;
                hr=Ensure(rawOutput,d,view); if(FAILED(hr)) return hr;
            } else {
                hr=Ensure(converted[role],d,view); if(FAILED(hr)) return hr;
            }
            if(role==Output) { hr=Ensure(copyBack,d,view); if(FAILED(hr)) return hr; }
        }
        originals=std::move(newOriginals);
        cleanup.committed=true;
        reason="ready"; return S_OK;
    }

    // Invoke receives the converted native DX11 parameter table, exactly where NR
    // detours NGX. Restore before returning so downstream FG sees its original set.
    template<class Invoke>
    NVSDK_NGX_Result Evaluate(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* params,
                             bool resetHistory, Invoke&& invoke) {
        struct Restore {
            NativeScreenSpaceGuides_Dx11& owner; NVSDK_NGX_Parameter* params; bool resetHistory; unsigned oldReset;
            ~Restore() {
                for(unsigned role=0;role<RoleCount;role++) params->Set(Names[role],owner.originals[role].Get());
                params->Set(NVSDK_NGX_Parameter_MV_Scale_Y,owner.mvY);
                params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,owner.jitterY);
                if(resetHistory) {
                    if(signedReset) params->Set(NVSDK_NGX_Parameter_Reset,static_cast<int>(oldReset));
                    else params->Set(NVSDK_NGX_Parameter_Reset,oldReset);
                }
                owner.ReleaseBorrowedInputs();
            }
            bool signedReset=false;
        } restore{*this,params,resetHistory,0};
        if(resetHistory) {
            if(params->Get(NVSDK_NGX_Parameter_Reset,&restore.oldReset)!=NVSDK_NGX_Result_Success) {
                int signedValue=0;
                restore.signedReset=params->Get(NVSDK_NGX_Parameter_Reset,&signedValue)==NVSDK_NGX_Result_Success;
                restore.oldReset=static_cast<unsigned>(signedValue);
            }
            if(restore.signedReset) params->Set(NVSDK_NGX_Parameter_Reset,1);
            else params->Set(NVSDK_NGX_Parameter_Reset,1u);
        }
        for(unsigned role=0;role<RoleCount;role++) {
            if(!originals[role]) continue;
            if(role!=Output) {
                D3D11_TEXTURE2D_DESC d {}; converted[role].texture->GetDesc(&d);
                Flip(context,inputs[role].Get(),converted[role].write.Get(),d.Width,d.Height);
            }
            params->Set(Names[role],static_cast<ID3D11Resource*>(converted[role].texture.Get()));
        }
        params->Set(NVSDK_NGX_Parameter_MV_Scale_Y,-mvY);
        params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,-jitterY);
        const auto result=invoke();
        if(result==NVSDK_NGX_Result_Success) {
            D3D11_TEXTURE2D_DESC d {}; converted[Output].texture->GetDesc(&d);
            auto* outputRead=converted[Output].read.Get();
            if(!outputRead) {
                context->CopyResource(rawOutput.texture.Get(),converted[Output].texture.Get());
                outputRead=rawOutput.read.Get();
            }
            Flip(context,outputRead,copyBack.write.Get(),d.Width,d.Height);
            context->CopyResource(originals[Output].Get(),copyBack.texture.Get());
        }
        return result;
    }
};
