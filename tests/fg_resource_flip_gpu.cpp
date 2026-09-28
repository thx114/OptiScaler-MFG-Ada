// Execute the production flip HLSL on WARP, including non-multiple-of-16 extents.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <iostream>
#include <cmath>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
void check(HRESULT h){if(FAILED(h))throw std::runtime_error("D3D11 failure");}
int main(){try{
 ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;
 check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));
 ComPtr<ID3DBlob> code,errors;check(D3DCompileFromFile(L"OptiScaler/shaders/resource_flip/precompiled/RF.hlsl",nullptr,nullptr,"CSMain","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors));
 ComPtr<ID3D11ComputeShader> shader;check(d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));c->CSSetShader(shader.Get(),nullptr,0);
 struct Pixel{float r,g,b,a;};
 for(UINT test=0;test<4;test++){
  UINT w=17+test,h=7+test,offset=(test&1)?3:0,velocity=test>=2;
  std::vector<Pixel> input(32*16),output(32*16,{-7,-7,-7,-7});
  for(UINT y=0;y<16;y++)for(UINT x=0;x<32;x++)input[y*32+x]={float(1+x+y)/100,.25f,.5f,1};
  auto texture=[&](UINT bind,const std::vector<Pixel>& values){
   D3D11_TEXTURE2D_DESC desc {};desc.Width=32;desc.Height=16;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.SampleDesc.Count=1;desc.BindFlags=bind;
   D3D11_SUBRESOURCE_DATA data {values.data(),32*sizeof(Pixel),0};ComPtr<ID3D11Texture2D> r;check(d->CreateTexture2D(&desc,&data,&r));return r;};
  auto source=texture(D3D11_BIND_SHADER_RESOURCE,input),dest=texture(D3D11_BIND_UNORDERED_ACCESS,output);
  ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;
  check(d->CreateShaderResourceView(source.Get(),nullptr,&srv));check(d->CreateUnorderedAccessView(dest.Get(),nullptr,&uav));
  auto* sp=srv.Get();auto* up=uav.Get();c->CSSetShaderResources(0,1,&sp);c->CSSetUnorderedAccessViews(0,1,&up,nullptr);
  UINT params[4]={w-1,h-1,offset,velocity};D3D11_BUFFER_DESC bd {};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA data {params,0,0};ComPtr<ID3D11Buffer> constants;check(d->CreateBuffer(&bd,&data,&constants));auto* cb=constants.Get();c->CSSetConstantBuffers(0,1,&cb);
  c->Dispatch((w+15)/16,(h+15)/16,1);up=nullptr;sp=nullptr;c->CSSetUnorderedAccessViews(0,1,&up,nullptr);c->CSSetShaderResources(0,1,&sp);
  D3D11_TEXTURE2D_DESC desc;dest->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> readback;check(d->CreateTexture2D(&desc,nullptr,&readback));c->CopyResource(readback.Get(),dest.Get());
  D3D11_MAPPED_SUBRESOURCE mapped;check(c->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
  for(UINT y=0;y<16;y++)for(UINT x=0;x<32;x++){
   auto p=((Pixel*)((char*)mapped.pData+y*mapped.RowPitch))[x];bool valid=x<w&&y<h;
   float expected=valid?float(1+x+h-1-y+offset)/100:-7;
   if(std::abs(p.r-expected)>.00001f||std::abs(p.g-(valid?(velocity?-.25f:.25f):-7.f))>.00001f)
    throw std::runtime_error("flip/offset/padding/sign GPU mismatch");
  }c->Unmap(readback.Get(),0);
 }
 std::cout<<"PASS: production flip shader depth, MV sign, offset crop, odd extents and untouched padding\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
