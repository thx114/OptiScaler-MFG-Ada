#include "pch.h"
#include "version_check.h"
#include "State.h"
#include "resource.h"
#include <format>
#include <mutex>

const std::string& VersionCheck::CurrentVersionString()
{
    static const std::string version =
        std::format("{}.{}.{}", VER_MAJOR_VERSION, VER_MINOR_VERSION, VER_HOTFIX_VERSION);
    return version;
}

// This fork is distributed manually. Never query another fork's releases or
// start a background worker, including when an old INI enables update checks.
void VersionCheck::Start()
{
    auto& state = State::Instance();
    std::scoped_lock lock(state.versionCheckMutex);
    state.versionCheckInProgress = false;
    state.versionCheckCompleted = false;
    state.updateAvailable = false;
    state.versionCheckError.clear();
    state.latestVersionTag.clear();
    state.latestVersionUrl.clear();
}
