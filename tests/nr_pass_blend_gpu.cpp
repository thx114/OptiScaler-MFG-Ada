// Run the production SM5 shader on WARP; verifies same-size mixing and padded chain rasters.
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <cstring>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D11 operation failed"); }
struct Pixel { float r,g,b,a; };
int main(int argc, char** argv)
{
    try {
    if (argc != 2) throw std::runtime_error("Pass the compiled SM5 shader path");
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<char> blob((std::istreambuf_iterator<char>(file)), {});
    if (blob.empty()) throw std::runtime_error("Missing shader");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<ID3D11ComputeShader> shader;
    check(device->CreateComputeShader(blob.data(),blob.size(),nullptr,&shader));
    context->CSSetShader(shader.Get(),nullptr,0);
    D3D11_SAMPLER_DESC sd {}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler; check(device->CreateSamplerState(&sd,&sampler));
    auto* sp=sampler.Get(); context->CSSetSamplers(0,1,&sp);
    auto run = [&](unsigned sourceContent, unsigned targetSize, unsigned baseContent, float weight) {
        const unsigned allocation=8;
        auto texture = [&](float value, unsigned content, UINT bind) {
            std::vector<Pixel> pixels(allocation*allocation,{9,9,9,1}); // poisonous padding
            for (unsigned y=0;y<content;++y) for(unsigned x=0;x<content;++x)
                pixels[y*allocation+x]={value,value,value,1};
            D3D11_TEXTURE2D_DESC d {}; d.Width=d.Height=allocation; d.MipLevels=d.ArraySize=1;
            d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; d.SampleDesc.Count=1; d.BindFlags=bind;
            D3D11_SUBRESOURCE_DATA initial {pixels.data(),allocation*sizeof(Pixel),0};
            ComPtr<ID3D11Texture2D> result; check(device->CreateTexture2D(&d,&initial,&result)); return result;
        };
        auto model=texture(.8f,sourceContent,D3D11_BIND_SHADER_RESOURCE);
        auto input=texture(.2f,baseContent,D3D11_BIND_SHADER_RESOURCE);
        auto output=texture(0,allocation,D3D11_BIND_UNORDERED_ACCESS);
        ComPtr<ID3D11ShaderResourceView> modelView,inputView;
        ComPtr<ID3D11UnorderedAccessView> outputView;
        check(device->CreateShaderResourceView(model.Get(),nullptr,&modelView));
        check(device->CreateShaderResourceView(input.Get(),nullptr,&inputView));
        check(device->CreateUnorderedAccessView(output.Get(),nullptr,&outputView));
        ID3D11ShaderResourceView* views[]={modelView.Get(),nullptr,inputView.Get()};
        context->CSSetShaderResources(0,3,views);
        auto* uav=outputView.Get(); context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        std::array<unsigned,64> constants {};
        constants[0]=8; constants[2]=constants[3]=targetSize;
        float inputWeight=1.f-weight; std::memcpy(&constants[33],&inputWeight,4);
        constants[34]=constants[35]=sourceContent;
        constants[36]=constants[37]=baseContent;
        D3D11_BUFFER_DESC bd {};bd.ByteWidth=256;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA cbData {constants.data(),0,0};
        ComPtr<ID3D11Buffer> cb;check(device->CreateBuffer(&bd,&cbData,&cb));
        auto* cbp=cb.Get();context->CSSetConstantBuffers(0,1,&cbp);
        context->Dispatch(1,1,1);
        ID3D11UnorderedAccessView* noUav=nullptr;context->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);
        ID3D11ShaderResourceView* noViews[3]={};context->CSSetShaderResources(0,3,noViews);
        D3D11_TEXTURE2D_DESC rd {};output->GetDesc(&rd);rd.BindFlags=0;rd.Usage=D3D11_USAGE_STAGING;rd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&rd,nullptr,&staging));
        context->CopyResource(staging.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        bool valid=true;
        const float expected=.8f*weight+.2f*(1.f-weight);
        for(unsigned y=0;y<targetSize;++y) for(unsigned x=0;x<targetSize;++x) {
            auto* row=reinterpret_cast<Pixel*>(static_cast<char*>(mapped.pData)+y*mapped.RowPitch);
            valid &= std::abs(row[x].r-expected)<1e-5f && std::abs(row[x].g-expected)<1e-5f && std::abs(row[x].b-expected)<1e-5f;
        }
        context->Unmap(staging.Get(),0);
        if(!valid) throw std::runtime_error("Blend/region mismatch");
    };
    for(float weight: {0.f,.25f,.5f,1.f}) {
        run(4,4,4,weight); // equal-size layers inside larger allocation
        run(2,6,3,weight); // upsample with poisoned source and input padding
        run(6,2,6,weight); // downsample
    }
    std::cout << "PASS production shader: 12 WARP blend/extent cases including same-size and padded edges\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}

