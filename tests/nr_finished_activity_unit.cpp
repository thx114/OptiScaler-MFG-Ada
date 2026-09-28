#include "../OptiScaler/dlssnr/DlssNr_FinishedActivity.h"
#include <cassert>
#include <cstdio>

int main()
{
    using DlssNr::FinishedInputActivity;
    FinishedInputActivity a;
    assert(a.Idle(true, 0)); // No SR ever ran: do not paint a stale snapshot.
    a.Input(10);
    assert(!a.Idle(true, 20));
    assert(!a.Idle(true, 30)); // One transient gap retains anti-flicker replay.
    assert(a.Idle(true, 40)); // Second real game frame without SR: let ESC draw.
    a.Clear();
    assert(a.Idle(true, 50));
    a.Input(60);
    assert(!a.Idle(true, 70)); // Returning to gameplay resumes automatically.
    for (unsigned frame = 0; frame < 100; ++frame)
    {
        a.Input(100 + frame * 30);
        assert(!a.Idle(true, 100 + frame * 30));
        for (unsigned generated = 0; generated < 6; ++generated)
            assert(!a.Idle(false, 100 + frame * 30 + generated));
    }
    // Submitted-but-busy input still renews activity; completion does not drive this policy.
    a.Input(4000);
    assert(!a.Idle(true, 4010));
    a.Input(4020);
    assert(!a.Idle(true, 4030));
    // Native/generated presents alone cannot consume the real-frame grace period.
    for (unsigned generated = 0; generated < 40; ++generated)
        assert(!a.Idle(false, 4030 + generated));
    assert(!a.Idle(false, 4519));
    assert(a.Idle(false, 4520)); // Bounded time-based escape for a native inactive producer.
    a.Input(5000);
    assert(!a.Idle(true, 6000)); // A newly observed long GPU frame isn't mistaken for a menu.
    assert(!a.Idle(false, 4999)); // Clock underflow must not expire the producer.
    a.Clear();
    assert(a.Idle(false, 6001));
    puts("PASS finished-input activity: ESC, resume, transient gaps, busy ring, FG6, native timeout, cancel");
}

