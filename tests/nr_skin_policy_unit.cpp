#include "../OptiScaler/dlssnr/DlssNr_SkinPolicy.h"
#include <cassert>
#include <cstdio>
int main()
{
    for (bool mask : {false, true})
        for (bool independent : {false, true})
            for (float structure : {0.0f, 0.25f, 0.84f, 1.0f, 1.5f, 2.0f})
            {
                const float actual = DlssNr::SkinIndependentStructure(structure, mask, independent);
                const float expected = mask && independent && structure > 1.0f ? 1.0f : structure;
                assert(actual == expected);
            }
    // Re-evaluate from configured values, never persist the capped value.
    assert(DlssNr::SkinIndependentStructure(2.0f, true, true) == 1.0f);
    assert(DlssNr::SkinIndependentStructure(2.0f, true, false) == 2.0f);
    assert(DlssNr::SkinIndependentStructure(2.0f, false, true) == 2.0f);
    std::puts("PASS skin policy: 24 gates/endpoints, toggle and mask restore configured structure");
}

