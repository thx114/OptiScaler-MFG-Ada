#pragma once
#include <cstdint>

namespace DlssNr
{
// A native FG present can run before the render submission it would otherwise wait for.
// Cross-queue bypass requires an explicit transitive GPU handoff, never just a submitted list.
inline bool FinishedInputReady(bool sameQueue, uint64_t completed, uint64_t required, bool orderedHandoff = false)
{
    return completed != UINT64_MAX && (sameQueue || orderedHandoff || completed >= required);
}

// Snapshot BEFORE the D3D12 signal; commit only AFTER the DX11 wait succeeds.
// New recordings cannot inherit a previous frame's ordering proof.
struct FinishedBridgeHandoff
{
    uint64_t candidate = 0;
    bool ordered = false;
    void Reset() { candidate = 0; ordered = false; }
    void Prepare(bool eligible, uint64_t ready) { candidate = eligible ? ready : 0; }
    void Commit(bool eligible, uint64_t ready)
    {
        if (eligible && candidate != 0 && candidate == ready)
            ordered = true;
        candidate = 0;
    }
};

inline bool UseFinishedBridgeHandoff(bool bridge, bool fgEnabled, bool externalFg, bool residualOnly, bool ordered)
{
    return bridge && !fgEnabled && !externalFg && !residualOnly && ordered;
}
}
