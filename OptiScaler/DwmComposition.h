#pragma once

#include <pch.h>
#include <Config.h>
#include <Logger.h>

namespace DwmComposition
{
    // WindowsApp.lib does not carry the classic layered-window API; resolve it from the
    // always-loaded user32.dll at runtime instead of changing the project's link line.
    inline bool SetLayeredAttributesOpaque(HWND root)
    {
        using SetLayeredWindowAttributesWFn = BOOL(WINAPI*)(HWND, COLORREF, BYTE, DWORD);
        static SetLayeredWindowAttributesWFn fn = [] {
            const HMODULE user32 = GetModuleHandleW(L"user32.dll");
            return user32 ? reinterpret_cast<SetLayeredWindowAttributesWFn>(
                                GetProcAddress(user32, "SetLayeredWindowAttributesW"))
                          : nullptr;
        }();
        constexpr DWORD kLwaAlpha = 0x2;
        return fn != nullptr && fn(root, 0, 255, kLwaAlpha) != FALSE;
    }

    // Some frame-generation pipelines pace and look noticeably worse while the game's window
    // presents through independent flip: DWM is not involved, so the generated-frame cadence
    // reaches scanout raw and any FG reset shows up as stutter or flicker. An overlay such as
    // Xbox Game Bar forces the window into the DWM composition tree (composed flip), where
    // every present is sampled at the display rate - smoother output, hidden reset flicker,
    // at the cost of up to one compositor frame of latency.
    //
    // Applying WS_EX_LAYERED to the game's top-level window (fully opaque, no visual change)
    // requests the same composition path without a visible overlay. Guarded by the
    // [FrameGen] ForceDwmComposition option; the style bit is only touched on state changes
    // and this helper remembers whether it set the bit itself.
    inline void Update(HWND hwnd, bool fgActive)
    {
        static HWND appliedRoot = nullptr;
        static bool appliedByUs = false;
        static bool lastWanted = false;

        const bool wanted = fgActive && Config::Instance()->ForceDwmComposition.value_or_default();
        if (wanted == lastWanted && (!wanted || appliedRoot == hwnd))
            return;

        if (hwnd == nullptr || !IsWindow(hwnd))
            return;

        const HWND root = GetAncestor(hwnd, GA_ROOT);
        if (root == nullptr)
            return;

        const LONG_PTR exStyle = GetWindowLongPtrW(root, GWL_EXSTYLE);
        if (wanted && (exStyle & WS_EX_LAYERED) == 0)
        {
            SetWindowLongPtrW(root, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
            SetLayeredAttributesOpaque(root);
            appliedByUs = true;
            LOG_INFO("DwmComposition: composed presentation requested on window {}", (void*) root);
        }
        else if (!wanted && appliedByUs && (exStyle & WS_EX_LAYERED) != 0)
        {
            SetWindowLongPtrW(root, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
            appliedByUs = false;
            LOG_INFO("DwmComposition: layered style removed from window {}", (void*) root);
        }

        lastWanted = wanted;
        appliedRoot = root;
    }
}
