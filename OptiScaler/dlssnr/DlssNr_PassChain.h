#pragma once
#include <array>
#include <algorithm>
#include <cmath>

namespace DlssNr
{
constexpr unsigned ChainCapacity = 30;
struct ChainSetting { bool enabled = true; float scale = 1.0f; float blend = 1.0f; };
struct ChainPass { unsigned logical = 0, width = 0, height = 0; float scale = 1.0f, blend = 1.0f; };
struct PassChain
{
    std::array<ChainPass, ChainCapacity> passes {};
    unsigned count = 0, maxWidth = 0, maxHeight = 0;
    bool SameLayout(const PassChain& other) const
    {
        if (count != other.count) return false;
        for (unsigned i = 0; i < count; ++i)
            if (passes[i].logical != other.passes[i].logical || passes[i].width != other.passes[i].width ||
                passes[i].height != other.passes[i].height) return false;
        return true;
    }
};
inline float ChainScale(float scale) { return std::isfinite(scale) ? std::clamp(scale, 0.25f, 2.0f) : 1.0f; }
inline PassChain BuildPassChain(const std::array<ChainSetting, ChainCapacity>& settings, unsigned count,
                               unsigned width, unsigned height)
{
    PassChain plan;
    for (unsigned i = 0; i < std::min(count, ChainCapacity); ++i)
    {
        const auto& setting = settings[i];
        if (!setting.enabled) continue;
        const float scale = ChainScale(setting.scale);
        const float blend = std::isfinite(setting.blend) ? std::clamp(setting.blend, 0.0f, 1.0f) : 1.0f;
        auto& pass = plan.passes[plan.count++];
        pass = { i, std::max(1u, (unsigned)(width * scale + 0.5f)),
                 std::max(1u, (unsigned)(height * scale + 0.5f)), scale, blend };
        plan.maxWidth = std::max(plan.maxWidth, pass.width);
        plan.maxHeight = std::max(plan.maxHeight, pass.height);
    }
    return plan;
}
}

