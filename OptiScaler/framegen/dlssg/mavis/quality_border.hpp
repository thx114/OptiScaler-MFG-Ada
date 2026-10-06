/* SPDX-License-Identifier: MIT
 * Research-only border confidence for valid warped-color candidates.
 * Attenuates this addon's extra warp-weight boost, never NVIDIA's native weight.
 * The two-pixel transition covers the bilinear sample footprint past the
 * provider's half-pixel validity bound; it is not a copied screen percentage.
 */
#pragma once

namespace mfgunlock::qualityborder {
inline constexpr const char* kBorderWeights = R"PTX(
// MFGUNLOCK_BORDER_CONFIDENCE_V1
cvt.rn.f32.u32 %qf2, %r10;
cvt.rn.f32.u32 %qf3, %r11;
// Compute both exact pixel distances first. Almost every warp is at least 2.5
// pixels from the screen edge, where the original confidence evaluates to one
// and therefore leaves both weights unchanged.
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
// MFGUNLOCK_BORDER_INTERIOR_FAST_PATH_V1
setp.ge.f32 %qv5, %qf4, 0f40200000;
setp.ge.f32 %qv6, %qf5, 0f40200000;
and.pred %qv2, %qv5, %qv6;
@%qv2 bra MFGUNLOCK_BORDER_CONFIDENCE_DONE_V1;
// Only the narrow screen-edge region evaluates the two smooth confidence
// ramps. The formula is unchanged from V1.
sub.f32 %qf4, %qf4, 0f3F000000;
mul.sat.f32 %qf4, %qf4, 0f3F000000;
fma.rn.f32 %qf6, %qf4, 0fC0000000, 0f40400000;
mul.f32 %qf4, %qf4, %qf4;
mul.f32 %qf4, %qf4, %qf6;
sub.f32 %qf6, %qf0, %qf8;
fma.rn.f32 %qf0, %qf4, %qf6, %qf8;
sub.f32 %qf5, %qf5, 0f3F000000;
mul.sat.f32 %qf5, %qf5, 0f3F000000;
fma.rn.f32 %qf6, %qf5, 0fC0000000, 0f40400000;
mul.f32 %qf5, %qf5, %qf5;
mul.f32 %qf5, %qf5, %qf6;
sub.f32 %qf6, %qf1, %qf10;
fma.rn.f32 %qf1, %qf5, %qf6, %qf10;
MFGUNLOCK_BORDER_CONFIDENCE_DONE_V1:
)PTX";
} // namespace mfgunlock::qualityborder
