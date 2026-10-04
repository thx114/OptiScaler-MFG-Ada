// Production DX11-to-FG resize routing on actual D3D11/D3D12 WARP chains.
#define NOMINMAX
#include "../OptiScaler/with_dx12/Dx11FgResize.h"
#include <d3d11.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <string>
using Microsoft::WRL::ComPtr;
void check(HRESULT h) { if(FAILED(h)) throw std::runtime_error("HRESULT "+std::to_string(static_cast<unsigned>(h))); }
void require(bool b,const char* s) {if(!b) throw std::runtime_error(s);}
int main(){try{
 ComPtr<ID3D12Debug> debug;check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
 ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
 ComPtr<IDXGIAdapter> warp;check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
 ComPtr<ID3D12Device> d12;check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d12)));
 ComPtr<ID3D12InfoQueue> info;check(d12.As(&info));
 ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> context;
 check(D3D11CreateDevice(warp.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&context));
 ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd {};check(d12->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
 HWND gameWindow=CreateWindowExW(0,L"STATIC",L"DX11 resize test",WS_OVERLAPPEDWINDOW,0,0,300,200,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 HWND fgWindow=CreateWindowExW(0,L"STATIC",L"DX12 FG resize test",WS_OVERLAPPEDWINDOW,0,0,300,200,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 require(gameWindow && fgWindow,"hidden windows");
 DXGI_SWAP_CHAIN_DESC gd {};gd.BufferDesc.Width=256;gd.BufferDesc.Height=144;
 gd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;gd.SampleDesc.Count=1;gd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
 gd.BufferCount=1;gd.Windowed=TRUE;gd.OutputWindow=gameWindow;gd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
 ComPtr<IDXGISwapChain> game;check(factory->CreateSwapChain(d11.Get(),&gd,&game));
 DXGI_SWAP_CHAIN_DESC1 fd {};fd.Width=256;fd.Height=144;fd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
 fd.SampleDesc.Count=1;fd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;fd.BufferCount=4;
 fd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;fd.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
 ComPtr<IDXGISwapChain1> fg;check(factory->CreateSwapChainForHwnd(queue.Get(),fgWindow,&fd,nullptr,nullptr,&fg));
 // Control reproduces the old routing's E_INVALIDARG. No referenced backbuffers are held.
 auto oldResult=fg->ResizeBuffers(1,300,180,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,0);
 require(oldResult==E_INVALIDARG,"old raw DX11 parameters did not reproduce E_INVALIDARG");
 std::cout<<"CONTROL: raw DX11 one-buffer/sRGB resize rejected by DX12 flip chain (E_INVALIDARG)\n";
 info->ClearStoredMessages();
 for(unsigned i=0;i<8;i++) {
   unsigned w=300+i*17,h=180+i*9;
   // Include UNKNOWN and zero dimensions resolved by native DXGI, not by the FG window.
   check(game->ResizeBuffers(1,i==7?0:w,i==7?0:h,i%2?DXGI_FORMAT_UNKNOWN:DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,0));
   Dx11FgResizeArgs args {};check(ResizeDx11FgSwapchain(game.Get(),fg.Get(),&args));
   DXGI_SWAP_CHAIN_DESC actualGame {};DXGI_SWAP_CHAIN_DESC1 actualFg {};
   check(game->GetDesc(&actualGame));check(fg->GetDesc1(&actualFg));
   require(args.bufferCount==0 && actualFg.BufferCount==4,"FG ring replaced by DX11 count");
   require(actualFg.Width==actualGame.BufferDesc.Width && actualFg.Height==actualGame.BufferDesc.Height,"resolved native dimensions differ");
   require(actualFg.Format==DXGI_FORMAT_R8G8B8A8_UNORM,"sRGB not normalized");
   require(actualFg.Flags==fd.Flags,"FG creation flags not preserved");
   ComPtr<ID3D12Resource> buffer;check(fg->GetBuffer(0,IID_PPV_ARGS(&buffer)));
   require(buffer->GetDesc().Width==actualFg.Width,"backbuffer stale after resize");
 }
 DXGI_SWAP_CHAIN_DESC fakeGame {};fakeGame.BufferDesc.Width=100;fakeGame.BufferDesc.Height=100;
 Dx11FgResizeArgs a {};
 const DXGI_FORMAT inputs[]={DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R10G10B10A2_UNORM};
 const DXGI_FORMAT outputs[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R10G10B10A2_UNORM};
 for(unsigned i=0;i<4;i++){fakeGame.BufferDesc.Format=inputs[i];check(ResolveDx11FgResize(fakeGame,fd,a));require(a.format==outputs[i],"format policy");}
 fakeGame.BufferDesc.Format=DXGI_FORMAT_R32_UINT;require(FAILED(ResolveDx11FgResize(fakeGame,fd,a)),"invalid format accepted");
 require(ResizeDx11FgSwapchain(nullptr,fg.Get())==E_POINTER,"null chain accepted");
 unsigned errors=0;
 for(UINT64 i=0;i<info->GetNumStoredMessages();i++){
  SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<char> storage(size);
  auto* msg=reinterpret_cast<D3D12_MESSAGE*>(storage.data());check(info->GetMessage(i,msg,&size));
  if(msg->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){std::cerr<<msg->pDescription<<'\n';++errors;}
 }
 require(errors==0,"D3D12 debug warnings/errors during corrected resize");
 fg.Reset();game.Reset();DestroyWindow(gameWindow);DestroyWindow(fgWindow);
 std::cout<<"PASS: production resize routing, repeated native sRGB one-buffer -> DX12 flip four-buffer resize, ring/flags preservation, native zero/UNKNOWN resolution, formats/guards; zero D3D12 debug warnings/errors after control\n";
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
