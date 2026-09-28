#include "../OptiScaler/dlssnr/DlssNr_PassChain.h"
#include <cassert>
#include <limits>
#include <iostream>
int main()
{
    std::array<DlssNr::ChainSetting, DlssNr::ChainCapacity> settings;
    settings[0] = {true, .5f, .6f};
    settings[1] = {true, .25f, .7f};
    settings[2] = {true, .75f, .8f};
    auto all = DlssNr::BuildPassChain(settings, 3, 2560, 1600);
    assert(all.count == 3 && all.maxWidth == 1920 && all.maxHeight == 1200);
    settings[1].enabled = false;
    auto skipped = DlssNr::BuildPassChain(settings, 3, 2560, 1600);
    assert(skipped.count == 2 && skipped.passes[1].logical == 2 && skipped.passes[1].blend == .8f);
    assert(!all.SameLayout(skipped));
    settings[0].enabled = false;
    auto onlyThird = DlssNr::BuildPassChain(settings, 3, 2560, 1600);
    assert(onlyThird.count == 1 && onlyThird.passes[0].logical == 2 && onlyThird.passes[0].width == 1920);
    settings[2].enabled = false;
    assert(DlssNr::BuildPassChain(settings, 3, 2560, 1600).count == 0);
    settings[0] = {true, std::numeric_limits<float>::quiet_NaN(), 2.f};
    auto sanitized = DlssNr::BuildPassChain(settings, 1, 2560, 1600);
    assert(sanitized.passes[0].width == 2560 && sanitized.passes[0].blend == 1.f);
    settings[0].blend = .4f;
    assert(sanitized.SameLayout(DlssNr::BuildPassChain(settings, 1, 2560, 1600)));
    // Sequential composition: second layer uses the blended first result, not the game again.
    const float game = .2f, model1 = .8f, model2 = .9f;
    const float mixed1 = game * (1.f - .6f) + model1 * .6f;
    const float mixed2 = mixed1 * (1.f - .7f) + model2 * .7f;
    assert(std::abs(mixed1 - .56f) < 1e-6f && std::abs(mixed2 - .798f) < 1e-6f);
    std::cout << "PASS per-layer chain: skip, logical identity, extents, zero layers, sanitization, blend layout and composition\n";
}

