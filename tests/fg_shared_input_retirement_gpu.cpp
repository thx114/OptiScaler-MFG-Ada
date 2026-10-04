#define NOMINMAX
#include "../OptiScaler/framegen/Dx12InputReadRetirement.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <array>
using Microsoft::WRL::ComPtr;
void check(HRESULT h){if(FAILED(h))throw std::runtime_error("HRESULT "+std::to_string(static_cast<unsigned>(h)));}
void require(bool b,const char* m){if(!b)throw std::runtime_error(m);}
int main(){try{
 ComPtr<ID3D12Debug> debug;check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
 ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
 ComPtr<IDXGIAdapter> adapter;check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
 ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> context;
 check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&context));
 ComPtr<ID3D11Device5> device11;ComPtr<ID3D11DeviceContext4> context11;check(d11.As(&device11));check(context.As(&context11));
 ComPtr<ID3D12Device> d12;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d12)));
 ComPtr<ID3D12InfoQueue> debug12;check(d12.As(&debug12));ComPtr<ID3D11InfoQueue> debug11;check(d11.As(&debug11));
 ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};check(d12->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
 ComPtr<ID3D12Fence> readFence;check(d12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&readFence)));
 ComPtr<ID3D11Fence> written11;check(device11->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&written11)));
 HANDLE sharedFence=nullptr;check(written11->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&sharedFence));
 ComPtr<ID3D12Fence> written12;check(d12->OpenSharedHandle(sharedFence,IID_PPV_ARGS(&written12)));CloseHandle(sharedFence);
 HANDLE done=CreateEvent(nullptr,FALSE,FALSE,nullptr);require(done!=nullptr,"event");
 D3D11_TEXTURE2D_DESC td{};td.Width=19;td.Height=11;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R32_FLOAT;
 td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
 ComPtr<ID3D11Texture2D> shared11;check(d11->CreateTexture2D(&td,nullptr,&shared11));
 ComPtr<IDXGIResource1> dxgi;check(shared11.As(&dxgi));HANDLE textureHandle=nullptr;
 check(dxgi->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&textureHandle));
 ComPtr<ID3D12Resource> shared12;check(d12->OpenSharedHandle(textureHandle,IID_PPV_ARGS(&shared12)));CloseHandle(textureHandle);
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;auto d=shared12->GetDesc();d12->GetCopyableFootprints(&d,0,1,0,&footprint,nullptr,nullptr,&bytes);
 struct Slot{ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Resource> readback;bool pending=false;UINT64 fence=0;};
 std::array<Slot,4> slots;
 for(auto& slot:slots){check(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot.allocator)));
  check(d12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot.allocator.Get(),nullptr,IID_PPV_ARGS(&slot.list)));check(slot.list->Close());
  D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=bytes;rd.Height=rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;check(d12->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&slot.readback)));}
 UINT64 readValue=0,writeValue=0;std::vector<unsigned> order;
 auto submit=[&](std::size_t i){order.push_back(unsigned(i));auto& s=slots[i];if(!s.pending)return true;
  check(s.list->Close());ID3D12CommandList* lists[]={s.list.Get()};queue->ExecuteCommandLists(1,lists);s.fence=++readValue;check(queue->Signal(readFence.Get(),s.fence));s.pending=false;return true;};
 auto wait=[&](std::size_t i){order.push_back(unsigned(i)+4);auto& s=slots[i];if(!s.fence)return true;
  require(readFence->GetCompletedValue()!=UINT64_MAX,"removed device must not be treated as complete");
  if(readFence->GetCompletedValue()<s.fence){check(readFence->SetEventOnCompletion(s.fence,done));require(WaitForSingleObject(done,5000)==WAIT_OBJECT_0,"input GPU read timeout");}return true;};
 auto record=[&](unsigned i){auto& s=slots[i];check(s.allocator->Reset());check(s.list->Reset(s.allocator.Get(),nullptr));
  D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={shared12.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};s.list->ResourceBarrier(1,&b);
  D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=s.readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
  D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=shared12.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;s.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COMMON;s.list->ResourceBarrier(1,&b);s.pending=true;};
 auto write=[&](float value){std::vector<float> pixels(19*11,value);context11->UpdateSubresource(shared11.Get(),0,nullptr,pixels.data(),19*4,0);
  check(context11->Signal(written11.Get(),++writeValue));context11->Flush();check(queue->Wait(written12.Get(),writeValue));};
 auto inspect=[&](unsigned slot,float expected){void* mapped=nullptr;check(slots[slot].readback->Map(0,nullptr,&mapped));
  for(unsigned y=0;y<11;y++)for(unsigned x=0;x<19;x++)require(reinterpret_cast<float*>(static_cast<char*>(mapped)+y*footprint.Footprint.RowPitch)[x]==expected,"shared input overwritten before D3D12 reader retired");slots[slot].readback->Unmap(0,nullptr);};
 // Reproduce old one-way protocol: recorded D3D12 read sees overwritten input.
 write(1.f);record(0);write(2.f);submit(0);wait(0);inspect(0,2.f);
 std::cout<<"CONTROL: D3D11->D3D12-only synchronization lets pending GPU reads consume the next input\n";
 for(unsigned frame=0;frame<16;frame++){
  require(RetireDx12InputReaders(4,submit,wait),"retirement rejected");
  auto value=float(frame+10);write(value);
  // Pending reads in non-adjacent slots: all must submit before any reserved fence wait.
  record(frame%4);record((frame+2)%4);
  order.clear();require(RetireDx12InputReaders(4,submit,wait),"retirement failed");
  require(order==std::vector<unsigned>({0,1,2,3,4,5,6,7}),"submission/wait order incorrect");
  write(value+1000);inspect(frame%4,value);inspect((frame+2)%4,value);
 }
 unsigned waits=0;require(!RetireDx12InputReaders(4,[](std::size_t i){return i!=2;},[&](std::size_t){waits++;return true;}),"submit failure ignored");require(!waits,"wait after failed submit");
 require(!RetireDx12InputReaders(4,[](std::size_t){return true;},[](std::size_t i){return i!=1;}),"wait failure ignored");
 unsigned errors=0;
 for(UINT64 i=0;i<debug12->GetNumStoredMessages();i++){SIZE_T size=0;debug12->GetMessage(i,nullptr,&size);std::vector<char> data(size);auto* msg=reinterpret_cast<D3D12_MESSAGE*>(data.data());check(debug12->GetMessage(i,msg,&size));if(msg->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){std::cerr<<msg->pDescription<<'\n';errors++;}}
 for(UINT64 i=0;i<debug11->GetNumStoredMessages();i++){SIZE_T size=0;debug11->GetMessage(i,nullptr,&size);std::vector<char> data(size);auto* msg=reinterpret_cast<D3D11_MESSAGE*>(data.data());check(debug11->GetMessage(i,msg,&size));if(msg->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::cerr<<msg->pDescription<<'\n';errors++;}}
 CloseHandle(done);require(!errors,"GPU debug warning/error");std::cout<<"PASS: actual D3D11/D3D12 shared input, all-slot submit-before-wait retirement, 16 alternating producers, failure guards; zero D3D11/D3D12 debug warnings/errors\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
