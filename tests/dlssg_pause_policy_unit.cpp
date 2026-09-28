#include "../OptiScaler/framegen/dlssg/DlssgPausePolicy.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

int main()
{
    using namespace DlssgPausePolicy;
    for (bool soft : {false, true})
        for (bool enabled : {false, true})
            for (bool active : {false, true})
                for (bool retained : {false, true})
                {
                    const bool keep = KeepRuntimeAlive(soft, enabled);
                    assert(keep == (soft && enabled));
                    assert(NeedsDeactivate(active, retained, keep) ==
                           (active || (retained && (!soft || !enabled))));
                }
    // FG 6x/2x stays enabled: retain resources during transient pauses.
    assert(!NeedsDeactivate(false, true, KeepRuntimeAlive(true, true)));
    // User disables FG after it already soft-paused: still send eOff.
    assert(NeedsDeactivate(false, true, KeepRuntimeAlive(true, false)));
    // A successful eOff clears retained state: no repeated disable calls.
    assert(!NeedsDeactivate(false, false, KeepRuntimeAlive(true, false)));
    // Failed eOff retains state: retry even though local active state is false.
    assert(NeedsDeactivate(false, true, KeepRuntimeAlive(false, false)));
    std::puts("DLSSG pause policy: all 16 combinations and transitions passed");
}
