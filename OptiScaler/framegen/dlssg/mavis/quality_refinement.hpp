/* SPDX-License-Identifier: MIT
 * Shared PTX fragment used by the offline builder and exact-profile runtime
 * rewrite. Experimental; does not modify bounds, invalid sentinels or tags.
 * Validated blending follows Tony Joaca's published qualityValidWarp research;
 * this confidence ramp is independently implemented in this fork.
 */
#pragma once
namespace mfgunlock::qualityrefinement {
inline constexpr const char* kBlendWeights = R"PTX(
// MFGUNLOCK_SMOOTH_CONFIDENCE_V1
add.sat.f32 %qf8, %f148, 0f00000000;
add.sat.f32 %qf10, %f149, 0f00000000;
// Agreement evidence: min of correlated margins, never a product of penalties.
sub.f32 %qf2, %qf6, %qf9;
sub.f32 %qf2, %qf2, 0f3DA3D70A;
mul.sat.f32 %qf2, %qf2, 0f40D55555;
sub.f32 %qf3, 0f3E19999A, %qf9;
mul.sat.f32 %qf3, %qf3, 0f40D55555;
min.f32 %qf2, %qf2, %qf3;
@!%qv3 mov.f32 %qf2, 0f00000000;
// Native evidence: retain each candidate's own confidence independently.
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
// MFGUNLOCK_FULL_CONFIDENCE_FAST_PATH_V1
// Saturated confidence leaves the already-boosted weights unchanged. This is
// the common interior case, so avoid two smoothsteps and two reconstructive
// FMAs. Mixed-confidence warps continue through the original arithmetic.
setp.ge.f32 %qv5, %qf4, 0f3F800000;
setp.ge.f32 %qv6, %qf5, 0f3F800000;
and.pred %qv2, %qv5, %qv6;
@%qv2 bra MFGUNLOCK_SMOOTH_CONFIDENCE_DONE_V1;
// Smoothstep has zero slope at the acceptance boundary.
fma.rn.f32 %qf7, %qf4, 0fC0000000, 0f40400000;
mul.f32 %qf4, %qf4, %qf4;
mul.f32 %qf4, %qf4, %qf7;
fma.rn.f32 %qf7, %qf5, 0fC0000000, 0f40400000;
mul.f32 %qf5, %qf5, %qf5;
mul.f32 %qf5, %qf5, %qf7;
sub.f32 %qf0, %qf0, %qf8;
sub.f32 %qf1, %qf1, %qf10;
fma.rn.f32 %qf0, %qf4, %qf0, %qf8;
fma.rn.f32 %qf1, %qf5, %qf1, %qf10;
MFGUNLOCK_SMOOTH_CONFIDENCE_DONE_V1:
)PTX";
}
