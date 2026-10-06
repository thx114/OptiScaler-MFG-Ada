#include "pch.h"

#include <Config.h>
#include <Logger.h>
#include <resource.h>
#include <Util.h>

#include "menu_common.h"
#include "menu_dx_base.h"
#include <framegen/dlssg/DepthDebugMenuRegions.h>

#include <imgui/imgui_impl_win32.h>

bool MenuDxBase::RenderMenu()
{
    if (Config::Instance()->OverlayMenu.value_or_default())
        return false;

    if (MenuCommon::RenderMenu())
    {
        ImGui::Render();
        DepthDebugMenuRegions::Snapshot regions;
        regions.updated = GetTickCount64();
        auto* draw = ImGui::GetDrawData();
        if (MenuCommon::IsVisible() && draw && draw->DisplaySize.x>0 && draw->DisplaySize.y>0)
            for (int i=0;i<draw->CmdListsCount;++i)
                for (const auto& cmd : draw->CmdLists[i]->CmdBuffer)
                {
                    if (cmd.ElemCount==0 || cmd.UserCallback) continue;
                    DepthDebugMenuRegions::Rect rect {
                        (cmd.ClipRect.x-draw->DisplayPos.x)/draw->DisplaySize.x,
                        (cmd.ClipRect.y-draw->DisplayPos.y)/draw->DisplaySize.y,
                        (cmd.ClipRect.z-draw->DisplayPos.x)/draw->DisplaySize.x,
                        (cmd.ClipRect.w-draw->DisplayPos.y)/draw->DisplaySize.y};
                    // Ignore full-screen dimming; preserve actual menu clips.
                    if ((rect.right-rect.left)*(rect.bottom-rect.top)>0.85f) continue;
                    DepthDebugMenuRegions::Add(regions,rect);
                    // Native Unity and interop output can invert Y independently
                    // of MenuFlipY. Protect both locations without changing input.
                    DepthDebugMenuRegions::Add(regions,{rect.left,1-rect.bottom,rect.right,1-rect.top});
                }
        if (MenuCommon::IsVisible() && regions.count==0)
            DepthDebugMenuRegions::Add(regions,{0,0,.8f,1});
        DepthDebugMenuRegions::Store(regions);
        return true;
    }

    DepthDebugMenuRegions::Store({});
    return false;
}

bool MenuDxBase::IsHandleDifferent()
{
    if (Config::Instance()->OverlayMenu.value_or_default())
        return false;

    HWND frontWindow = Util::GetProcessWindow();

    if (frontWindow != nullptr && frontWindow == _handle)
        return false;

    _handle = frontWindow;

    return true;
}

void MenuDxBase::Dx11Ready() { MenuCommon::Dx11Inited(); }

void MenuDxBase::Dx12Ready() { MenuCommon::Dx12Inited(); }

MenuDxBase::MenuDxBase(HWND handle) : _handle(handle)
{
    if (Config::Instance()->OverlayMenu.value_or_default())
        return;

    MenuCommon::Init(handle, false);

    _baseInit = MenuCommon::IsInited();
}

MenuDxBase::~MenuDxBase()
{
    if (!_baseInit)
        return;

    MenuCommon::Shutdown();
}

bool MenuDxBase::IsVisible() { return MenuCommon::IsVisible(); }
