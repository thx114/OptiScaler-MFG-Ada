#pragma once
#include <algorithm>

namespace DlssNr
{
// Reference add-on's embedded UI describes this as a compatibility policy, not
// a new segmentation algorithm. Input has already been sanitized by PassTuning.
inline float SkinIndependentStructure(float structure, bool autoMask, bool independent)
{
    return autoMask && independent ? std::min(structure, 1.0f) : structure;
}
}

