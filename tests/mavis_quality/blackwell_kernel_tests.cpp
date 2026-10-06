// Upstream tests adapted to the vendored MIT headers; see mavis/UPSTREAM.md.
#include <cstdlib>
#include <iostream>
#include <string>

#include "../../OptiScaler/framegen/dlssg/mavis/blackwell.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                        \
      return EXIT_FAILURE;                                                      \
    }                                                                           \
  } while (false)

int main() {
  using namespace mfgunlock::blackwell;

  CHECK(internal::RoleFromSharedMemory(7776) == KernelRole::MotionVector);
  CHECK(internal::RoleFromSharedMemory(3920) == KernelRole::Inpaint);
  CHECK(internal::RoleFromSharedMemory(784) == KernelRole::InpaintDecision);
  CHECK(internal::RoleFromSharedMemory(0) == KernelRole::Unknown);

#if MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS
  CHECK(HasGeneratedCubins());
  CHECK(generated::kCubinsBuiltFor[0] != '\0');
  for (const auto& replacement : generated::kCubinPatches) {
    CHECK(replacement.data != nullptr);
    CHECK(replacement.size != 0);
    CHECK(replacement.size <= replacement.orig_size);
    CHECK(internal::RoleFromSharedMemory(replacement.shared) != KernelRole::Unknown);
  }
#else
  CHECK(!HasGeneratedCubins());
#endif

#if MFGUNLOCK_HAS_GENERATED_THIN_GEOMETRY_CUBINS
  bool found_intermediate_scatter = false;
  bool found_silhouette_guard = false;
  bool found_aggressive_silhouette_guard = false;
  bool found_adaptive_geometry_v1 = false;
  bool found_adaptive_geometry_v2 = false;
  bool found_adaptive_geometry_v31_local = false;
  bool found_adaptive_geometry_v31_temporal = false;
  bool found_adaptive_inpaint_v1 = false;
  bool found_adaptive_inpaint_v2 = false;
  bool found_adaptive_inpaint_v3_local = false;
  bool found_adaptive_inpaint_v3_temporal = false;
  for (const auto& replacement : generated_thin_geometry::kThinGeometryCubins) {
    CHECK(replacement.data != nullptr);
    CHECK(replacement.size != 0);
    const std::string mechanism = replacement.mechanism;
    const bool redirected_geometry_v3 =
        (mechanism == "adaptive_quality_geometry_v31_local" ||
         mechanism == "adaptive_quality_geometry_v31_temporal") &&
        replacement.size > replacement.slot_size;
    const bool redirected_inpaint_v3 =
        (mechanism == "adaptive_inpaint_decision_v3_local" ||
         mechanism == "adaptive_inpaint_decision_v3_temporal") &&
        replacement.size > replacement.slot_size;
    CHECK(replacement.size <= replacement.slot_size ||
          redirected_geometry_v3 || redirected_inpaint_v3);
    CHECK(replacement.source_fnv1a64 != 0);
    CHECK(mechanism == "intermediate_scatter" ||
          mechanism == "geometry_motion" ||
          mechanism == "geometry_motion_depth" ||
          mechanism == "geometry_motion_depth_refined" ||
          mechanism == "geometry_support_smooth_v2" ||
          mechanism == "geometry_motion_depth_aggressive" ||
          mechanism == "adaptive_quality_geometry_v1" ||
          mechanism == "adaptive_quality_geometry_v2" ||
          mechanism == "adaptive_quality_geometry_v31_local" ||
          mechanism == "adaptive_quality_geometry_v31_temporal" ||
          mechanism == "adaptive_inpaint_decision_v1" ||
          mechanism == "adaptive_inpaint_decision_v2" ||
          mechanism == "adaptive_inpaint_decision_v3_local" ||
          mechanism == "adaptive_inpaint_decision_v3_temporal");
    found_intermediate_scatter |= mechanism == "intermediate_scatter";
    found_silhouette_guard |= mechanism == "geometry_motion_depth";
    found_aggressive_silhouette_guard |=
        mechanism == "geometry_motion_depth_aggressive";
    found_adaptive_geometry_v1 |=
        mechanism == "adaptive_quality_geometry_v1";
    found_adaptive_geometry_v2 |=
        mechanism == "adaptive_quality_geometry_v2";
    found_adaptive_geometry_v31_local |=
        mechanism == "adaptive_quality_geometry_v31_local";
    found_adaptive_geometry_v31_temporal |=
        mechanism == "adaptive_quality_geometry_v31_temporal";
    found_adaptive_inpaint_v1 |=
        mechanism == "adaptive_inpaint_decision_v1";
    found_adaptive_inpaint_v2 |=
        mechanism == "adaptive_inpaint_decision_v2";
    found_adaptive_inpaint_v3_local |=
        mechanism == "adaptive_inpaint_decision_v3_local";
    found_adaptive_inpaint_v3_temporal |=
        mechanism == "adaptive_inpaint_decision_v3_temporal";
    uint64_t expected_v1_hash = 0;
    if (mechanism == "intermediate_scatter")
      expected_v1_hash = 0xa1e796760d8efc27ull;
    else if (mechanism == "adaptive_quality_geometry_v1")
      expected_v1_hash = 0x999b9b3d8e59a36full;
    else if (mechanism == "geometry_support_smooth_v2")
      expected_v1_hash = 0x1c998cefe6163055ull;
    else if (mechanism == "geometry_motion_depth_refined")
      expected_v1_hash = 0xd6e2aaa1abe800cfull;
    else if (mechanism == "geometry_motion_depth")
      expected_v1_hash = 0x539df6677e883dcdull;
    else if (mechanism == "geometry_motion_depth_aggressive")
      expected_v1_hash = 0xf41e0078e5f06bf8ull;
    else if (mechanism == "adaptive_inpaint_decision_v1")
      expected_v1_hash = 0xf46ec332a9bc97f9ull;
    if (expected_v1_hash != 0) {
      const uint64_t actual_hash =
          internal::Fnv1a64(replacement.data, replacement.size);
      if (actual_hash != expected_v1_hash) {
        std::cerr << "compiled hash mismatch for " << mechanism << ": 0x"
                  << std::hex << actual_hash << " expected 0x"
                  << expected_v1_hash << std::dec << '\n';
      }
      CHECK(actual_hash == expected_v1_hash);
    }
    if (mechanism == "adaptive_quality_geometry_v1" ||
        mechanism == "adaptive_quality_geometry_v2" ||
        mechanism == "adaptive_quality_geometry_v31_local" ||
        mechanism == "adaptive_quality_geometry_v31_temporal" ||
        mechanism == "adaptive_inpaint_decision_v1" ||
        mechanism == "adaptive_inpaint_decision_v2" ||
        mechanism == "adaptive_inpaint_decision_v3_local" ||
        mechanism == "adaptive_inpaint_decision_v3_temporal") {
      internal::ElfFingerprint compiled{};
      CHECK(internal::FingerprintElf(replacement.data, replacement.size,
                                     compiled));
      CHECK(compiled.shared == replacement.source_shared);
      if (redirected_geometry_v3) {
        if (mechanism == "adaptive_quality_geometry_v31_local")
          CHECK(compiled.text <= 39552u);
        CHECK(compiled.registers == 40u);
      } else if (redirected_inpaint_v3) {
        CHECK(compiled.shared == 784u);
        CHECK(compiled.registers <= 48u);
      } else {
        CHECK(compiled.text <= replacement.source_text);
        CHECK(compiled.registers <=
              (mechanism.find("geometry") != std::string::npos ? 40u : 48u));
      }
    }
  }
  CHECK(found_intermediate_scatter);
  CHECK(found_silhouette_guard);
  CHECK(found_aggressive_silhouette_guard);
  CHECK(found_adaptive_geometry_v1);
  CHECK(found_adaptive_geometry_v2);
  // V3 is generated only when the offline ptxas artifact satisfies the strict
  // slot/register gate. Runtime fallback to V2 is valid when it is absent.
  CHECK(!found_adaptive_geometry_v31_temporal ||
        found_adaptive_geometry_v31_local);
  CHECK(!found_adaptive_geometry_v31_local || found_adaptive_geometry_v2);
  CHECK(found_adaptive_inpaint_v1);
  CHECK(found_adaptive_inpaint_v2);
  CHECK(!found_adaptive_inpaint_v3_temporal ||
        found_adaptive_inpaint_v3_local);
  CHECK(!found_adaptive_inpaint_v3_local || found_adaptive_inpaint_v2);

  // Oversized geometry V3 is installed by redirecting exact fatbin descriptor
  // references, never by writing beyond the provider's original cubin slot.
  {
    constexpr size_t kImageSize = 0x4000;
    auto* image = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, kImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(image != nullptr);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x100;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.NumberOfSections = 2;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = kImageSize;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    sections[0].VirtualAddress = 0x1000;
    sections[0].Misc.VirtualSize = 0x1000;
    sections[0].Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
    sections[1].VirtualAddress = 0x3000;
    sections[1].Misc.VirtualSize = 0x100;
    sections[1].Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

    auto* fatbin = image + sections[0].VirtualAddress;
    constexpr size_t kOriginalPayload = 16;
    constexpr size_t kEntryHeader = 64;
    constexpr size_t kOriginalFatbin = 16 + kEntryHeader + kOriginalPayload;
    const uint32_t magic = internal::kFatbinMagic;
    const uint16_t outer_header = 16;
    const uint64_t outer_size = kOriginalFatbin - 16;
    std::memcpy(fatbin, &magic, sizeof(magic));
    std::memcpy(fatbin + 6, &outer_header, sizeof(outer_header));
    std::memcpy(fatbin + 8, &outer_size, sizeof(outer_size));
    auto* entry = fatbin + 16;
    const uint16_t cubin_kind = 2;
    const uint32_t entry_header = kEntryHeader;
    const uint64_t original_payload = kOriginalPayload;
    std::memcpy(entry, &cubin_kind, sizeof(cubin_kind));
    std::memcpy(entry + 4, &entry_header, sizeof(entry_header));
    std::memcpy(entry + 8, &original_payload, sizeof(original_payload));
    for (size_t index = 0; index < kOriginalPayload; ++index)
      entry[kEntryHeader + index] = static_cast<uint8_t>(0x80u + index);

    auto* descriptor = reinterpret_cast<uint64_t*>(
        image + sections[1].VirtualAddress);
    *descriptor = reinterpret_cast<uint64_t>(fatbin);
    const uint8_t replacement_bytes[24] = {
        0x7f, 0x45, 0x4c, 0x46, 1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    const generated_thin_geometry::CubinVariant replacement{
        0, 7776, 51, kOriginalPayload, 1, sizeof(replacement_bytes),
        replacement_bytes, "adaptive_quality_geometry_v31_temporal"};
    internal::Candidate candidate{fatbin, kOriginalFatbin, 16,
                                  entry + kEntryHeader, kOriginalPayload,
                                  nullptr, KernelRole::MotionVector};
    std::vector<Patch> redirect_patches;
    std::vector<void*> redirect_allocations;
    std::string redirect_detail;
    CHECK(internal::RedirectOversizedCubin(
        reinterpret_cast<HMODULE>(image), candidate, replacement,
        redirect_patches, redirect_allocations, redirect_detail));
    CHECK(redirect_patches.size() == 1);
    CHECK(redirect_allocations.size() == 1);
    CHECK(*descriptor != reinterpret_cast<uint64_t>(fatbin));
    const auto* redirected = reinterpret_cast<const uint8_t*>(*descriptor);
    uint64_t redirected_outer = 0;
    uint64_t redirected_payload = 0;
    std::memcpy(&redirected_outer, redirected + 8, sizeof(redirected_outer));
    std::memcpy(&redirected_payload, redirected + 16 + 8,
                sizeof(redirected_payload));
    CHECK(redirected_outer == kOriginalFatbin - 16 - kOriginalPayload +
                                  sizeof(replacement_bytes));
    CHECK(redirected_payload == sizeof(replacement_bytes));
    CHECK(std::memcmp(redirected + 16 + kEntryHeader,
                      replacement_bytes, sizeof(replacement_bytes)) == 0);
    Restore(redirect_patches, redirect_allocations);
    CHECK(*descriptor == reinterpret_cast<uint64_t>(fatbin));
    CHECK(redirect_patches.empty());
    CHECK(redirect_allocations.empty());
    CHECK(VirtualFree(image, 0, MEM_RELEASE) != 0);
  }
#endif

  const mfgunlock::blackwell::Result defaults;
  CHECK(!defaults.silhouette_guard_requested);
  CHECK(!defaults.silhouette_guard);
  CHECK(!defaults.silhouette_guard_fallback);
  CHECK(defaults.silhouette_guard_mode_requested ==
        mfgunlock::blackwell::SilhouetteGuardMode::Off);
  CHECK(defaults.silhouette_guard_mode_selected ==
        mfgunlock::blackwell::SilhouetteGuardMode::Off);
  CHECK(std::string(mfgunlock::blackwell::SilhouetteGuardMechanism(
            mfgunlock::blackwell::SilhouetteGuardMode::Balanced)) ==
        "geometry_motion_depth");
  CHECK(std::string(mfgunlock::blackwell::SilhouetteGuardMechanism(
            mfgunlock::blackwell::SilhouetteGuardMode::Aggressive)) ==
        "geometry_motion_depth_aggressive");
  g_refinement_enabled = true;
  CHECK(std::string(SilhouetteGuardMechanism(SilhouetteGuardMode::Balanced)) ==
        "geometry_motion_depth_refined");
  g_geometry_confidence_v2_enabled = true;
  CHECK(std::string(SilhouetteGuardMechanism(SilhouetteGuardMode::Balanced)) ==
        "geometry_support_smooth_v2");
  CHECK(std::string(SilhouetteGuardMechanism(SilhouetteGuardMode::Aggressive)) ==
        "geometry_motion_depth_aggressive");
  g_geometry_confidence_v2_enabled = false;
  g_refinement_enabled = false;
  g_adaptive_quality_enabled = true;
  g_adaptive_quality_profile =
      mfgunlock::adaptivequality::Profile::kStableV1;
  CHECK(std::string(SilhouetteGuardMechanism(SilhouetteGuardMode::Balanced)) ==
        "adaptive_quality_geometry_v1");
  g_adaptive_quality_profile =
      mfgunlock::adaptivequality::Profile::kFlickerReducedV2;
  CHECK(std::string(SilhouetteGuardMechanism(SilhouetteGuardMode::Balanced)) ==
        "adaptive_quality_geometry_v2");
  g_adaptive_quality_profile =
      mfgunlock::adaptivequality::Profile::kLuminanceDirectionalV3;
  g_adaptive_quality_v3_oriented_geometry = true;
  g_adaptive_quality_v3_temporal_geometry = true;
  CHECK(std::string(SilhouetteGuardMechanism(SilhouetteGuardMode::Balanced)) ==
        "adaptive_quality_geometry_v31_temporal");
  g_adaptive_quality_v3_temporal_geometry = false;
  CHECK(std::string(SilhouetteGuardMechanism(SilhouetteGuardMode::Balanced)) ==
        "adaptive_quality_geometry_v31_local");
  g_adaptive_quality_v3_oriented_geometry = false;
  CHECK(std::string(SilhouetteGuardMechanism(SilhouetteGuardMode::Balanced)) ==
        "adaptive_quality_geometry_v2");
  g_adaptive_quality_v3_oriented_geometry = true;
  g_adaptive_quality_enabled = false;
  g_adaptive_quality_profile =
      mfgunlock::adaptivequality::Profile::kStableV1;

  std::cout << "blackwell kernel tests passed\n";
  return EXIT_SUCCESS;
}
