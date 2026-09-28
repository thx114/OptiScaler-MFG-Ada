#pragma once

namespace DlssgPausePolicy
{
constexpr bool KeepRuntimeAlive(bool softPause, bool enabled)
{
    return softPause && enabled;
}

constexpr bool NeedsDeactivate(bool active, bool retainedRuntime, bool keepAlive)
{
    // A menu disable must also reach the runtime after an earlier transient soft pause.
    return active || (retainedRuntime && !keepAlive);
}
}
