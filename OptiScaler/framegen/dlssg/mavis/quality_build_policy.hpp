#pragma once

#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD) && \
    (defined(MFGUNLOCK_NVAPI_PROBE) || defined(MFGUNLOCK_NVAPI_TEMPORAL))
#error Local low-overhead builds must not include NVAPI launch interception
#endif

namespace mfgunlock::qualitybuild {

#if defined(MFGUNLOCK_LOCAL_LOW_OVERHEAD)
inline constexpr bool kConfidenceHistoryEnabled = false;
#else
inline constexpr bool kConfidenceHistoryEnabled = true;
#endif

// Inputs have already been normalized by the public configuration policy.
// The local build ignores saved research modes without rewriting their keys.
inline constexpr unsigned int StabilityMode(unsigned int normalized) {
  return kConfidenceHistoryEnabled ? normalized : 1;  // Local Stable
}

inline constexpr unsigned int InpaintMode(unsigned int normalized) {
  return kConfidenceHistoryEnabled ? normalized : 0;  // V2 Compatibility
}

}  // namespace mfgunlock::qualitybuild
