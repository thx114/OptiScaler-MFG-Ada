// Upstream tests adapted to the vendored MIT headers; see mavis/UPSTREAM.md.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include "../../OptiScaler/framegen/dlssg/mavis/adaptive_quality.hpp"
#include "../../OptiScaler/framegen/dlssg/mavis/adaptive_quality_v2.hpp"
#include "../../OptiScaler/framegen/dlssg/mavis/adaptive_quality_v3.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                        \
      return EXIT_FAILURE;                                                      \
    }                                                                           \
  } while (false)

constexpr uint64_t Fnv1a64(const char* text) {
  uint64_t hash = UINT64_C(14695981039346656037);
  while (*text != '\0') {
    hash ^= static_cast<unsigned char>(*text++);
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

int main() {
  namespace aq = mfgunlock::adaptivequality;
  namespace aq2 = mfgunlock::adaptivequalityv2;
  namespace aq3 = mfgunlock::adaptivequalityv3;

  // Release-candidate payload fingerprints. These intentionally freeze the
  // user-validated endpoint fast paths (14-register synthetic V2 allocation)
  // so later UI/latency work cannot silently change visual PTX.
  CHECK(Fnv1a64(aq2::kSmoothWarpConfidence) ==
        UINT64_C(0x66abc7e3a697a65f));
  CHECK(Fnv1a64(aq2::kBorderConfidence) ==
        UINT64_C(0x2adb5a0dd7cdf2e2));
  CHECK(Fnv1a64(aq2::kCandidateArbitration) ==
        UINT64_C(0xb0e724b196e70779));

  CHECK(aq::NormalizeProfile(0) == aq::Profile::kStableV1);
  CHECK(aq::kDefaultProfile == aq::Profile::kLuminanceDirectionalV3);
  CHECK(aq::NormalizeProfile(1) == aq::Profile::kStableV1);
  CHECK(aq::NormalizeProfile(2) == aq::Profile::kFlickerReducedV2);
  CHECK(aq::NormalizeProfile(3) == aq::Profile::kLuminanceDirectionalV3);
  CHECK(aq::NormalizeProfile(999) == aq::Profile::kStableV1);
  CHECK(aq::SelectComponentVersion(aq::Profile::kStableV1, true, true,
                                   true) ==
        aq::ComponentVersion::kV1);
  CHECK(aq::SelectComponentVersion(aq::Profile::kStableV1, true, true,
                                   false) ==
        aq::ComponentVersion::kNative);
  CHECK(aq::SelectComponentVersion(aq::Profile::kFlickerReducedV2, true,
                                   true, true) == aq::ComponentVersion::kV2);
  CHECK(aq::SelectComponentVersion(aq::Profile::kFlickerReducedV2, false,
                                   false, true) == aq::ComponentVersion::kV1);
  CHECK(aq::SelectComponentVersion(aq::Profile::kLuminanceDirectionalV3,
                                   true, true, true) ==
        aq::ComponentVersion::kV3);
  CHECK(aq::SelectComponentVersion(aq::Profile::kLuminanceDirectionalV3,
                                   false, true, true) ==
        aq::ComponentVersion::kV2);
  CHECK(aq::SelectComponentVersion(aq::Profile::kLuminanceDirectionalV3,
                                   false, false, true) ==
        aq::ComponentVersion::kV1);
  CHECK(aq::SelectComponentVersion(aq::Profile::kLuminanceDirectionalV3,
                                   false, false, false) ==
        aq::ComponentVersion::kNative);
  for (const aq::Profile profile : {aq::Profile::kStableV1,
                                    aq::Profile::kFlickerReducedV2,
                                    aq::Profile::kLuminanceDirectionalV3}) {
    for (unsigned mask = 0; mask != 8; ++mask) {
      const bool v3_available = (mask & 4u) != 0;
      const bool v2_available = (mask & 2u) != 0;
      const bool v1_available = (mask & 1u) != 0;
      const auto expected =
          profile == aq::Profile::kLuminanceDirectionalV3 && v3_available
              ? aq::ComponentVersion::kV3
              : profile != aq::Profile::kStableV1 && v2_available
                    ? aq::ComponentVersion::kV2
                    : v1_available ? aq::ComponentVersion::kV1
                                   : aq::ComponentVersion::kNative;
      CHECK(aq::SelectComponentVersion(profile, v3_available, v2_available,
                                       v1_available) == expected);
    }
  }
  CHECK(aq::ExpectedComponentVersion(
            aq::Profile::kLuminanceDirectionalV3,
            aq::Component::kWarp) == aq::ComponentVersion::kV3);
  CHECK(aq::ExpectedComponentVersion(
            aq::Profile::kLuminanceDirectionalV3,
            aq::Component::kGeometry) == aq::ComponentVersion::kV3);
  CHECK(aq::ExpectedComponentVersion(
            aq::Profile::kLuminanceDirectionalV3,
            aq::Component::kInpaint) == aq::ComponentVersion::kV2);
  CHECK(aq::MergeComponentVersions(aq::ComponentVersion::kNative,
                                   aq::ComponentVersion::kV2) ==
        aq::ComponentVersion::kV2);
  CHECK(aq::MergeComponentVersions(aq::ComponentVersion::kV2,
                                   aq::ComponentVersion::kV1) ==
        aq::ComponentVersion::kMixed);
  CHECK(aq::MergeComponentVersions(aq::ComponentVersion::kV3,
                                   aq::ComponentVersion::kV3) ==
        aq::ComponentVersion::kV3);

  constexpr float kNative = 0.2f;
  constexpr float kV1Target = 0.85f;
  CHECK(aq2::EffectiveWarpWeight(kNative, kV1Target, 0.0f) == 0.0f);
  CHECK(std::abs(aq2::EffectiveWarpWeight(kNative, kV1Target, 1.0f) -
                 kV1Target) < 0.00001f);
  float previous = 0.0f;
  for (int index = 0; index <= 1000; ++index) {
    const float confidence = static_cast<float>(index) / 1000.0f;
    const float weight =
        aq2::EffectiveWarpWeight(kNative, kV1Target, confidence);
    CHECK(std::isfinite(weight));
    CHECK(weight >= 0.0f && weight <= kV1Target);
    CHECK(weight + 0.000001f >= previous);
    previous = weight;
  }
  const float before = aq2::EffectiveWarpWeight(kNative, kV1Target, 0.4999f);
  const float after = aq2::EffectiveWarpWeight(kNative, kV1Target, 0.5001f);
  CHECK(after >= before);
  CHECK(after - before < 0.001f);

  CHECK(aq2::GeometryRelaxation(0.0f, 0.0f) == 0.0f);
  CHECK(std::abs(aq2::GeometryRelaxation(1.0f, 0.0f) - 0.25f) <
        0.00001f);
  CHECK(std::abs(aq2::GeometryRelaxation(1.0f, 1.0f) - 0.5f) <
        0.00001f);
  float prior_relaxation = 0.0f;
  for (int index = 0; index <= 1000; ++index) {
    const float second = static_cast<float>(index) / 1000.0f;
    const float relaxation = aq2::GeometryRelaxation(1.0f, second);
    CHECK(relaxation >= 0.25f && relaxation <= 0.5f);
    CHECK(relaxation + 0.000001f >= prior_relaxation);
    prior_relaxation = relaxation;
  }

  CHECK(!aq2::NeedsInpaintV2(-1.0f));
  CHECK(!aq2::NeedsInpaintV2(-0.0f));
  CHECK(!aq2::NeedsInpaintV2(0.0f));
  CHECK(aq2::NeedsInpaintV2(0.0001f));
  CHECK(aq2::NeedsInpaintV2(std::numeric_limits<float>::infinity()));
  CHECK(aq2::NeedsInpaintV2(-std::numeric_limits<float>::infinity()));
  CHECK(aq2::NeedsInpaintV2(std::numeric_limits<float>::quiet_NaN()));

  const aq3::Rgb first{0.35f, 0.6f, 1.2f};
  const aq3::Rgb second{0.4f, 0.55f, 1.1f};
  const float relative_error = aq3::RelativePhotometricError(first, second);
  const float exposed_error = aq3::RelativePhotometricError(
      {first.r * 4.0f, first.g * 4.0f, first.b * 4.0f},
      {second.r * 4.0f, second.g * 4.0f, second.b * 4.0f});
  CHECK(std::isfinite(relative_error));
  CHECK(std::abs(relative_error - exposed_error) < 0.00001f);

  const float chroma_only = aq3::RelativePhotometricError(
      {0.5f, 0.5f, 0.5f}, {0.6f, 0.5f, 0.20557064f});
  CHECK(chroma_only > 0.1f);
  CHECK(std::isfinite(aq3::RelativePhotometricError(
      {0.0f, 0.0f, 0.0f}, {0.001f, -0.001f, 0.002f})));
  CHECK(std::isfinite(aq3::RelativePhotometricError(
      {-4.0f, 2.0f, 8.0f}, {-3.5f, 2.2f, 7.7f})));
  CHECK(std::isfinite(aq3::RelativePhotometricError(
      {10000.0f, 4000.0f, 2000.0f}, {9800.0f, 4100.0f, 1900.0f})));
  CHECK(std::isinf(aq3::RelativePhotometricError(
      {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f},
      {0.0f, 0.0f, 0.0f})));
  CHECK(std::isinf(aq3::RelativePhotometricError(
      {std::numeric_limits<float>::infinity(), 0.0f, 0.0f},
      {0.0f, 0.0f, 0.0f})));

  CHECK(aq3::WarpConfidence(0.20f, 0.10f, 0.20f, true) == 0.0f);
  CHECK(aq3::WarpConfidence(0.80f, 0.04f, 0.20f, true) > 0.99f);
  CHECK(aq3::WarpConfidence(0.80f, 0.04f, 0.20f, false) == 0.0f);
  CHECK(aq3::WarpConfidence(0.80f, 0.04f, 0.50f, false) == 0.0f);
  CHECK(aq3::WarpConfidence(0.80f, 0.04f, 0.75f, false) > 0.0f);
  for (int index = 0; index <= 1000; ++index) {
    const float confidence = static_cast<float>(index) / 1000.0f;
    const float adapted =
        aq3::EffectiveWarpWeight(0.2f, 0.85f, confidence);
    CHECK(adapted >= 0.2f && adapted <= 0.85f);
    const float tapered = aq3::TaperAddedWeight(
        0.2f, adapted, 1.0f - confidence);
    CHECK(tapered >= 0.2f && tapered <= adapted);
  }
  CHECK(aq3::EffectiveWarpWeight(0.2f, 0.85f, 0.0f) == 0.2f);
  CHECK(aq3::EffectiveWarpWeight(0.2f, 0.85f, 1.0f) == 0.85f);
  CHECK(aq3::TaperAddedWeight(0.2f, 0.85f, 0.0f) == 0.2f);
  CHECK(aq3::TaperAddedWeight(0.2f, 0.85f, 1.0f) == 0.85f);
  const float v3_unchanged = aq3::ArbitrateExtraWeight(
      0.2f, 0.85f, 0.09f, 1.0f);
  const float v3_attenuated = aq3::ArbitrateExtraWeight(
      0.2f, 0.85f, 0.30f, 1.0f);
  CHECK(v3_unchanged > 0.849f);
  CHECK(v3_attenuated >= 0.2f && v3_attenuated < v3_unchanged);
  CHECK(aq3::ArbitrateExtraWeight(0.6f, 0.2f, 0.30f, 1.0f) ==
        0.6f);

  const float symmetric =
      aq3::SymmetricBorderDistance(0.1001f, 0.5f, 1000.0f, 1000.0f);
  const float stationary = aq3::DirectionalBorderDistance(
      0.1f, 0.5f, 0.1001f, 0.5f, 1000.0f, 1000.0f);
  CHECK(std::abs(symmetric - stationary) < 0.0001f);
  const float entering_left = aq3::DirectionalBorderDistance(
      0.2f, 0.5f, 0.1f, 0.5f, 1000.0f, 1000.0f);
  const float exiting_left = aq3::DirectionalBorderDistance(
      0.1f, 0.5f, 0.2f, 0.5f, 1000.0f, 1000.0f);
  const float entering_right = aq3::DirectionalBorderDistance(
      0.8f, 0.5f, 0.9f, 0.5f, 1000.0f, 1000.0f);
  const float exiting_right = aq3::DirectionalBorderDistance(
      0.9f, 0.5f, 0.8f, 0.5f, 1000.0f, 1000.0f);
  const float entering_top = aq3::DirectionalBorderDistance(
      0.5f, 0.2f, 0.5f, 0.1f, 1000.0f, 1000.0f);
  const float exiting_top = aq3::DirectionalBorderDistance(
      0.5f, 0.1f, 0.5f, 0.2f, 1000.0f, 1000.0f);
  const float entering_bottom = aq3::DirectionalBorderDistance(
      0.5f, 0.8f, 0.5f, 0.9f, 1000.0f, 1000.0f);
  const float exiting_bottom = aq3::DirectionalBorderDistance(
      0.5f, 0.9f, 0.5f, 0.8f, 1000.0f, 1000.0f);
  CHECK(entering_left > exiting_left);
  CHECK(entering_right > exiting_right);
  CHECK(entering_top > exiting_top);
  CHECK(entering_bottom > exiting_bottom);
  CHECK(entering_left <= exiting_left + 1.0001f);
  const float parallel_to_top = aq3::DirectionalBorderDistance(
      0.4f, 0.001f, 0.6f, 0.001f, 1000.0f, 1000.0f);
  const float diagonal_corner = aq3::DirectionalBorderDistance(
      0.002f, 0.002f, 0.004f, 0.004f, 1000.0f, 1000.0f);
  CHECK(parallel_to_top <= 1.0001f);
  CHECK(diagonal_corner <= 2.0001f);
  CHECK(aq3::BorderConfidence(0.5f) == 0.0f);
  CHECK(aq3::BorderConfidence(2.5f) == 1.0f);

  CHECK(std::abs(aq3::OrientationWeight(1.0f, 0.0f, 1.0f, 0.0f) -
                 1.0f) < 0.00001f);
  CHECK(std::abs(aq3::OrientationWeight(1.0f, 0.0f, 0.0f, 1.0f) -
                 0.875f) < 0.00001f);
  CHECK(aq3::OrientationWeight(0.0f, 0.0f, 1.0f, 0.0f) == 1.0f);
  CHECK(aq3::OrientationWeight(0.5f, 0.0f, 0.0f, 1.0f) == 1.0f);
  CHECK(std::abs(aq3::OrientationWeight(1.5f, 0.0f, 0.0f, 1.0f) -
                 0.75f) < 0.00001f);
  CHECK(aq3::DiagonalAmbiguityWeight(0.35f) == 1.0f);
  CHECK(aq3::DiagonalAmbiguityWeight(0.60f) == 0.0f);
  CHECK(aq3::DiagonalAmbiguityWeight(0.475f) > 0.49f);
  CHECK(aq3::DiagonalAmbiguityWeight(0.475f) < 0.51f);
  CHECK(aq3::DiagonalDirectionWeight(0.5f, 0.5f) == 0.0f);
  CHECK(aq3::DiagonalDirectionWeight(2.0f, 0.6f) == 0.0f);
  CHECK(aq3::DiagonalDirectionWeight(2.0f, 1.2f) > 0.99f);
  CHECK(std::abs(aq3::StableGeometryRelaxation(1.0f, 0.0f) - 0.125f) <
        0.00001f);
  CHECK(std::abs(aq3::StableGeometryRelaxation(1.0f, 1.0f) - 0.5f) <
        0.00001f);
  float prior_stable_relaxation = 0.0f;
  for (int index = 0; index <= 1000; ++index) {
    const float second = static_cast<float>(index) / 1000.0f;
    const float relaxation = aq3::StableGeometryRelaxation(1.0f, second);
    CHECK(relaxation + 0.000001f >= prior_stable_relaxation);
    prior_stable_relaxation = relaxation;
  }

  CHECK(std::string(aq3::kRelativePhotometricError).find("ld.") ==
        std::string::npos);
  CHECK(std::string(aq3::kSmoothWarpConfidence).find("ld.") ==
        std::string::npos);
  CHECK(std::string(aq3::kDirectionalBorderDistances).find("ld.") ==
        std::string::npos);
  CHECK(std::string(aq3::kDirectionalBorderConfidence).find("ld.") ==
        std::string::npos);
  CHECK(std::string(aq3::kCandidateArbitration).find("ld.") ==
        std::string::npos);

  std::cout << "adaptive quality V2/V3 tests passed\n";
  return EXIT_SUCCESS;
}
