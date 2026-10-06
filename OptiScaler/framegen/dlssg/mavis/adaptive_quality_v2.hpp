/* SPDX-License-Identifier: MIT
 * Frame-local confidence stabilization for Adaptive Quality Suite V2.
 *
 * V2 keeps structural rejection (bounds, sentinels and finite-color checks)
 * discrete, but replaces the addon's semantic accept/reject transition with a
 * continuous weight. It does not add history, resource reads or storage.
 */
#pragma once

#include <cmath>

namespace mfgunlock::adaptivequalityv2 {

inline constexpr const char* kSmoothWarpConfidence = R"PTX(
// MFGUNLOCK_SMOOTH_WARP_CONFIDENCE_V2
add.sat.f32 %qf8, %f148, 0f00000000;
add.sat.f32 %qf10, %f149, 0f00000000;
// Continuous two-candidate agreement evidence. qv3 still requires both
// candidates to have passed the structural validity checks.
sub.f32 %qf2, %qf6, %qf9;
sub.f32 %qf2, %qf2, 0f3DA3D70A;
mul.sat.f32 %qf2, %qf2, 0f40D55555;
sub.f32 %qf3, 0f3E19999A, %qf9;
mul.sat.f32 %qf3, %qf3, 0f40D55555;
min.f32 %qf2, %qf2, %qf3;
@!%qv3 mov.f32 %qf2, 0f00000000;
// Per-candidate native evidence uses the same V1 endpoints, but no longer
// turns the whole addon path on at a single predicate boundary.
sub.f32 %qf3, %qf6, 0f3E800000;
mul.sat.f32 %qf3, %qf3, 0f40800000;
sub.f32 %qf4, %qf8, 0f3E4CCCCD;
mul.sat.f32 %qf4, %qf4, 0f40555555;
min.f32 %qf4, %qf4, %qf3;
max.f32 %qf4, %qf4, %qf2;
sub.f32 %qf5, %qf10, 0f3E4CCCCD;
mul.sat.f32 %qf5, %qf5, 0f40555555;
min.f32 %qf5, %qf5, %qf3;
max.f32 %qf5, %qf5, %qf2;
// MFGUNLOCK_CONFIDENCE_ENDPOINT_FAST_PATHS_V2
// Invalid/zero-confidence candidates cannot contribute. Full confidence is
// exactly the already-clamped V1 target in qf0/qf1. Skip the interpolation in
// both endpoint cases; partial confidence keeps the original V2 sequence.
@!%qv0 mov.f32 %qf0, 0f00000000;
@!%qv0 bra MFGUNLOCK_FORWARD_CONFIDENCE_DONE_V2;
setp.le.f32 %qv5, %qf4, 0f00000000;
@%qv5 mov.f32 %qf0, 0f00000000;
@%qv5 bra MFGUNLOCK_FORWARD_CONFIDENCE_DONE_V2;
setp.ge.f32 %qv5, %qf4, 0f3F800000;
@%qv5 bra MFGUNLOCK_FORWARD_CONFIDENCE_DONE_V2;
// Smoothstep confidence has zero slope at both endpoints.
fma.rn.f32 %qf7, %qf4, 0fC0000000, 0f40400000;
mul.f32 %qf4, %qf4, %qf4;
mul.f32 %qf4, %qf4, %qf7;
// First reconstruct the V1 target from its native anchor, then fade the whole
// addon contribution from zero. Confidence zero is therefore an exact no-op;
// confidence one is the V1 result.
sub.f32 %qf6, %qf0, %qf8;
fma.rn.f32 %qf0, %qf4, %qf6, %qf8;
mul.f32 %qf0, %qf0, %qf4;
MFGUNLOCK_FORWARD_CONFIDENCE_DONE_V2:
@!%qv1 mov.f32 %qf1, 0f00000000;
@!%qv1 bra MFGUNLOCK_INVERSE_CONFIDENCE_DONE_V2;
setp.le.f32 %qv5, %qf5, 0f00000000;
@%qv5 mov.f32 %qf1, 0f00000000;
@%qv5 bra MFGUNLOCK_INVERSE_CONFIDENCE_DONE_V2;
setp.ge.f32 %qv5, %qf5, 0f3F800000;
@%qv5 bra MFGUNLOCK_INVERSE_CONFIDENCE_DONE_V2;
fma.rn.f32 %qf7, %qf5, 0fC0000000, 0f40400000;
mul.f32 %qf5, %qf5, %qf5;
mul.f32 %qf5, %qf5, %qf7;
sub.f32 %qf6, %qf1, %qf10;
fma.rn.f32 %qf1, %qf5, %qf6, %qf10;
mul.f32 %qf1, %qf1, %qf5;
MFGUNLOCK_INVERSE_CONFIDENCE_DONE_V2:
)PTX";

