#pragma once
#include <array>
#include <mutex>
#include <cstdint>
#include <algorithm>

// The fallback menu is baked into the upscaler output before the FG depth
// preview. Snapshot normalized clips instead of moving its ImGui backend/input
// hooks to another device. Reads never access ImGui from the Present thread.
namespace DepthDebugMenuRegions
{
struct Rect { float left, top, right, bottom; };
struct Snapshot { std::array<Rect, 8> rects{}; uint32_t count=0; uint64_t updated=0; };
inline std::mutex mutex;
inline Snapshot latest;
inline void Store(const Snapshot& value) { std::lock_guard lock(mutex); latest=value; }
inline Snapshot Read(uint64_t now)
{
    std::lock_guard lock(mutex);
    return now >= latest.updated && now-latest.updated < 500 ? latest : Snapshot{};
}
inline void Add(Snapshot& out, Rect rect)
{
    rect.left=std::clamp(rect.left,0.0f,1.0f); rect.right=std::clamp(rect.right,0.0f,1.0f);
    rect.top=std::clamp(rect.top,0.0f,1.0f); rect.bottom=std::clamp(rect.bottom,0.0f,1.0f);
    if (rect.right<=rect.left || rect.bottom<=rect.top || out.count>=out.rects.size()) return;
    for (uint32_t i=0;i<out.count;++i)
        if (out.rects[i].left<=rect.left && out.rects[i].right>=rect.right &&
            out.rects[i].top<=rect.top && out.rects[i].bottom>=rect.bottom) return;
    out.rects[out.count++]=rect;
}
}
