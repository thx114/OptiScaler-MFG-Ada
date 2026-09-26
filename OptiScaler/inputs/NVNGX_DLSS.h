#pragma once

#include <chrono>
#include <memory>

template <typename FeatureType> struct ContextData
{
    std::unique_ptr<FeatureType> feature;
    NVSDK_NGX_Parameter* createParams = nullptr;
    int changeBackendCounter = 0;

    // Dormant-reuse: when the game releases a feature it intends to recreate with identical
    // parameters (Star Rail does this every time a UI screen opens/closes), we keep the
    // fully-initialized feature alive in the contexts map instead of destroying it. The
    // matching CreateFeature call wakes it up and returns the same handle, turning the
    // release+recreate cycle into a no-op and skipping the ~100 ms NGX teardown+rebuild.
    bool dormant = false;
    std::chrono::steady_clock::time_point dormantSince {};
};
