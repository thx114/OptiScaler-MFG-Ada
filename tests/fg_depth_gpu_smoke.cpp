// WARP GPU readbacks of production depth renderer, four outstanding command lists.
#define NOMINMAX
#include <Windows.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <limits>
#include <algorithm>
#include "../OptiScaler/framegen/dlssg/FgDepthDebug.h"
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) {if(FAILED(hr)) {std::cerr<<std::hex<<hr<<'\n';throw std::runtime_error("D3D12 failure");}}
void require(bool v) {if(!v)throw std::runtime_error("GPU readback/assertion failed");}
void transition(ID3D12GraphicsCommandList* c,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
 D3D12_RESOURCE_BARRIER t {};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
 t.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};c->ResourceBarrier(1,&t);
}
int main(int argc, char**) {try {
 const bool enhanced = argc > 1;
 const bool preserveMenu = argc > 2;
 auto input = [&](UINT i, UINT x, UINT y) {
  if (!enhanced) return float(1+i+x+y)/100;
  const float values[] = {0.f, 1e-8f, 1.f, -0.1f, std::numeric_limits<float>::quiet_NaN(), 0.001f};
  return values[(x+y+i)%6];
 };
 ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
 ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
 ComPtr<IDXGIAdapter> adapter;check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
 ComPtr<ID3D12Device> d;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
 ComPtr<ID3D12InfoQueue> info;d.As(&info);
 ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q {};check(d->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
 auto buffer=[&](UINT64 n,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp {};hp.Type=type;D3D12_RESOURCE_DESC rd {};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width=n;rd.Height=1;rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> r;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,state,nullptr,IID_PPV_ARGS(&r)));return r;
 };
 auto texture=[&](DXGI_FORMAT format,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp {};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC rd {};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width=32;rd.Height=16;rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Format=format;rd.Flags=flags;
  ComPtr<ID3D12Resource> r;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,state,nullptr,IID_PPV_ARGS(&r)));return r;
 };
 struct Slot {ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> cmd;
  ComPtr<ID3D12Resource> depth,target,upload,readback;D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};};
 std::array<Slot,4> slots;FgDepthDebug view;
 for(UINT i=0;i<4;i++) {
  auto& s=slots[i];check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator)));
  check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.cmd)));
  s.depth=texture(DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
  s.target=texture(DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
  auto desc=s.depth->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT f {};UINT64 bytes;
  d->GetCopyableFootprints(&desc,0,1,0,&f,nullptr,nullptr,&bytes);s.upload=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  void* p;check(s.upload->Map(0,nullptr,&p));
  for(UINT y=0;y<16;y++)for(UINT x=0;x<32;x++){float v=input(i,x,y);std::memcpy((char*)p+y*f.Footprint.RowPitch+x*4,&v,4);}
  s.upload->Unmap(0,nullptr);
  D3D12_TEXTURE_COPY_LOCATION src {};src.pResource=s.upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=f;
  D3D12_TEXTURE_COPY_LOCATION dst {};dst.pResource=s.depth.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  s.cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);transition(s.cmd.Get(),s.depth.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  require(!view.Draw(d.Get(),s.cmd.Get(),4,s.depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,s.target.Get(),32,16,0,0,1,false));
  require(!view.Draw(d.Get(),s.cmd.Get(),i,s.depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,s.target.Get(),33,16,0,0,1,false));
  if (preserveMenu) {
   ComPtr<ID3D12DescriptorHeap> rtvHeap;
   D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=1;
   check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvHeap)));
   d->CreateRenderTargetView(s.target.Get(),nullptr,rtvHeap->GetCPUDescriptorHandleForHeapStart());
   transition(s.cmd.Get(),s.target.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
   const float menuColor[]={0,1,0,1};
   s.cmd->ClearRenderTargetView(rtvHeap->GetCPUDescriptorHandleForHeapStart(),menuColor,0,nullptr);
   transition(s.cmd.Get(),s.target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
   // Shader only accesses descriptors at recording time here.
   DepthDebugMenuRegions::Snapshot regions; regions.updated=GetTickCount64();
   DepthDebugMenuRegions::Add(regions,{0,.0f,.25f,.5f});
   DepthDebugMenuRegions::Store(regions);
  }
  require(view.Draw(d.Get(),s.cmd.Get(),i,s.depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,s.target.Get(),17+i,7+i,2,3,i==3?2.f:1.f,i>=2,enhanced,preserveMenu));
  desc=s.target->GetDesc();d->GetCopyableFootprints(&desc,0,1,0,&s.footprint,nullptr,nullptr,&bytes);
  s.readback=buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  transition(s.cmd.Get(),s.target.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
  src={};src.pResource=s.target.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst={};dst.pResource=s.readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=s.footprint;
  s.cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);check(s.cmd->Close());
 }
 ID3D12CommandList* lists[4];for(UINT i=0;i<4;i++)lists[i]=slots[i].cmd.Get();queue->ExecuteCommandLists(4,lists);
 ComPtr<ID3D12Fence> fence;check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));check(queue->Signal(fence.Get(),1));
 HANDLE e=CreateEvent(nullptr,FALSE,FALSE,nullptr);check(fence->SetEventOnCompletion(1,e));require(WaitForSingleObject(e,10000)==WAIT_OBJECT_0);CloseHandle(e);
 for(UINT i=0;i<4;i++) {
  auto& s=slots[i];void* data;check(s.readback->Map(0,nullptr,&data));
  for(UINT y=0;y<16;y++)for(UINT x=0;x<32;x++) {
   UINT sx=UINT((x+.5f)*(17+i)/32)+2,sy=UINT((y+.5f)*(7+i)/16)+3;
   float raw=input(i,sx,sy),v=raw;if(i>=2)v=1-v;if(i==3)v*=2;v=std::min(1.f,v);
   if(enhanced) v=.12f+.88f*std::clamp((std::log2(std::max(v,1e-12f))+40)/40,0.f,1.f);
   float rgb[3]={v,v,v};
   if(enhanced) {
    if(!std::isfinite(raw)) {rgb[0]=1;rgb[1]=0;rgb[2]=1;}
    else if(raw==0) {rgb[0]=0;rgb[1]=.15f;rgb[2]=1;}
    else if(raw==1) {rgb[0]=1;rgb[1]=1;rgb[2]=0;}
    else if(raw<0||raw>1) {rgb[0]=1;rgb[1]=0;rgb[2]=0;}
   }
   if (preserveMenu && x<8 && y<8) {rgb[0]=0;rgb[1]=1;rgb[2]=0;}
   auto* p=(unsigned char*)data+y*s.footprint.Footprint.RowPitch+x*4;
   for(UINT c=0;c<3;c++) require(std::abs(int(p[c])-int(std::round(rgb[c]*255)))<=1);
   require(p[3]==255);
  }s.readback->Unmap(0,nullptr);
 }
 if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();i++) {
  SIZE_T n=0;check(info->GetMessage(i,nullptr,&n));std::vector<char> b(n);auto* m=(D3D12_MESSAGE*)b.data();check(info->GetMessage(i,m,&n));
  if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<m->pDescription<<'\n';require(false);}
 }
 std::cout<<"PASS: production depth debug GPU readback, four slots, crops, gain, inversion; debug layer="<<bool(info)<<'\n';return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
