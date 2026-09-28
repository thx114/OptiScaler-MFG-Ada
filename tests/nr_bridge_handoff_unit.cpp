#include "../OptiScaler/dlssnr/DlssNr_FinishedReady.h"
#include <cassert>
#include <cstdio>

int main()
{
    using namespace DlssNr;
    FinishedBridgeHandoff h;
    h.Prepare(true, 3);
    assert(!h.ordered); // Failed Signal/Wait: no Commit, no permission.
    h.Commit(true, 3);
    assert(h.ordered && h.candidate == 0);
    assert(FinishedInputReady(false, 0, 3, h.ordered));
    assert(!FinishedInputReady(false, UINT64_MAX, 3, h.ordered));
    h.Reset();
    assert(!h.ordered && h.candidate == 0);
    h.Prepare(true, 3);
    h.Commit(true, 5); // Reused slot cannot inherit the old snapshot.
    assert(!h.ordered);
    h.Prepare(false, 5);
    h.Commit(true, 5); // Submission after snapshot was not covered by the signal.
    assert(!h.ordered);
    h.Prepare(true, 5);
    h.Commit(false, 5); // Different producer/cancelled slot.
    assert(!h.ordered);
    for (unsigned bits = 0; bits < 32; ++bits)
    {
        const bool bridge = bits & 1, fg = bits & 2, external = bits & 4;
        const bool residual = bits & 8, ordered = bits & 16;
        assert(UseFinishedBridgeHandoff(bridge, fg, external, residual, ordered) ==
               (bridge && !fg && !external && !residual && ordered));
    }
    assert(!FinishedInputReady(false, 0, 3)); // Native/FG policy unchanged.
    puts("PASS NR bridge handoff: snapshot/commit, failed sync, reuse, future submission, device removal, 32 gates");
}
