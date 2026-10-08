#pragma once
namespace FinalReShadeStage
{
// 当前真实帧的最终绘制请求；FG/NR/UI 写入提交后执行，实际 Present 前结束。
// 不跨线程传递，不运行于插值帧的内部重复呈现，也不添加队列等待。
struct Request
{
    const void* target;
    void* context;
    void (*render)(void*);
    bool attempted=false;
    void TryRender(const void* presenting,bool test)
    {
        if(test || presenting!=target || attempted) return;
        attempted=true;
        render(context);
    }
};
inline thread_local Request* current=nullptr;
class Scope
{
    Request* previous;
  public:
    explicit Scope(Request& request):previous(current){current=&request;}
    ~Scope(){current=previous;}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
inline void RenderAfterWrites(const void* swapchain,bool test)
{
    if(current) current->TryRender(swapchain,test);
}
}
