#pragma once
#include <cstdint>

namespace DlssNr
{
// Producer activity, NOT GPU completion: an in-flight frame may still need replay.
// Only the DX11 bridge's real game handoff may advance the missing-frame count.
// Native/generated presentations use the time limit, never a generated-frame count.
struct FinishedInputActivity
{
    uint64_t generation = 0, observed = 0, lastInputMs = 0;
    unsigned missingGameFrames = 0;
    bool hasInput = false;
    void Clear() { *this = {}; }
    void Input(uint64_t nowMs)
    {
        ++generation;
        lastInputMs = nowMs;
        hasInput = true;
    }
    bool Idle(bool realBridgeFrame, uint64_t nowMs)
    {
        if (!hasInput)
            return true;
        if (observed != generation)
        {
            observed = generation;
            missingGameFrames = 0;
            return false; // The producer ran for this picture, even after a long GPU frame.
        }
        if (realBridgeFrame && missingGameFrames < 2)
            ++missingGameFrames;
        return missingGameFrames >= 2 || (nowMs >= lastInputMs && nowMs - lastInputMs >= 500);
    }
};
}

