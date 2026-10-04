// Exercise the actual native-DX11 contract converter and NGX call boundary on WARP.
#define NOMINMAX
#include <d3d12.h>
#include "../OptiScaler/upscalers/dlss/NativeScreenSpaceGuides_Dx11.h"
#include <d3d11sdklayers.h>
#include <dxgi.h>
#include <vector>
#include <iostream>
#include <map>
#include <variant>
#include <string>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
void check(HRESULT h) { if(FAILED(h)) throw std::runtime_error("HRESULT "+std::to_string(static_cast<unsigned>(h))); }
void require(bool b,const char* message) { if(!b) throw std::runtime_error(message); }
struct DriverParams : NVSDK_NGX_Parameter {
    using Value=std::variant<unsigned long long,float,double,unsigned,int,ID3D11Resource*,void*>;
    std::map<std::string,Value> values;
#define PARAM_TYPE(T) \
    void Set(const char* n,T v) override { values[n]=v; } \
    NVSDK_NGX_Result Get(const char* n,T* v) const override { \
        auto it=values.find(n); if(it!=values.end()) if(auto p=std::get_if<T>(&it->second)) { *v=*p; return NVSDK_NGX_Result_Success; } \
        return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    PARAM_TYPE(unsigned long long) PARAM_TYPE(float) PARAM_TYPE(double)
    PARAM_TYPE(unsigned) PARAM_TYPE(int) PARAM_TYPE(ID3D11Resource*) PARAM_TYPE(void*)
#undef PARAM_TYPE
    void Set(const char*,ID3D12Resource*) override {}
    NVSDK_NGX_Result Get(const char*,ID3D12Resource**) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    void Reset() override { values.clear(); }
};

// The native table accepts DX11 resources. A mock callback substitutes for NVIDIA;
// row conversion/readback and CS restoration execute on the real D3D11 GPU API.
int main() { try {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,
        nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<ID3D11InfoQueue> info; check(device.As(&info));
    auto texture=[&](unsigned w,unsigned h,DXGI_FORMAT fmt,unsigned flags, const void* data,UINT pitch) {
        D3D11_TEXTURE2D_DESC d {}; d.Width=w; d.Height=h; d.MipLevels=d.ArraySize=1;
        d.Format=fmt; d.SampleDesc.Count=1; d.BindFlags=flags;
        D3D11_SUBRESOURCE_DATA initial {data,pitch,0}; ComPtr<ID3D11Texture2D> t;
        check(device->CreateTexture2D(&d,data?&initial:nullptr,&t)); return t;
    };
    auto read=[&](ID3D11Texture2D* t,unsigned bytesPerPixel) {
        D3D11_TEXTURE2D_DESC d {}; t->GetDesc(&d); d.Usage=D3D11_USAGE_STAGING;
        d.BindFlags=d.MiscFlags=0; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging; check(device->CreateTexture2D(&d,nullptr,&staging));
        context->CopyResource(staging.Get(),t); D3D11_MAPPED_SUBRESOURCE mapped {};
        check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        std::vector<unsigned char> result(d.Width*d.Height*bytesPerPixel);
        for(unsigned y=0;y<d.Height;y++) std::memcpy(result.data()+y*d.Width*bytesPerPixel,
            static_cast<const char*>(mapped.pData)+y*mapped.RowPitch,d.Width*bytesPerPixel);
        context->Unmap(staging.Get(),0); return result;
    };
    auto rawResource=[](DriverParams& p,const char* name) { ID3D11Resource* r=nullptr;
        require(p.Get(name,&r)==NVSDK_NGX_Result_Success,"DX11 resource parameter missing"); return r; };
    auto asTexture=[](ID3D11Resource* r) { ComPtr<ID3D11Texture2D> t; check(r->QueryInterface(IID_PPV_ARGS(&t))); return t; };
    auto half=[](float value) { unsigned bits; std::memcpy(&bits,&value,4);
        return static_cast<unsigned short>(((bits>>16)&0x8000)|(((bits>>23)-112)&31)<<10|((bits>>13)&1023)); };
    // Verify both the pass's local restoration and the state at the native NGX boundary.
    auto sentinelReadTexture=texture(2,2,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,nullptr,0);
    auto sentinelWriteTexture=texture(2,2,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_UNORDERED_ACCESS,nullptr,0);
    ComPtr<ID3D11ShaderResourceView> sentinelSrv; check(device->CreateShaderResourceView(sentinelReadTexture.Get(),nullptr,&sentinelSrv));
    ComPtr<ID3D11UnorderedAccessView> sentinelUav; check(device->CreateUnorderedAccessView(sentinelWriteTexture.Get(),nullptr,&sentinelUav));
    ComPtr<ID3DBlob> sentinelCode,compileErrors;
    check(D3DCompile(NativeScreenSpaceGuides_Dx11::Shader,std::strlen(NativeScreenSpaceGuides_Dx11::Shader),nullptr,
        nullptr,nullptr,"CSMain","cs_5_0",0,0,&sentinelCode,&compileErrors));
    ComPtr<ID3D11ComputeShader> sentinelShader;
    check(device->CreateComputeShader(sentinelCode->GetBufferPointer(),sentinelCode->GetBufferSize(),nullptr,&sentinelShader));
    D3D11_BUFFER_DESC cbd {}; cbd.ByteWidth=16; cbd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> sentinelCb; check(device->CreateBuffer(&cbd,nullptr,&sentinelCb));
    auto* sr=sentinelSrv.Get(); auto* uw=sentinelUav.Get(); auto* cb=sentinelCb.Get();
    context->CSSetShader(sentinelShader.Get(),nullptr,0); context->CSSetShaderResources(0,1,&sr);
    context->CSSetUnorderedAccessViews(0,1,&uw,nullptr); context->CSSetConstantBuffers(0,1,&cb);
    auto checkCs=[&] {
        ComPtr<ID3D11ComputeShader> sh; ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav; ComPtr<ID3D11Buffer> cb;
        context->CSGetShader(&sh,nullptr,nullptr); context->CSGetShaderResources(0,1,&srv);
        context->CSGetUnorderedAccessViews(0,1,&uav); context->CSGetConstantBuffers(0,1,&cb);
        require(sh.Get()==sentinelShader.Get() && srv.Get()==sentinelSrv.Get() &&
            uav.Get()==sentinelUav.Get() && cb.Get()==sentinelCb.Get(),"CS state not restored");
    };
    NativeScreenSpaceGuides_Dx11 guides;
    using G=NativeScreenSpaceGuides_Dx11;
    for(unsigned test=0;test<6;test++) {
        unsigned w=17+test,h=9+test,tw=w*2+1,th=h*2+1;
        bool fullMotion=test%2; unsigned mw=fullMotion?tw:w,mh=fullMotion?th:h;
        std::vector<unsigned char> pixels(w*h*4),maskPixels(w*h),outPixels(tw*th*4),sentinel(outPixels.size(),0xA7);
        std::vector<unsigned short> mv(mw*mh*2);
        std::vector<unsigned long long> packedDepth(w*h);
        for(unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++) {
            for(unsigned c=0;c<4;c++) pixels[(y*w+x)*4+c]=static_cast<unsigned char>((1+x+3*y+11*c)%255);
            maskPixels[y*w+x]=static_cast<unsigned char>((x+7*y)%255);
            float d=float(1+x+y*w)/float(w*h+1); unsigned bits; std::memcpy(&bits,&d,4);
            packedDepth[y*w+x]=bits | (static_cast<unsigned long long>(0x5B)<<32);
        }
        for(unsigned y=0;y<mh;y++) for(unsigned x=0;x<mw;x++) {
            mv[(y*mw+x)*2]=half(float(1+(x+y)%7)/8);
            mv[(y*mw+x)*2+1]=half(-float(1+(2*x+y)%7)/8);
        }
        for(unsigned y=0;y<th;y++) for(unsigned x=0;x<tw;x++) for(unsigned c=0;c<4;c++)
            outPixels[(y*tw+x)*4+c]=static_cast<unsigned char>((x+5*y+29*c)%255);
        auto color=texture(w,h,DXGI_FORMAT_R8G8B8A8_TYPELESS,D3D11_BIND_SHADER_RESOURCE,pixels.data(),w*4);
        auto depth=texture(w,h,DXGI_FORMAT_R32G8X24_TYPELESS,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,packedDepth.data(),w*8);
        auto motion=texture(mw,mh,DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_SHADER_RESOURCE,mv.data(),mw*4);
        auto mask=texture(w,h,DXGI_FORMAT_R8_UNORM,D3D11_BIND_SHADER_RESOURCE,maskPixels.data(),w);
        // Native output intentionally lacks UAV and SRV binds; copy-back must still work.
        auto output=texture(tw,th,test%2?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_TYPELESS,
            D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,sentinel.data(),tw*4);
        DriverParams params;
        ID3D11Resource* originals[5]={color.Get(),depth.Get(),motion.Get(),test%3?mask.Get():nullptr,output.Get()};
        for(unsigned r=0;r<5;r++) params.Set(G::Names[r],originals[r]);
        params.Set(NVSDK_NGX_Parameter_MV_Scale_Y,float(mh)); params.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,.25f);
        params.Set(NVSDK_NGX_Parameter_MV_Scale_X,float(mw)); params.Set(NVSDK_NGX_Parameter_Jitter_Offset_X,-.125f);
        if(test%2) params.Set(NVSDK_NGX_Parameter_Reset,1); else params.Set(NVSDK_NGX_Parameter_Reset,0u);
        params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,w);
        params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,h);
        check(guides.Prepare(device.Get(),context.Get(),&params,w,h,tw,th));
        auto* cached=guides.Converted(G::Motion);
        check(guides.Prepare(device.Get(),context.Get(),&params,w,h,tw,th));
        require(cached==guides.Converted(G::Motion),"cache identity changed");
        D3D11_TEXTURE2D_DESC outputDesc {},captureDesc {};
        output->GetDesc(&outputDesc); guides.Converted(G::Output)->GetDesc(&captureDesc);
        require(outputDesc.Format==captureDesc.Format,"NGX output format changed");
        auto restored=[&] {
            for(unsigned r=0;r<5;r++) require(rawResource(params,G::Names[r])==originals[r],"resource not restored");
            float f=0; unsigned reset=9;
            params.Get(NVSDK_NGX_Parameter_MV_Scale_Y,&f); require(f==float(mh),"MV Y not restored");
            params.Get(NVSDK_NGX_Parameter_Jitter_Offset_Y,&f); require(f==.25f,"jitter Y not restored");
            params.Get(NVSDK_NGX_Parameter_MV_Scale_X,&f); require(f==float(mw),"MV X changed");
            params.Get(NVSDK_NGX_Parameter_Jitter_Offset_X,&f); require(f==-.125f,"jitter X changed");
            if(test%2) { int signedValue=0; require(params.Get(NVSDK_NGX_Parameter_Reset,&signedValue)==NVSDK_NGX_Result_Success && signedValue==1,"signed reset not restored"); }
            else { params.Get(NVSDK_NGX_Parameter_Reset,&reset); require(reset==0,"reset not restored"); }
            checkCs();
            for(unsigned r=0;r<5;r++) require(guides.Original(static_cast<G::Role>(r))==nullptr,"game resource retained across frame");
        };
        auto captured=[&] {
            for(unsigned r=0;r<5;r++) if(originals[r]) require(rawResource(params,G::Names[r])==guides.Converted(static_cast<G::Role>(r)),"capture missed clone");
            float f=0; unsigned reset=0;
            params.Get(NVSDK_NGX_Parameter_MV_Scale_Y,&f); require(f==-float(mh),"MV Y not negated");
            params.Get(NVSDK_NGX_Parameter_Jitter_Offset_Y,&f); require(f==-.25f,"jitter Y not negated");
            if(test%2) { int signedValue=0; require(params.Get(NVSDK_NGX_Parameter_Reset,&signedValue)==NVSDK_NGX_Result_Success && signedValue==1,"signed history reset"); }
            else { params.Get(NVSDK_NGX_Parameter_Reset,&reset); require(reset==1,"history not reset"); }
            checkCs();
            auto col=read(guides.Converted(G::Color),4),mov=read(guides.Converted(G::Motion),4),dep=read(guides.Converted(G::Depth),4);
            for(unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++) {
                require(!std::memcmp(col.data()+(y*w+x)*4,pixels.data()+((h-1-y)*w+x)*4,4),"color row/alpha mismatch");
                float actual; std::memcpy(&actual,dep.data()+(y*w+x)*4,4);
                unsigned bits=static_cast<unsigned>(packedDepth[(h-1-y)*w+x]); float expected; std::memcpy(&expected,&bits,4);
                require(std::abs(actual-expected)<.000001f,"packed depth plane/row mismatch");
            }
            for(unsigned y=0;y<mh;y++) for(unsigned x=0;x<mw;x++)
                require(!std::memcmp(mov.data()+(y*mw+x)*4,mv.data()+((mh-1-y)*mw+x)*2,4),"motion row/component mismatch");
            if(originals[G::Mask]) {
                auto mk=read(guides.Converted(G::Mask),1);
                for(unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++)
                    require(mk[y*w+x]==maskPixels[(h-1-y)*w+x],"reactive mask row mismatch");
            }
        };
        // Prove success copy-back reverses rows, and failure never overwrites the game output.
        auto result=guides.Evaluate(context.Get(),&params,true,[&] {
            captured(); auto target=asTexture(rawResource(params,NVSDK_NGX_Parameter_Output));
            context->UpdateSubresource(target.Get(),0,nullptr,outPixels.data(),tw*4,0);
            return NVSDK_NGX_Result_Success;
        });
        require(result==NVSDK_NGX_Result_Success,"evaluate failed"); restored();
        auto actual=read(output.Get(),4);
        for(unsigned y=0;y<th;y++) for(unsigned x=0;x<tw;x++)
            require(!std::memcmp(actual.data()+(y*tw+x)*4,outPixels.data()+((th-1-y)*tw+x)*4,4),"output orientation/alpha mismatch");
        auto before=actual;
        check(guides.Prepare(device.Get(),context.Get(),&params,w,h,tw,th));
        result=guides.Evaluate(context.Get(),&params,true,[&] { captured(); return NVSDK_NGX_Result_Fail; });
        require(result==NVSDK_NGX_Result_Fail,"failure status lost"); restored(); require(read(output.Get(),4)==before,"failure copied output");
        check(guides.Prepare(device.Get(),context.Get(),&params,w,h,tw,th));
        bool threw=false;
        try { guides.Evaluate(context.Get(),&params,true,[&]() -> NVSDK_NGX_Result { throw std::runtime_error("injected callback exception"); }); }
        catch(const std::runtime_error&) { threw=true; }
        require(threw,"exception not propagated"); restored();
        params.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,1u);
        require(FAILED(guides.Prepare(device.Get(),context.Get(),&params,w,h,tw,th)),"subrect accepted"); restored();
        params.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,0u);
        params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,w-1);
        require(FAILED(guides.Prepare(device.Get(),context.Get(),&params,w,h,tw,th)),"cropped input accepted");
        params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,w);
        params.values.erase(NVSDK_NGX_Parameter_MV_Scale_Y);
        require(FAILED(guides.Prepare(device.Get(),context.Get(),&params,w,h,tw,th)),"missing metadata accepted");
    }
    // Regression: a live converter must not pin a real swapchain buffer across resize.
    context->ClearState(); context->Flush();
    HWND window=CreateWindowExW(0,L"STATIC",L"Native guide resize test",WS_OVERLAPPEDWINDOW,
        0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    require(window!=nullptr,"hidden test window");
    ComPtr<IDXGIDevice> dxgiDevice; check(device.As(&dxgiDevice));
    ComPtr<IDXGIAdapter> adapter; check(dxgiDevice->GetAdapter(&adapter));
    ComPtr<IDXGIFactory> factory; check(adapter->GetParent(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC sd {}; sd.BufferDesc.Width=38; sd.BufferDesc.Height=22;
    sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count=1;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount=1; sd.OutputWindow=window;
    sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain> swapchain; check(factory->CreateSwapChain(device.Get(),&sd,&swapchain));
    for(unsigned iteration=0;iteration<4;iteration++) {
        unsigned tw=38+iteration*2,th=22+iteration*2,w=tw/2,h=th/2;
        if(iteration) check(swapchain->ResizeBuffers(1,tw,th,DXGI_FORMAT_R8G8B8A8_UNORM,0));
        ComPtr<ID3D11Texture2D> backbuffer; check(swapchain->GetBuffer(0,IID_PPV_ARGS(&backbuffer)));
        auto color=texture(w,h,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE,nullptr,0);
        auto depth=texture(w,h,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE,nullptr,0);
        auto motion=texture(w,h,DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_SHADER_RESOURCE,nullptr,0);
        DriverParams params;
        params.Set(G::Names[G::Color],static_cast<ID3D11Resource*>(color.Get()));
        params.Set(G::Names[G::Depth],static_cast<ID3D11Resource*>(depth.Get()));
        params.Set(G::Names[G::Motion],static_cast<ID3D11Resource*>(motion.Get()));
        params.Set(G::Names[G::Mask],static_cast<ID3D11Resource*>(nullptr));
        params.Set(G::Names[G::Output],static_cast<ID3D11Resource*>(backbuffer.Get()));
        params.Set(NVSDK_NGX_Parameter_MV_Scale_Y,float(h));
        params.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,0.f); params.Set(NVSDK_NGX_Parameter_Reset,0u);
        check(guides.Prepare(device.Get(),context.Get(),&params,w,h,tw,th));
        try { guides.Evaluate(context.Get(),&params,true,[&] {
            if(iteration==1) return NVSDK_NGX_Result_Fail;
            if(iteration==2) throw std::runtime_error("resize test callback");
            return NVSDK_NGX_Result_Success;
        }); } catch(const std::runtime_error&) { require(iteration==2,"unexpected callback exception"); }
        backbuffer.Reset();
        for(unsigned r=0;r<5;r++) require(guides.Original(static_cast<G::Role>(r))==nullptr,"swapchain buffer pinned");
        context->ClearState(); context->Flush();
    }
    swapchain.Reset(); DestroyWindow(window);

    context->ClearState(); context->Flush();
    unsigned errors=0;
    for(UINT64 i=0;i<info->GetNumStoredMessages();i++) {
        SIZE_T size=0; info->GetMessage(i,nullptr,&size); std::vector<char> storage(size);
        auto* msg=reinterpret_cast<D3D11_MESSAGE*>(storage.data()); check(info->GetMessage(i,msg,&size));
        if(msg->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) { std::cerr<<msg->pDescription<<'\n'; ++errors; }
    }
    require(errors==0,"D3D11 debug warnings/errors");
    std::cout<<"PASS: native DX11 captured rows, packed depth plane, FP16 signed motion, alpha/mask, low/full-res MV, SRGB/typeless output format and raw copy-back without UAV binds, resize/cache, success/failure/exception parameter restoration, input guards, signed/unsigned history reset, CS shader/SRV/UAV/CB restoration, no borrowed game resources retained after success/failure/exception, live DXGI swapchain ResizeBuffers cycles; zero debug warnings/errors\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