inline constexpr const char* kBorderConfidence = R"PTX(
// MFGUNLOCK_BORDER_CONFIDENCE_V2
cvt.rn.f32.u32 %qf2, %r10;
cvt.rn.f32.u32 %qf3, %r11;
sub.f32 %qf4, 0f3F800000, %f123;
min.f32 %qf4, %qf4, %f123;
mul.f32 %qf4, %qf4, %qf2;
sub.f32 %qf6, 0f3F800000, %f124;
min.f32 %qf6, %qf6, %f124;
mul.f32 %qf6, %qf6, %qf3;
min.f32 %qf4, %qf4, %qf6;
sub.f32 %qf5, 0f3F800000, %f129;
min.f32 %qf5, %qf5, %f129;
mul.f32 %qf5, %qf5, %qf2;
sub.f32 %qf6, 0f3F800000, %f130;
min.f32 %qf6, %qf6, %f130;
mul.f32 %qf6, %qf6, %qf3;
min.f32 %qf5, %qf5, %qf6;
setp.ge.f32 %qv5, %qf4, 0f40200000;
setp.ge.f32 %qv6, %qf5, 0f40200000;
and.pred %qv2, %qv5, %qv6;
@%qv2 bra MFGUNLOCK_BORDER_CONFIDENCE_DONE_V2;
sub.f32 %qf4, %qf4, 0f3F000000;
mul.sat.f32 %qf4, %qf4, 0f3F000000;
fma.rn.f32 %qf6, %qf4, 0fC0000000, 0f40400000;
mul.f32 %qf4, %qf4, %qf4;
mul.f32 %qf4, %qf4, %qf6;
mul.f32 %qf0, %qf0, %qf4;
sub.f32 %qf5, %qf5, 0f3F000000;
mul.sat.f32 %qf5, %qf5, 0f3F000000;
fma.rn.f32 %qf6, %qf5, 0fC0000000, 0f40400000;
mul.f32 %qf5, %qf5, %qf5;
mul.f32 %qf5, %qf5, %qf6;
mul.f32 %qf1, %qf1, %qf5;
MFGUNLOCK_BORDER_CONFIDENCE_DONE_V2:
)PTX";

inline constexpr const char* kCandidateArbitration = R"PTX(
// MFGUNLOCK_CANDIDATE_ARBITRATION_V3
sub.f32 %qf2, %qf9, 0f3D4CCCCD;
mul.sat.f32 %qf2, %qf2, 0f41200000;
@!%qv3 mov.f32 %qf2, 0f00000000;
setp.le.f32 %qv2, %qf2, 0f00000000;
@%qv2 bra MFGUNLOCK_CANDIDATE_ARBITRATION_DONE_V3;
sub.f32 %qf3, %qf0, %qf1;
abs.f32 %qf4, %qf3;
mul.sat.f32 %qf2, %qf2, %qf4;
fma.rn.f32 %qf4, %qf2, 0fC0000000, 0f40400000;
mul.f32 %qf2, %qf2, %qf2;
mul.f32 %qf2, %qf2, %qf4;
fma.rn.f32 %qf4, %qf2, 0fBF400000, 0f3F800000;
setp.ge.f32 %qv2, %qf3, 0f00000000;
selp.f32 %qf2, %qf1, %qf0, %qv2;
mul.f32 %qf2, %qf2, %qf4;
@%qv2 mov.f32 %qf1, %qf2;
@!%qv2 mov.f32 %qf0, %qf2;
MFGUNLOCK_CANDIDATE_ARBITRATION_DONE_V3:
// Zero confidence must not execute even a zero-weight FMA over candidate data.
setp.gt.f32 %qv5, %qf0, 0f00000000;
and.pred %qv0, %qv0, %qv5;
setp.gt.f32 %qv6, %qf1, 0f00000000;
and.pred %qv1, %qv1, %qv6;
)PTX";

inline float Smoothstep01(float value) {
  if (!(value > 0.0f)) return 0.0f;
  if (value >= 1.0f) return 1.0f;
  return value * value * (3.0f - 2.0f * value);
}

inline float EffectiveWarpWeight(float native_weight, float v1_target,
                                 float confidence) {
  const auto clamp01 = [](float value) {
    if (!(value > 0.0f)) return 0.0f;
    return value < 1.0f ? value : 1.0f;
  };
  native_weight = clamp01(native_weight);
  v1_target = clamp01(v1_target);
  confidence = clamp01(confidence);
  if (confidence == 0.0f) return 0.0f;
  if (confidence == 1.0f) return v1_target;
  confidence = Smoothstep01(confidence);
  const float v1_weight =
      native_weight + confidence * (v1_target - native_weight);
  return confidence * v1_weight;
}

inline float GeometryRelaxation(float best_support, float second_support) {
  best_support = Smoothstep01(best_support);
  second_support = Smoothstep01(second_support);
  return best_support * (0.25f + 0.25f * second_support);
}

inline bool NeedsInpaintV2(float value) {
  return value > 0.0f || !std::isfinite(value);
}

}  // namespace mfgunlock::adaptivequalityv2
