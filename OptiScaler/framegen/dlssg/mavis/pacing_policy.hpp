/*
 * Presentation-pacing policy shared by the addon and its regression test.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <limits>

namespace mfgunlock::pacing {

enum class LatencyGuardMode : uint32_t {
  kOff = 0,
  kMonitor = 1,
  kAutomatic = 2,
};

enum class ReflexModeOverride : uint32_t {
  kGame = 0,
  kOff = 1,
  kOn = 2,
  kOnBoost = 3,
};

enum class ReflexPacingMethod : uint32_t {
  kNativeSleep = 0,
  kDxgiWaitable = 1,
};

inline constexpr ReflexModeOverride NormalizeReflexModeOverride(
    uint32_t value) {
  return value <= static_cast<uint32_t>(ReflexModeOverride::kOnBoost)
             ? static_cast<ReflexModeOverride>(value)
             : ReflexModeOverride::kGame;
}

inline constexpr ReflexPacingMethod NormalizeReflexPacingMethod(
    uint32_t value) {
  return value <= static_cast<uint32_t>(ReflexPacingMethod::kDxgiWaitable)
             ? static_cast<ReflexPacingMethod>(value)
             : ReflexPacingMethod::kNativeSleep;
}

inline constexpr uint32_t NormalizeHeadroomBasisPoints(uint32_t value) {
  return value >= 50 && value <= 300 ? value : 100;
}

// The integer values deliberately match sl::ReflexMode without introducing a
// Streamline-header dependency into this small, independently tested policy.
// "Off" is implemented as an FG-safe sleep bypass: Streamline must continue
// seeing LowLatency because some integrations treat Reflex mode Off as a
// request to disable DLSS-G as well. The slReflexSleep wrapper removes the
// Reflex pacing wait instead.
inline constexpr uint32_t ResolveReflexMode(
    uint32_t native_mode, ReflexModeOverride override_mode) {
  switch (override_mode) {
    case ReflexModeOverride::kOff:
      return 1;
    case ReflexModeOverride::kOn:
      return 1;
    case ReflexModeOverride::kOnBoost:
      return 2;
    default:
      return native_mode;
  }
}

// refresh_millihz keeps 59.94/119.88/239.76 Hz displays precise. Reflex wants
// a whole-microsecond frame interval, so round upward: the limiter must stay
// below the requested VRR ceiling rather than accidentally overshooting it.
inline constexpr uint32_t VrrHeadroomFrameLimitUs(
    uint32_t refresh_millihz, uint32_t basis_points) {
  if (refresh_millihz < 10000 || refresh_millihz > 1000000 ||
      basis_points < 50 || basis_points > 300)
    return 0;
  const uint64_t target_millihz =
      (static_cast<uint64_t>(refresh_millihz) *
       (10000u - basis_points)) /
      10000u;
  if (target_millihz == 0) return 0;
  return static_cast<uint32_t>(
      (1000000000ull + target_millihz - 1ull) / target_millihz);
}

inline constexpr uint32_t StrictestFrameLimitUs(uint32_t a, uint32_t b) {
  if (a == 0) return b;
  if (b == 0) return a;
  return a > b ? a : b;
}

inline constexpr uint32_t ComposeFrameLimitUs(
    uint32_t native_limit_us, uint32_t explicit_limit_us,
    uint32_t latency_guard_limit_us, uint32_t headroom_limit_us) {
  return StrictestFrameLimitUs(
      StrictestFrameLimitUs(native_limit_us, explicit_limit_us),
      StrictestFrameLimitUs(latency_guard_limit_us, headroom_limit_us));
}

enum class MarkerHealth : uint32_t {
  kUnavailable = 0,
  kWaiting = 1,
  kHealthy = 2,
  kMissingSleep = 3,
  kIncomplete = 4,
  kInvalidOrder = 5,
  kUnstableTiming = 6,
};

enum class LatencyBottleneck : uint32_t {
  kInsufficientData = 0,
  kBalanced,
  kDisplayOversubscription,
  kRenderQueue,
  kFrameGeneration,
  kGpuWork,
  kCpuOrSource,
};

struct LatencyGuardRecommendation {
  uint32_t output_target_fps = 0;
  uint32_t source_cap_fps = 0;
  uint32_t estimated_source_fps = 0;
  uint32_t projected_output_fps = 0;
  uint32_t suggested_total_multiplier = 0;
  bool target_from_vsync = false;
  bool data_complete = false;
  bool source_oversubscribed = false;
  bool sustained_queue_pressure = false;
  bool multiplier_may_be_higher_than_needed = false;
};

// Modern Streamline pacing is the native/default path and needs no memory
// patch. Only an explicit request for legacy software-flip compatibility makes
// the old metering-field patch a prerequisite for raising the multiplier.
inline constexpr bool IsReady(bool legacy_software_flip_requested,
                              bool legacy_patch_applied) {
  return !legacy_software_flip_requested || legacy_patch_applied;
}

// Reflex expresses its limiter as an integer frame interval in microseconds.
// Round to nearest instead of truncating so common targets (60/100/120/144)
// do not acquire a systematic high-FPS bias.
inline constexpr uint32_t TargetFpsToFrameLimitUs(uint32_t target_fps) {
  if (target_fps == 0) return 0;
  return static_cast<uint32_t>((1000000ull + target_fps / 2u) / target_fps);
}

// Reflex's limiter is frame-generation aware: the submitted rate is the final
// pacing target, while the driver derives the application-rendered cadence.
// Latency Guard reasons in source FPS, so convert its private queue-trim target
// back to a final/output target before forwarding it to Reflex.
inline constexpr uint32_t SourceFpsCapToReflexOutputTargetFps(
    uint32_t source_fps, uint32_t multiplier) {
  if (source_fps == 0 || multiplier < 2 || multiplier > 6) return 0;
  const uint64_t output_fps =
      static_cast<uint64_t>(source_fps) * multiplier;
  return output_fps > (std::numeric_limits<uint32_t>::max)()
             ? (std::numeric_limits<uint32_t>::max)()
             : static_cast<uint32_t>(output_fps);
}

inline constexpr bool IsValidFixedOutputCap(uint32_t output_fps,
                                            uint32_t multiplier) {
  return output_fps >= 10 && output_fps <= 1000 &&
         multiplier >= 2 && multiplier <= 6;
}

// Legacy conversion retained only for regression coverage of the superseded
// FixedOutputFpsCap behavior. The current Reflex output cap is applied directly
// and does not use this multiplier-dependent conversion.
inline constexpr uint32_t FixedOutputCapFrameLimitUs(uint32_t output_fps,
                                                     uint32_t multiplier) {
  if (!IsValidFixedOutputCap(output_fps, multiplier)) return 0;
  const uint64_t numerator = uint64_t{1000000} * multiplier;
  return static_cast<uint32_t>((numerator + output_fps - 1) / output_fps);
}

inline constexpr uint32_t FrameLimitUsToFps(uint32_t frame_limit_us) {
  if (frame_limit_us == 0) return 0;
  return static_cast<uint32_t>((1000000ull + frame_limit_us / 2u) /
                               frame_limit_us);
}

inline constexpr bool IsValidReflexOutputFpsCap(uint32_t output_fps) {
  return output_fps >= 10 && output_fps <= 1000;
}

enum class SourceCapConfigOrigin : uint32_t {
  kNone = 0,
  kConfigured,
  kLegacyDynamic,
  kLegacyFixedOutput,
};

struct SourceCapConfig {
  uint32_t output_fps = 0;
  SourceCapConfigOrigin origin = SourceCapConfigOrigin::kNone;
};

// Preserve the currently active meaning of the two experimental cap controls
// without writing over their old keys. The current key retains its historical
// name for configuration compatibility, but its value is the final/output FPS
// target passed to Reflex. An explicit value, including zero, always wins.
inline constexpr SourceCapConfig ResolveSourceCapConfig(
    bool current_key_present, uint32_t current_output_fps,
    bool dynamic_configured, bool legacy_dynamic_cap,
    uint32_t dynamic_target_fps, bool legacy_fixed_key_present,
    uint32_t legacy_fixed_output_fps, uint32_t fixed_multiplier) {
  if (current_key_present) {
    return {IsValidReflexOutputFpsCap(current_output_fps)
                ? current_output_fps
                : 0,
            SourceCapConfigOrigin::kConfigured};
  }
  if (dynamic_configured && legacy_dynamic_cap &&
      IsValidReflexOutputFpsCap(dynamic_target_fps)) {
    return {dynamic_target_fps, SourceCapConfigOrigin::kLegacyDynamic};
  }
  if (legacy_fixed_key_present &&
      IsValidFixedOutputCap(legacy_fixed_output_fps, fixed_multiplier)) {
    return {legacy_fixed_output_fps,
            SourceCapConfigOrigin::kLegacyFixedOutput};
  }
  if (legacy_dynamic_cap && IsValidReflexOutputFpsCap(dynamic_target_fps)) {
    return {dynamic_target_fps, SourceCapConfigOrigin::kLegacyDynamic};
  }
  return {};
}

// Resolve the final-output target without pretending that DynamicTargetFPS is
// authoritative under VSync. Streamline 2.14.1 follows the active display in
// that case. The driver-observed target is a fallback for integrations where
// the display refresh cannot be queried reliably.
inline constexpr uint32_t ResolveOutputTargetFps(
    bool vsync_active, uint32_t display_refresh_fps,
    uint32_t configured_dynamic_target_fps,
    uint32_t driver_dynamic_target_us) {
  if (vsync_active && display_refresh_fps != 0) return display_refresh_fps;
  if (!vsync_active && configured_dynamic_target_fps != 0)
    return configured_dynamic_target_fps;
  if (driver_dynamic_target_us != 0)
    return FrameLimitUsToFps(driver_dynamic_target_us);
  return display_refresh_fps;
}

// A latency-first limiter must not divide the display refresh by the generated
// multiplier. Doing so trades away real frames (and therefore input response)
// merely to make all generated frames fit below VSync. Instead, when a robust
// Reflex window proves that the render queue is accumulating, trim only a
// small amount from the sustainable real-frame rate. The native Reflex sleep
// point remains responsible for the actual just-in-time scheduling.
inline constexpr uint32_t RecommendQueueTrimSourceCapFps(
    uint32_t observed_source_interval_us, uint32_t queue_wait_us,
    bool source_timing_confident, uint32_t trim_percent = 3) {
  if (!source_timing_confident || observed_source_interval_us == 0 ||
      queue_wait_us < 1500 || trim_percent == 0 ||
      trim_percent >= 10)
    return 0;
  // Require queueing to account for at least 25% of a real-frame interval.
  // Tiny queues are normal and do not justify reducing real-frame throughput.
  if (static_cast<uint64_t>(queue_wait_us) * 4ull <
      observed_source_interval_us) return 0;
  const uint32_t source_fps = FrameLimitUsToFps(observed_source_interval_us);
  if (source_fps < 30 || source_fps > 1000) return 0;
  const uint32_t cap = static_cast<uint32_t>(
      (static_cast<uint64_t>(source_fps) * (100u - trim_percent) + 99ull) / 100ull);
  // Require a meaningful but bounded change. One-FPS adjustments mostly add
  // limiter churn, while the percentage bound prevents the old 58-FPS cliff.
  return source_fps > cap && source_fps - cap >= 2 ? cap : 0;
}

inline constexpr LatencyGuardRecommendation BuildLatencyGuardRecommendation(
    bool vsync_active, uint32_t display_refresh_fps,
    uint32_t configured_dynamic_target_fps,
    uint32_t driver_dynamic_target_us, uint32_t total_multiplier,
    uint32_t observed_source_interval_us, uint32_t queue_wait_us,
    bool source_timing_confident) {
  LatencyGuardRecommendation result{};
  result.output_target_fps = ResolveOutputTargetFps(
      vsync_active, display_refresh_fps, configured_dynamic_target_fps,
      driver_dynamic_target_us);
  result.target_from_vsync = vsync_active && display_refresh_fps != 0;
  const uint32_t source_fps = FrameLimitUsToFps(observed_source_interval_us);
  result.estimated_source_fps = source_fps;
  if (source_fps != 0 && total_multiplier >= 2) {
    result.projected_output_fps = source_fps * total_multiplier;
    if (result.output_target_fps != 0) {
      const uint32_t required = static_cast<uint32_t>(
          (static_cast<uint64_t>(result.output_target_fps) + source_fps - 1u) /
          source_fps);
      if (required >= 2 && required <= 6) {
        result.suggested_total_multiplier = required;
        result.multiplier_may_be_higher_than_needed =
            total_multiplier > required;
      }
    }
  }
  result.sustained_queue_pressure =
      source_timing_confident && queue_wait_us >= 1500 &&
      static_cast<uint64_t>(queue_wait_us) * 4ull >=
          observed_source_interval_us;
  result.source_cap_fps = RecommendQueueTrimSourceCapFps(
      observed_source_interval_us, queue_wait_us, source_timing_confident);
  result.data_complete = source_timing_confident && source_fps != 0 &&
                         total_multiplier >= 2;
  result.source_oversubscribed =
      result.projected_output_fps != 0 && result.output_target_fps != 0 &&
      static_cast<uint64_t>(result.projected_output_fps) * 100ull >
          static_cast<uint64_t>(result.output_target_fps) * 102ull;
  return result;
}

// A native game cap is never relaxed. The automatic guard may only keep it or
// make it stricter, while the old explicit source-cap option retains its exact
// opt-in behavior for backward compatibility.
inline constexpr uint32_t PreserveStricterNativeLimit(
    uint32_t native_limit_us, uint32_t guard_limit_us) {
  if (guard_limit_us == 0) return native_limit_us;
  if (native_limit_us == 0) return guard_limit_us;
  return native_limit_us > guard_limit_us ? native_limit_us : guard_limit_us;
}

inline constexpr bool ShouldApplyAutomaticLatencyCap(
    LatencyGuardMode mode, MarkerHealth marker_health, bool reflex_hooked,
    bool reflex_options_seen, const LatencyGuardRecommendation& recommendation,
    bool sustained_queue_pressure = false) {
  return mode == LatencyGuardMode::kAutomatic &&
         marker_health == MarkerHealth::kHealthy && reflex_hooked &&
         reflex_options_seen && recommendation.data_complete &&
         recommendation.source_cap_fps != 0 &&
         (recommendation.sustained_queue_pressure || sustained_queue_pressure);
}

// Classification is deliberately diagnostic. It attributes the largest
// observed stage without pretending that marker-to-GPU time is end-to-end
// display latency or that every large value is actionable by this addon.
inline constexpr LatencyBottleneck ClassifyLatencyBottleneck(
    bool timing_confident, bool oversubscribed, uint32_t queue_p95_us,
    uint32_t source_interval_us, uint32_t gpu_active_us,
    uint32_t ai_frame_time_us) {
  if (!timing_confident || source_interval_us == 0)
    return LatencyBottleneck::kInsufficientData;
  if (oversubscribed && queue_p95_us >= 1500 &&
      static_cast<uint64_t>(queue_p95_us) * 4ull >= source_interval_us)
    return LatencyBottleneck::kDisplayOversubscription;
  if (queue_p95_us >= 1500 &&
      static_cast<uint64_t>(queue_p95_us) * 4ull >= source_interval_us)
    return LatencyBottleneck::kRenderQueue;
  if (ai_frame_time_us >= 3000 && ai_frame_time_us > gpu_active_us / 3)
    return LatencyBottleneck::kFrameGeneration;
  if (gpu_active_us != 0 &&
      static_cast<uint64_t>(gpu_active_us) * 100ull >=
          static_cast<uint64_t>(source_interval_us) * 80ull)
    return LatencyBottleneck::kGpuWork;
  if (source_interval_us >= 20000 && gpu_active_us != 0 &&
      static_cast<uint64_t>(gpu_active_us) * 2ull < source_interval_us)
    return LatencyBottleneck::kCpuOrSource;
  return LatencyBottleneck::kBalanced;
}

inline constexpr bool ShouldTrialLowerMultiplier(
    LatencyGuardMode mode, MarkerHealth marker_health,
    uint32_t configured_multiplier, uint32_t suggested_multiplier,
    bool dynamic_active, bool source_oversubscribed,
    bool sustained_queue_pressure) {
  return mode == LatencyGuardMode::kAutomatic &&
         marker_health == MarkerHealth::kHealthy && !dynamic_active &&
         configured_multiplier >= 3 && suggested_multiplier >= 2 &&
         suggested_multiplier < configured_multiplier &&
         source_oversubscribed && sustained_queue_pressure;
}

}  // namespace mfgunlock::pacing
