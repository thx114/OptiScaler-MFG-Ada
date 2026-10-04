#define NOMINMAX
#include "../OptiScaler/menu/MenuGpuLifetime_Dx12.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include <vector>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstring>
using Microsoft::WRL::ComPtr;
void check(HRESULT h){if(FAILED(h))throw std::runtime_error("HRESULT "+std::to_string(static_cast<unsigned>(h)));}
void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
int main(){try{
 ComPtr<ID3D12Debug> debug;check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
 ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
 ComPtr<IDXGIAdapter> warp;check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
 ComPtr<ID3D12Device> device;check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
 ComPtr<ID3D12InfoQueue> info;check(device.As(&info));
 D3D12_COMMAND_QUEUE_DESC qd {};ComPtr<ID3D12CommandQueue> queue;check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
 MenuGpuLifetime_Dx12 lifetime;check(lifetime.Initialize(device.Get(),queue.Get()));
 ComPtr<ID3D12Fence> gate;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
 check(queue->Wait(gate.Get(),1));check(lifetime.SignalSubmitted(3));
 require(lifetime.WaitSlot(3,20)==HRESULT_FROM_WIN32(WAIT_TIMEOUT),"in-flight allocator reused");
 require(lifetime.WaitIdle(20)==HRESULT_FROM_WIN32(WAIT_TIMEOUT),"in-flight resize cleanup allowed");
 check(gate->Signal(1));check(lifetime.WaitSlot(3));check(lifetime.Reset());
 check(lifetime.Initialize(device.Get(),queue.Get()));require(lifetime.WaitSlot(8)==E_INVALIDARG,"slot guard");
 ComPtr<ID3D12CommandQueue> replacementQueue;check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&replacementQueue)));
 check(lifetime.SignalSubmitted(2));check(lifetime.Initialize(device.Get(),replacementQueue.Get()));
 check(lifetime.SignalSubmitted(5));check(lifetime.WaitSlot(5));check(lifetime.Reset());
 check(lifetime.Initialize(device.Get(),queue.Get()));
 auto* context=ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
 ComPtr<ID3D12DescriptorHeap> srvHeap;D3D12_DESCRIPTOR_HEAP_DESC hd {};
 hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=64;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
 check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srvHeap)));
 DescriptorHeapAllocator descriptors;descriptors.Create(device.Get(),srvHeap.Get());
 ImGui_ImplDX12_InitInfo init {};init.Device=device.Get();init.CommandQueue=queue.Get();
 init.NumFramesInFlight=8;init.RTVFormat=DXGI_FORMAT_R8G8B8A8_UNORM;init.SrvDescriptorHeap=srvHeap.Get();
 init.UserData=&descriptors;
 init.SrvDescriptorAllocFn=[](ImGui_ImplDX12_InitInfo* i,D3D12_CPU_DESCRIPTOR_HANDLE* c,D3D12_GPU_DESCRIPTOR_HANDLE* g){static_cast<DescriptorHeapAllocator*>(i->UserData)->Alloc(c,g);};
 init.SrvDescriptorFreeFn=[](ImGui_ImplDX12_InitInfo* i,D3D12_CPU_DESCRIPTOR_HANDLE c,D3D12_GPU_DESCRIPTOR_HANDLE g){static_cast<DescriptorHeapAllocator*>(i->UserData)->Free(c,g);};
 require(ImGui_ImplDX12_Init(&init),"backend init");
 // Drive the production backend's dynamic font upload through menu reopen/re-init.
 for(unsigned cycle=0;cycle<4;cycle++){

  io.DisplaySize=ImVec2(float(640+cycle*57),float(360+cycle*31));io.DeltaTime=1.f/60;
  ImGui_ImplDX12_NewFrame();ImGui::NewFrame();ImGui::Begin("Menu after resize");
  ImGui::Text("Resolution cycle %u: dynamic font atlas upload",cycle);ImGui::End();ImGui::Render();
  auto* draw=ImGui::GetDrawData();require(draw && draw->Textures,"font texture list");
  for(auto* tex:*draw->Textures){if(tex->Status!=ImTextureStatus_OK)ImGui_ImplDX12_UpdateTexture(tex);
   require(tex->Status==ImTextureStatus_OK,"texture upload not completed");}
  // Render the actual menu draw data to resized offscreen targets, not only font uploads.
  D3D12_RESOURCE_DESC targetDesc {};targetDesc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  targetDesc.Width=640+cycle*57;targetDesc.Height=360+cycle*31;targetDesc.DepthOrArraySize=targetDesc.MipLevels=1;
  targetDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;targetDesc.SampleDesc.Count=1;
  targetDesc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_HEAP_PROPERTIES properties {};properties.Type=D3D12_HEAP_TYPE_DEFAULT;
  ComPtr<ID3D12Resource> target;check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&targetDesc,
      D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)));
  D3D12_DESCRIPTOR_HEAP_DESC rtvDesc {};rtvDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rtvDesc.NumDescriptors=1;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;check(device->CreateDescriptorHeap(&rtvDesc,IID_PPV_ARGS(&rtvHeap)));
  auto rtv=rtvHeap->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(target.Get(),nullptr,rtv);
  ComPtr<ID3D12CommandAllocator> allocator;check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,
      allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
  auto* heap=srvHeap.Get();list->SetDescriptorHeaps(1,&heap);list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
  ImGui_ImplDX12_RenderDrawData(draw,list.Get());check(list->Close());
  ID3D12CommandList* commands[]={list.Get()};queue->ExecuteCommandLists(1,commands);
  check(lifetime.SignalSubmitted(cycle));check(lifetime.WaitSlot(cycle));check(lifetime.WaitIdle());
  ImGui_ImplDX12_Shutdown(false);require(io.BackendRendererUserData==nullptr,"old backend retained");
  require(ImGui_ImplDX12_Init(&init),"backend re-init");
 }
 unsigned debugErrors=0;
 for(UINT64 i=0;i<info->GetNumStoredMessages();i++){
  SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<char> storage(size);
  auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());check(info->GetMessage(i,message,&size));
  if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){std::cerr<<message->pDescription<<'\n';++debugErrors;}
 }
 require(debugErrors==0,"valid menu upload/reopen caused debug warnings/errors");
 unsigned before=descriptors.FreeIndices.Size;
 // Bad allocation must release its descriptor and never call Map on a null resource.
 ImTextureData rejected;rejected.Status=ImTextureStatus_WantCreate;rejected.TexID=ImTextureID_Invalid;
 rejected.Format=ImTextureFormat_RGBA32;rejected.Width=2147483647;rejected.Height=1;rejected.BytesPerPixel=4;ImGui_ImplDX12_UpdateTexture(&rejected);
 require(rejected.BackendUserData==nullptr && rejected.TexID==ImTextureID_Invalid,"failed texture published");
 require(descriptors.FreeIndices.Size==static_cast<int>(before),"failed allocation descriptor leaked");
 info->ClearStoredMessages(); // The intentionally invalid texture dimension above emits expected validation errors.
 ComPtr<ID3D12Device5> removable;check(device.As(&removable));removable->RemoveDevice();
 require(FAILED(device->GetDeviceRemovedReason()),"device removal control");
 ImTextureData afterRemoval;afterRemoval.Status=ImTextureStatus_WantCreate;afterRemoval.TexID=ImTextureID_Invalid;
 afterRemoval.Format=ImTextureFormat_RGBA32;afterRemoval.Width=8;afterRemoval.Height=8;afterRemoval.BytesPerPixel=4;
 ImGui_ImplDX12_UpdateTexture(&afterRemoval);
 require(afterRemoval.BackendUserData==nullptr,"removed-device upload allocated");
 ImGui_ImplDX12_Shutdown(false);ImGui::DestroyContext(context);descriptors.Destroy();
 std::cout<<"PASS: menu GPU fence blocks allocator/resize reuse until completion; actual ImGui DX12 font uploads/drawing to resized targets/reopen/re-init, failed allocation cleanup and removed-device upload skip; zero debug warnings/errors on valid operations\n";
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
