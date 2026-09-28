#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include "../OptiScaler/shaders/depth_transfer/DepthTransferBindings.h"
using Microsoft::WRL::ComPtr;
void check(HRESULT h) {if(FAILED(h)) throw std::runtime_error("D3D failure");}
void require(bool b) {if(!b) throw std::runtime_error("assertion failed");}
int main() {try {
 ComPtr<ID3D11Device> d; ComPtr<ID3D11DeviceContext> c;
 check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));
 ComPtr<ID3D11InfoQueue> info;check(d.As(&info));
 const char* code="Texture2D<float> src:register(t0); RWTexture2D<float> dst:register(u0); [numthreads(16,16,1)] void main(uint3 p:SV_DispatchThreadID){dst[p.xy]=src.Load(int3(p.xy,0));}";
 ComPtr<ID3DBlob> blob,err;check(D3DCompile(code,strlen(code),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&blob,&err));
 ComPtr<ID3D11ComputeShader> shader;check(d->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&shader));
 D3D11_TEXTURE2D_DESC td {};td.Width=td.Height=16;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=1;
 td.Format=DXGI_FORMAT_R24G8_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
 ComPtr<ID3D11Texture2D> depth,out,readback;check(d->CreateTexture2D(&td,nullptr,&depth));
 D3D11_DEPTH_STENCIL_VIEW_DESC dd {};dd.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
 ComPtr<ID3D11DepthStencilView> dsv;check(d->CreateDepthStencilView(depth.Get(),&dd,&dsv));
 D3D11_SHADER_RESOURCE_VIEW_DESC sd {};sd.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
 ComPtr<ID3D11ShaderResourceView> srv;check(d->CreateShaderResourceView(depth.Get(),&sd,&srv));
 td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;check(d->CreateTexture2D(&td,nullptr,&out));
 ComPtr<ID3D11UnorderedAccessView> uav;check(d->CreateUnorderedAccessView(out.Get(),nullptr,&uav));
 td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(d->CreateTexture2D(&td,nullptr,&readback));
 c->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,.25f,0);
 c->OMSetRenderTargets(0,nullptr,dsv.Get());
 auto* s=srv.Get();auto* u=uav.Get();
 auto dispatch=[&](){c->CSSetShader(shader.Get(),nullptr,0);c->CSSetShaderResources(0,1,&s);c->CSSetUnorderedAccessViews(0,1,&u,nullptr);c->Dispatch(1,1,1);};
 auto read=[&](float expected){c->CopyResource(readback.Get(),out.Get());D3D11_MAPPED_SUBRESOURCE m {};check(c->Map(readback.Get(),0,D3D11_MAP_READ,0,&m));
  for(UINT y=0;y<16;y++)for(UINT x=0;x<16;x++)require(std::abs(((float*)((char*)m.pData+y*m.RowPitch))[x]-expected)<1e-6f);c->Unmap(readback.Get(),0);};
 dispatch();ComPtr<ID3D11ShaderResourceView> bound;c->CSGetShaderResources(0,1,&bound);require(!bound);read(0.f);
 // The baseline intentionally emits an SRV hazard. Only inspect guarded-pass messages below.
 info->ClearStoredMessages();c->ClearState();c->OMSetRenderTargets(0,nullptr,dsv.Get());
 {
  DepthTransferBindings guard(c.Get(),depth.Get());require(guard.DetachedDepth());dispatch();
  c->CSGetShaderResources(0,1,&bound);require(bound.Get()==srv.Get());
 }
 read(.25f);
 ComPtr<ID3D11DepthStencilView> restored;c->OMGetRenderTargets(0,nullptr,&restored);require(restored.Get()==dsv.Get());
 bound.Reset();c->CSGetShaderResources(0,1,&bound);require(!bound);
 ComPtr<ID3D11ComputeShader> restoredShader;c->CSGetShader(&restoredShader,nullptr,nullptr);require(!restoredShader);
 ComPtr<ID3D11UnorderedAccessView> restoredUav;c->CSGetUnorderedAccessViews(0,1,&restoredUav);require(!restoredUav);
 // Non-null CS/OM UAV restoration, with no RTVs and the OM UAV in slot 1.
 ComPtr<ID3D11Texture2D> other;td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
 check(d->CreateTexture2D(&td,nullptr,&other));ComPtr<ID3D11UnorderedAccessView> omUav;check(d->CreateUnorderedAccessView(other.Get(),nullptr,&omUav));
 auto* om=omUav.Get();c->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,dsv.Get(),1,1,&om,nullptr);
 c->CSSetShader(shader.Get(),nullptr,0);c->CSSetUnorderedAccessViews(0,1,&u,nullptr);
 {DepthTransferBindings guard(c.Get(),depth.Get());require(guard.DetachedDepth());dispatch();}
 restoredUav.Reset();c->CSGetUnorderedAccessViews(0,1,&restoredUav);require(restoredUav.Get()==uav.Get());
 ComPtr<ID3D11UnorderedAccessView> restoredOm;c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,1,1,&restoredOm);require(restoredOm.Get()==omUav.Get());
 restoredShader.Reset();c->CSGetShader(&restoredShader,nullptr,nullptr);require(restoredShader.Get()==shader.Get());read(.25f);
 for(UINT64 i=0;i<info->GetNumStoredMessages();i++){SIZE_T n=0;check(info->GetMessage(i,nullptr,&n));std::vector<char> bytes(n);auto* m=(D3D11_MESSAGE*)bytes.data();check(info->GetMessage(i,m,&n));if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::cerr<<m->pDescription<<'\n';require(false);}}
 std::cout<<"PASS: old bound-DSV path reads zero; production binding guard reads 0.25 and restores CS/DSV/OM UAV, no debug-layer warnings\n";
 return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
