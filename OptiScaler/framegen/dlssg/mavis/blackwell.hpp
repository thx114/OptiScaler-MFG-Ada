/*
 * Blackwell framework kernels rebuilt for Ada.
 *
 * The implementation follows the fail-closed method validated by MatiasLombo:
 * identify NVIDIA's original sm_89 cubins by their ELF fingerprint and exact
 * fatbin slot size, then replace only the payload in that existing slot. A
 * deliberately oversized, zero-spill geometry V3 research payload uses a
 * bounded copy of the exact fatbin and redirects its descriptor references;
 * it is never written beyond the original slot. Registration metadata and the
 * surrounding provider image remain untouched.
 *
 * Replacement cubins are generated locally from installed NVIDIA DLSS-G
 * providers with MatiasLombo's rebuild_cubins.py workflow. They are deliberately
 * excluded from source control. A source-only build simply reports that no
 * table is present and lets the addon use its stable midpoint fallback.
 */

#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "adaptive_quality.hpp"
#include "quality_build_policy.hpp"

#if __has_include("./blackwell_cubins.generated.hpp")
namespace mfgunlock::blackwell::generated {
#include "./blackwell_cubins.generated.hpp"
}
#define MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS 1
#else
#define MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS 0
#endif

#if __has_include("./thin_geometry_cubins.generated.hpp")
namespace mfgunlock::blackwell::generated_thin_geometry {
#include "./thin_geometry_cubins.generated.hpp"
}
#define MFGUNLOCK_HAS_GENERATED_THIN_GEOMETRY_CUBINS 1
#else
#define MFGUNLOCK_HAS_GENERATED_THIN_GEOMETRY_CUBINS 0
#endif

namespace mfgunlock::blackwell {
inline bool g_refinement_enabled = false; // immutable after startup
inline bool g_geometry_confidence_v2_enabled = false; // research-only, startup-scoped
inline bool g_adaptive_quality_enabled = false; // unified research suite, startup-scoped
inline adaptivequality::Profile g_adaptive_quality_profile =
    adaptivequality::Profile::kStableV1;
inline bool g_adaptive_quality_v3_oriented_geometry = true;
inline bool g_adaptive_quality_v3_temporal_geometry =
    qualitybuild::kConfidenceHistoryEnabled;
// Mirrors cudatemporal::kDefaultInpaintMode without coupling the cubin patcher
// to the CUDA interception header.
inline unsigned int g_adaptive_quality_v3_inpaint_mode =
    qualitybuild::InpaintMode(2);

enum class AdaptiveGeometryVariant : unsigned int {
  kNone = 0,
  kLocal = 1,
  kTemporal = 2,
  kMixed = 3,
};

inline constexpr const char* AdaptiveGeometryVariantName(
    AdaptiveGeometryVariant variant) {
  switch (variant) {
    case AdaptiveGeometryVariant::kLocal: return "Local";
    case AdaptiveGeometryVariant::kTemporal: return "Temporal";
    case AdaptiveGeometryVariant::kMixed: return "mixed";
    default: return "fallback/native";
  }
}

enum class AdaptiveInpaintVariant : unsigned int {
  kNone = 0,
  kV2Compatibility = 1,
  kLocal = 2,
  kTemporal = 3,
  kMixed = 4,
};

inline constexpr const char* AdaptiveInpaintVariantName(
    AdaptiveInpaintVariant variant) {
  switch (variant) {
    case AdaptiveInpaintVariant::kV2Compatibility: return "V2 Compatibility";
    case AdaptiveInpaintVariant::kLocal: return "Local V3";
    case AdaptiveInpaintVariant::kTemporal: return "Temporal V3";
    case AdaptiveInpaintVariant::kMixed: return "mixed";
    default: return "fallback/native";
  }
}

inline const char* AdaptiveInpaintMechanism() {
  const auto mode = qualitybuild::InpaintMode(g_adaptive_quality_v3_inpaint_mode);
  if (g_adaptive_quality_profile != adaptivequality::Profile::kLuminanceDirectionalV3 ||
      mode == 0) {
    return g_adaptive_quality_profile == adaptivequality::Profile::kStableV1
               ? "adaptive_inpaint_decision_v1"
               : "adaptive_inpaint_decision_v2";
  }
  return mode == 2 ? "adaptive_inpaint_decision_v3_temporal"
                   : "adaptive_inpaint_decision_v3_local";
}

enum class KernelRole {
  Unknown,
  MotionVector,
  Inpaint,
  InpaintDecision,
};

enum class SilhouetteGuardMode : unsigned int {
  Off = 0,
  Balanced = 1,
  Aggressive = 2,
};

inline const char* SilhouetteGuardMechanism(
    SilhouetteGuardMode mode) {
  switch (mode) {
    case SilhouetteGuardMode::Balanced:
      if (g_adaptive_quality_enabled) {
        if (g_adaptive_quality_profile ==
                adaptivequality::Profile::kLuminanceDirectionalV3 &&
            g_adaptive_quality_v3_oriented_geometry)
          return qualitybuild::kConfidenceHistoryEnabled &&
                         g_adaptive_quality_v3_temporal_geometry
                     ? "adaptive_quality_geometry_v31_temporal"
                     : "adaptive_quality_geometry_v31_local";
        if (g_adaptive_quality_profile !=
            adaptivequality::Profile::kStableV1)
          return "adaptive_quality_geometry_v2";
        return "adaptive_quality_geometry_v1";
      }
      return g_refinement_enabled
                 ? (g_geometry_confidence_v2_enabled
                        ? "geometry_support_smooth_v2"
                        : "geometry_motion_depth_refined")
                 : "geometry_motion_depth";
    case SilhouetteGuardMode::Aggressive:
      return "geometry_motion_depth_aggressive";
    default:
      return nullptr;
  }
}

inline constexpr const char* SilhouetteGuardName(SilhouetteGuardMode mode) {
  switch (mode) {
    case SilhouetteGuardMode::Balanced:
      return "balanced";
    case SilhouetteGuardMode::Aggressive:
      return "aggressive";
    default:
      return "off";
  }
}

inline const char* RoleName(KernelRole role) {
  switch (role) {
    case KernelRole::MotionVector: return "motion-vector estimate";
    case KernelRole::Inpaint: return "inpaint";
    case KernelRole::InpaintDecision: return "inpaint decision";
    default: return "unknown";
  }
}

struct Patch {
  uint8_t* payload = nullptr;
  std::vector<uint8_t> original;
};

struct Result {
  bool motion_vector = false;
  bool inpaint = false;
  bool inpaint_decision = false;
  bool intermediate_scatter_requested = false;
  bool intermediate_scatter = false;
  bool silhouette_guard_requested = false;
  bool silhouette_guard = false;
  bool silhouette_guard_fallback = false;
  bool refined_geometry = false;
  bool geometry_confidence_v2 = false;
  bool adaptive_quality_requested = false;
  bool adaptive_geometry = false;
  bool adaptive_geometry_redirected = false;
  adaptivequality::ComponentVersion adaptive_geometry_version =
      adaptivequality::ComponentVersion::kNative;
  AdaptiveGeometryVariant adaptive_geometry_variant =
      AdaptiveGeometryVariant::kNone;
  bool adaptive_inpaint_decision = false;
  adaptivequality::ComponentVersion adaptive_inpaint_version =
      adaptivequality::ComponentVersion::kNative;
  AdaptiveInpaintVariant adaptive_inpaint_variant =
      AdaptiveInpaintVariant::kNone;
  bool adaptive_inpaint_redirected = false;
  bool adaptive_directional_scatter = false;
  bool adaptive_fallback = false;
  SilhouetteGuardMode silhouette_guard_mode_requested =
      SilhouetteGuardMode::Off;
  SilhouetteGuardMode silhouette_guard_mode_selected =
      SilhouetteGuardMode::Off;
  size_t kernels = 0;
};

namespace internal {

constexpr uint32_t kFatbinMagic = 0xBA55ED50u;
constexpr uint32_t kAdaArch = 89u;
constexpr size_t kMaxFatbinSize = 4u * 1024u * 1024u;

inline uint16_t ReadU16(const uint8_t* p) {
  uint16_t value = 0;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

inline uint32_t ReadU32(const uint8_t* p) {
  uint32_t value = 0;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

inline uint64_t ReadU64(const uint8_t* p) {
  uint64_t value = 0;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

inline uint64_t Fnv1a64(const uint8_t* bytes, size_t size) {
  uint64_t value = 0xcbf29ce484222325ull;
  for (size_t index = 0; index < size; ++index) {
    value = (value ^ bytes[index]) * 0x100000001b3ull;
  }
  return value;
}

struct ElfFingerprint {
  uint32_t text = 0;
  uint32_t shared = 0;
  uint32_t registers = 0;
};

inline bool FingerprintElf(const uint8_t* bytes, size_t size, ElfFingerprint& out) {
  out = {};
  if (bytes == nullptr || size < 0x40 || bytes[0] != 0x7f || bytes[1] != 'E' ||
      bytes[2] != 'L' || bytes[3] != 'F') {
    return false;
  }

  const uint64_t section_offset = ReadU64(bytes + 0x28);
  const uint16_t section_entry_size = ReadU16(bytes + 0x3a);
  const uint16_t section_count = ReadU16(bytes + 0x3c);
  const uint16_t string_section = ReadU16(bytes + 0x3e);
  if (section_entry_size < 0x40 || section_count == 0 || string_section >= section_count ||
      section_offset > size ||
      static_cast<uint64_t>(section_entry_size) * section_count > size - section_offset) {
    return false;
  }

  const uint8_t* string_header =
      bytes + static_cast<size_t>(section_offset) + static_cast<size_t>(string_section) * section_entry_size;
  const uint64_t string_offset = ReadU64(string_header + 0x18);
  const uint64_t string_size = ReadU64(string_header + 0x20);
  if (string_offset >= size || string_size > size - string_offset) return false;

  for (uint16_t index = 0; index < section_count; ++index) {
    const uint8_t* section =
        bytes + static_cast<size_t>(section_offset) + static_cast<size_t>(index) * section_entry_size;
    const uint32_t name_offset = ReadU32(section);
    if (name_offset >= string_size) continue;

    const char* name = reinterpret_cast<const char*>(bytes + string_offset + name_offset);
    const size_t remaining = static_cast<size_t>(string_size - name_offset);
    const void* terminator = std::memchr(name, '\0', remaining);
    if (terminator == nullptr) continue;

    const uint64_t section_size = ReadU64(section + 0x20);
    const uint32_t info = ReadU32(section + 0x2c);
    if (std::strncmp(name, ".text.", 6) == 0) {
      if (section_size > UINT32_MAX) return false;
      out.text = static_cast<uint32_t>(section_size);
      out.registers = (info >> 24u) & 0xffu;
    } else if (std::strncmp(name, ".nv.shared", 10) == 0) {
      if (section_size > UINT32_MAX) return false;
      out.shared = static_cast<uint32_t>(section_size);
    }
  }
  return out.text != 0;
}

inline KernelRole RoleFromSharedMemory(uint32_t shared) {
  switch (shared) {
    case 7776u: return KernelRole::MotionVector;
    case 3920u: return KernelRole::Inpaint;
    case 784u: return KernelRole::InpaintDecision;
    default: return KernelRole::Unknown;
  }
}

#if MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS
inline const generated::CubinPatch* MatchReplacement(const ElfFingerprint& fingerprint,
                                                      size_t slot_size) {
  for (const auto& replacement : generated::kCubinPatches) {
    if (replacement.text == fingerprint.text && replacement.shared == fingerprint.shared &&
        replacement.regs == fingerprint.registers && replacement.orig_size == slot_size &&
        replacement.data != nullptr && replacement.size != 0 && replacement.size <= slot_size) {
      return &replacement;
    }
  }
  return nullptr;
}
#endif

#if MFGUNLOCK_HAS_GENERATED_THIN_GEOMETRY_CUBINS
inline const generated_thin_geometry::CubinVariant* MatchScatterVariant(
    const ElfFingerprint& fingerprint, const uint8_t* payload, size_t slot_size,
    const char* mechanism, bool require_in_place = true) {
  if (mechanism == nullptr) return nullptr;
  for (const auto& replacement : generated_thin_geometry::kThinGeometryCubins) {
    if (std::strcmp(replacement.mechanism, mechanism) != 0) continue;
    if (replacement.source_text == fingerprint.text &&
        replacement.source_shared == fingerprint.shared &&
        replacement.source_regs == fingerprint.registers &&
        replacement.slot_size == slot_size &&
        replacement.source_fnv1a64 == Fnv1a64(payload, slot_size) &&
        replacement.data != nullptr && replacement.size != 0 &&
        (!require_in_place || replacement.size <= slot_size)) {
      return &replacement;
    }
  }
  return nullptr;
}
#endif

struct Candidate {
  uint8_t* fatbin = nullptr;
  size_t fatbin_size = 0;
  size_t entry_offset = 0;
  uint8_t* payload = nullptr;
  size_t slot_size = 0;
#if MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS
  const generated::CubinPatch* replacement = nullptr;
#endif
  KernelRole role = KernelRole::Unknown;
};

inline bool CollectCandidates(HMODULE module, std::vector<Candidate>& candidates, std::string& why) {
#if !MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS
  (void)module;
  (void)candidates;
  why = "this build contains no locally generated Blackwell cubin table";
  return false;
#else
  auto* base = reinterpret_cast<uint8_t*>(module);
  if (base == nullptr) {
    why = "provider module is null";
    return false;
  }

  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
    why = "provider has no valid DOS header";
    return false;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    why = "provider has no valid PE header";
    return false;
  }

  const size_t image_size = nt->OptionalHeader.SizeOfImage;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (uint16_t section_index = 0; section_index < nt->FileHeader.NumberOfSections;
       ++section_index) {
    if ((section[section_index].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) continue;
    const size_t section_offset = section[section_index].VirtualAddress;
    const size_t section_size = section[section_index].Misc.VirtualSize;
    if (section_offset >= image_size || section_size > image_size - section_offset) continue;

    uint8_t* bytes = base + section_offset;
    for (size_t offset = 0; offset + 16 <= section_size; ++offset) {
      if (ReadU32(bytes + offset) != kFatbinMagic) continue;

      const uint16_t header_size = ReadU16(bytes + offset + 6);
      const uint64_t fatbin_size = ReadU64(bytes + offset + 8);
      if (header_size != 16 || fatbin_size == 0 || fatbin_size > kMaxFatbinSize ||
          fatbin_size > section_size - offset - 16) {
        continue;
      }

      size_t entry_offset = offset + 16;
      const size_t fatbin_end = entry_offset + static_cast<size_t>(fatbin_size);
      while (entry_offset + 32 <= fatbin_end) {
        const uint8_t* entry = bytes + entry_offset;
        const uint16_t kind = ReadU16(entry);
        const uint32_t entry_header_size = ReadU32(entry + 4);
        const uint64_t payload_size = ReadU64(entry + 8);
        const uint64_t compressed_size = ReadU64(entry + 16);
        const uint32_t arch = ReadU32(entry + 28);
        if (entry_header_size < 64 || entry_header_size > 256 ||
            entry_header_size > fatbin_end - entry_offset ||
            payload_size > fatbin_end - entry_offset - entry_header_size) {
          break;
        }

        if (kind == 2 && arch == kAdaArch && compressed_size == 0 && payload_size > 0x40) {
          uint8_t* payload = bytes + entry_offset + entry_header_size;
          ElfFingerprint fingerprint;
          if (FingerprintElf(payload, static_cast<size_t>(payload_size), fingerprint)) {
            if (const auto* replacement =
                    MatchReplacement(fingerprint, static_cast<size_t>(payload_size))) {
              const KernelRole role = RoleFromSharedMemory(fingerprint.shared);
              if (role != KernelRole::Unknown &&
                  std::none_of(candidates.begin(), candidates.end(),
                               [payload](const Candidate& item) { return item.payload == payload; })) {
                candidates.push_back({bytes + offset,
                                      static_cast<size_t>(fatbin_size) + 16u,
                                      entry_offset - offset,
                                      payload,
                                      static_cast<size_t>(payload_size),
                                      replacement, role});
              }
            }
          }
        }
        entry_offset += entry_header_size + static_cast<size_t>(payload_size);
      }
      offset = fatbin_end - 1;
    }
  }

  if (candidates.empty()) {
    why = std::string("no exact Ada cubin slot matched the generated table for ") +
          generated::kCubinsBuiltFor;
    return false;
  }
  return true;
#endif
}

#if MFGUNLOCK_HAS_GENERATED_THIN_GEOMETRY_CUBINS
inline bool RedirectOversizedCubin(
    HMODULE module, const Candidate& candidate,
    const generated_thin_geometry::CubinVariant& replacement,
    std::vector<Patch>& patches, std::vector<void*>& allocations,
    std::string& why) {
  if (candidate.fatbin == nullptr || candidate.fatbin_size < 80u ||
      candidate.entry_offset < 16u ||
      candidate.entry_offset + 64u > candidate.fatbin_size) {
    why = "oversized V3 candidate has invalid fatbin bounds";
    return false;
  }
  uint8_t* entry = candidate.fatbin + candidate.entry_offset;
  const uint32_t header_size = ReadU32(entry + 4);
  const uint64_t payload_size64 = ReadU64(entry + 8);
  if (header_size < 64u || payload_size64 != candidate.slot_size ||
      candidate.entry_offset + header_size > candidate.fatbin_size ||
      payload_size64 > candidate.fatbin_size - candidate.entry_offset - header_size ||
      entry + header_size != candidate.payload) {
    why = "oversized V3 cubin entry no longer matches the exact candidate";
    return false;
  }
  const size_t padded = (static_cast<size_t>(replacement.size) + 7u) & ~size_t{7u};
  const size_t suffix_offset = candidate.entry_offset + header_size +
                               candidate.slot_size;
  if (padded < replacement.size ||
      padded > std::numeric_limits<size_t>::max() -
                   (candidate.fatbin_size - candidate.slot_size)) {
    why = "oversized V3 fatbin size overflow";
    return false;
  }
  const size_t rebuilt_size = candidate.fatbin_size - candidate.slot_size + padded;
  if (rebuilt_size > kMaxFatbinSize + 16u) {
    why = "oversized V3 rebuilt fatbin exceeds the bounded size";
    return false;
  }

  std::vector<uint8_t> rebuilt(candidate.fatbin,
                               candidate.fatbin + candidate.entry_offset + header_size);
  rebuilt.resize(rebuilt_size, 0);
  std::memcpy(rebuilt.data() + candidate.entry_offset + header_size,
              replacement.data, replacement.size);
  std::memcpy(rebuilt.data() + candidate.entry_offset + header_size + padded,
              candidate.fatbin + suffix_offset,
              candidate.fatbin_size - suffix_offset);
  const uint64_t new_payload_size = padded;
  const uint64_t new_outer_size = rebuilt_size - 16u;
  std::memcpy(rebuilt.data() + candidate.entry_offset + 8,
              &new_payload_size, sizeof(new_payload_size));
  std::memcpy(rebuilt.data() + 8, &new_outer_size, sizeof(new_outer_size));

  void* allocation = VirtualAlloc(nullptr, rebuilt.size(),
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (allocation == nullptr) {
    why = "oversized V3 replacement fatbin allocation failed";
    return false;
  }
  std::memcpy(allocation, rebuilt.data(), rebuilt.size());
  allocations.push_back(allocation);

  auto* base = reinterpret_cast<uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  const size_t image_size = nt->OptionalHeader.SizeOfImage;
  const uint64_t expected = reinterpret_cast<uint64_t>(candidate.fatbin);
  const uint64_t redirected = reinterpret_cast<uint64_t>(allocation);
  const auto* section = IMAGE_FIRST_SECTION(nt);
  size_t descriptor_count = 0;
  for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0 ||
        (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 ||
        section->VirtualAddress >= image_size) {
      continue;
    }
    const size_t section_size = std::min<size_t>(
        section->Misc.VirtualSize, image_size - section->VirtualAddress);
    uint8_t* bytes = base + section->VirtualAddress;
    for (size_t offset = 0; offset + sizeof(uint64_t) <= section_size;
         offset += alignof(uint64_t)) {
      uint64_t value = 0;
      std::memcpy(&value, bytes + offset, sizeof(value));
      if (value != expected) continue;
      auto* slot = bytes + offset;
      DWORD old_protection = 0;
      if (!VirtualProtect(slot, sizeof(uint64_t), PAGE_READWRITE,
                          &old_protection)) {
        continue;
      }
      Patch patch;
      patch.payload = slot;
      patch.original.assign(slot, slot + sizeof(uint64_t));
      std::memcpy(slot, &redirected, sizeof(redirected));
      DWORD ignored = 0;
      VirtualProtect(slot, sizeof(uint64_t), old_protection, &ignored);
      patches.push_back(std::move(patch));
      ++descriptor_count;
    }
  }
  if (descriptor_count == 0 || descriptor_count > 32) {
    std::ostringstream stream;
    stream << "oversized V3 descriptor reference count is "
           << descriptor_count << ", expected 1..32";
    why = stream.str();
    return false;
  }
  std::ostringstream stream;
  stream << "redirected " << descriptor_count
         << " exact descriptor reference(s) to a " << rebuilt_size
         << "-byte ptxas geometry V3 fatbin";
  why = stream.str();
  return true;
}
#endif

}  // namespace internal

inline constexpr bool HasGeneratedCubins() {
  return MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS != 0;
}

inline void Restore(std::vector<Patch>& patches, std::vector<void*>& allocations) {
  bool restored_all = true;
  for (auto patch = patches.rbegin(); patch != patches.rend(); ++patch) {
    if (patch->payload == nullptr || patch->original.empty()) continue;
    DWORD old_protection = 0;
    if (VirtualProtect(patch->payload, patch->original.size(), PAGE_READWRITE, &old_protection)) {
      std::memcpy(patch->payload, patch->original.data(), patch->original.size());
      DWORD ignored = 0;
      VirtualProtect(patch->payload, patch->original.size(), old_protection, &ignored);
    } else {
      restored_all = false;
    }
  }
  patches.clear();
  // Never free a redirected fatbin while a descriptor might still reference
  // it. A bounded leak on abnormal provider unload is safer than a dangling
  // CUDA registration pointer.
  if (restored_all) {
    for (void* allocation : allocations) {
      if (allocation != nullptr) VirtualFree(allocation, 0, MEM_RELEASE);
    }
  }
  allocations.clear();
}

inline bool Apply(HMODULE module, std::vector<Patch>& patches, std::vector<void*>& allocations,
                  Result& result, std::string& detail,
                  bool enable_intermediate_scatter = false,
                  SilhouetteGuardMode silhouette_guard_mode =
                      SilhouetteGuardMode::Off,
                  bool enable_adaptive_quality = false) {
  patches.clear();
  allocations.clear();
  result = {};
  result.intermediate_scatter_requested = enable_intermediate_scatter;
  result.silhouette_guard_requested =
      silhouette_guard_mode != SilhouetteGuardMode::Off;
  result.silhouette_guard_mode_requested = silhouette_guard_mode;
  result.adaptive_quality_requested = enable_adaptive_quality;
  detail.clear();

  std::vector<internal::Candidate> candidates;
  if (!internal::CollectCandidates(module, candidates, detail)) return false;

  const auto count_role = [&candidates](KernelRole role) {
    return std::count_if(candidates.begin(), candidates.end(),
                         [role](const internal::Candidate& item) { return item.role == role; });
  };
  const size_t motion_vectors = count_role(KernelRole::MotionVector);
  const size_t inpaints = count_role(KernelRole::Inpaint);
  const size_t decisions = count_role(KernelRole::InpaintDecision);
  if (motion_vectors != 1 || inpaints > 1 || decisions > 1) {
    std::ostringstream stream;
    stream << "ambiguous cubin set (motion-vector=" << motion_vectors << ", inpaint=" << inpaints
           << ", decision=" << decisions << ')';
    detail = stream.str();
    return false;
  }

#if MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS
  for (const auto& candidate : candidates) {
    const uint8_t* replacement_data = candidate.replacement->data;
    size_t replacement_size = candidate.replacement->size;
#if MFGUNLOCK_HAS_GENERATED_THIN_GEOMETRY_CUBINS
    const generated_thin_geometry::CubinVariant* redirect_variant = nullptr;
    if ((enable_intermediate_scatter || result.silhouette_guard_requested) &&
        candidate.role == KernelRole::MotionVector) {
      const auto fingerprint = internal::ElfFingerprint{
          candidate.replacement->text, candidate.replacement->shared,
          candidate.replacement->regs};
      const char* requested_mechanism = result.silhouette_guard_requested
                                            ? SilhouetteGuardMechanism(
                                                  silhouette_guard_mode)
                                            : "intermediate_scatter";
      const bool adaptive_geometry_requested = enable_adaptive_quality &&
          silhouette_guard_mode == SilhouetteGuardMode::Balanced;
      const bool full_v3_redirect_requested = adaptive_geometry_requested &&
          requested_mechanism != nullptr &&
          (std::strcmp(requested_mechanism,
                       "adaptive_quality_geometry_v31_local") == 0 ||
           std::strcmp(requested_mechanism,
                       "adaptive_quality_geometry_v31_temporal") == 0);
      const auto* experimental = internal::MatchScatterVariant(
          fingerprint, candidate.payload, candidate.slot_size,
          requested_mechanism, !full_v3_redirect_requested);
      const bool v2_requested = g_refinement_enabled &&
          g_geometry_confidence_v2_enabled &&
          silhouette_guard_mode == SilhouetteGuardMode::Balanced;
      const char* selected_mechanism = experimental != nullptr
                                           ? requested_mechanism
                                           : nullptr;
      if (experimental == nullptr && adaptive_geometry_requested &&
          requested_mechanism != nullptr &&
          std::strcmp(requested_mechanism,
                      "adaptive_quality_geometry_v31_temporal") == 0) {
        experimental = internal::MatchScatterVariant(
            fingerprint, candidate.payload, candidate.slot_size,
            "adaptive_quality_geometry_v31_local", false);
        if (experimental != nullptr) {
          selected_mechanism = "adaptive_quality_geometry_v31_local";
          result.silhouette_guard_fallback = true;
          result.adaptive_fallback = true;
        }
      }
      if (experimental == nullptr && adaptive_geometry_requested &&
          g_adaptive_quality_profile ==
              adaptivequality::Profile::kLuminanceDirectionalV3 &&
          g_adaptive_quality_v3_oriented_geometry) {
        experimental = internal::MatchScatterVariant(
            fingerprint, candidate.payload, candidate.slot_size,
            "adaptive_quality_geometry_v2");
        if (experimental != nullptr) {
          selected_mechanism = "adaptive_quality_geometry_v2";
          result.silhouette_guard_fallback = true;
          result.adaptive_fallback = true;
        }
      }
      if (experimental == nullptr && adaptive_geometry_requested) {
        experimental = internal::MatchScatterVariant(
            fingerprint, candidate.payload, candidate.slot_size,
            "adaptive_quality_geometry_v1");
        if (experimental != nullptr) {
          selected_mechanism = "adaptive_quality_geometry_v1";
          result.silhouette_guard_fallback = true;
          result.adaptive_fallback = true;
        }
      }
      if (experimental == nullptr && adaptive_geometry_requested)
        result.adaptive_fallback = true;
      if (experimental == nullptr && v2_requested) {
        experimental = internal::MatchScatterVariant(
            fingerprint, candidate.payload, candidate.slot_size,
            "geometry_motion_depth_refined");
        if (experimental != nullptr) {
          selected_mechanism = "geometry_motion_depth_refined";
          result.silhouette_guard_fallback = true;
        }
      }
      if (experimental == nullptr && g_refinement_enabled &&
          silhouette_guard_mode == SilhouetteGuardMode::Balanced) {
        experimental = internal::MatchScatterVariant(fingerprint, candidate.payload,
            candidate.slot_size, "geometry_motion_depth");
        result.silhouette_guard_fallback = experimental != nullptr;
        if (experimental != nullptr) selected_mechanism = "geometry_motion_depth";
      }
      if (experimental != nullptr) {
        result.adaptive_geometry = selected_mechanism != nullptr &&
            (std::strcmp(selected_mechanism,
                         "adaptive_quality_geometry_v1") == 0 ||
             std::strcmp(selected_mechanism,
                         "adaptive_quality_geometry_v2") == 0 ||
             std::strcmp(selected_mechanism,
                         "adaptive_quality_geometry_v31_local") == 0 ||
             std::strcmp(selected_mechanism,
                         "adaptive_quality_geometry_v31_temporal") == 0);
        if (result.adaptive_geometry) {
          const bool v31_local =
              std::strcmp(selected_mechanism,
                          "adaptive_quality_geometry_v31_local") == 0;
          const bool v31_temporal =
              std::strcmp(selected_mechanism,
                          "adaptive_quality_geometry_v31_temporal") == 0;
          result.adaptive_geometry_version =
              (v31_local || v31_temporal)
                  ? adaptivequality::ComponentVersion::kV3
                  : std::strcmp(selected_mechanism,
                                "adaptive_quality_geometry_v2") == 0
                        ? adaptivequality::ComponentVersion::kV2
                        : adaptivequality::ComponentVersion::kV1;
          result.adaptive_geometry_variant =
              v31_temporal ? AdaptiveGeometryVariant::kTemporal
                           : v31_local ? AdaptiveGeometryVariant::kLocal
                                       : AdaptiveGeometryVariant::kNone;
        }
        result.adaptive_directional_scatter = result.adaptive_geometry;
        result.geometry_confidence_v2 = selected_mechanism != nullptr &&
            (std::strcmp(selected_mechanism, "geometry_support_smooth_v2") == 0 ||
             result.adaptive_geometry);
        result.refined_geometry = result.geometry_confidence_v2 ||
            (selected_mechanism != nullptr &&
             std::strcmp(selected_mechanism, "geometry_motion_depth_refined") == 0);
        replacement_data = experimental->data;
        replacement_size = experimental->size;
        if (replacement_size > candidate.slot_size)
          redirect_variant = experimental;
        result.silhouette_guard = result.silhouette_guard_requested;
        result.silhouette_guard_mode_selected = silhouette_guard_mode;
        result.intermediate_scatter = !result.silhouette_guard_requested;
      } else if (silhouette_guard_mode == SilhouetteGuardMode::Aggressive) {
        // A build that predates the aggressive variant may still contain the
        // balanced guard. Prefer that guarded path over unconditional retention.
        experimental = internal::MatchScatterVariant(
            fingerprint, candidate.payload, candidate.slot_size,
            "geometry_motion_depth");
        if (experimental != nullptr) {
          replacement_data = experimental->data;
          replacement_size = experimental->size;
          result.silhouette_guard = true;
          result.silhouette_guard_fallback = true;
          result.silhouette_guard_mode_selected =
              SilhouetteGuardMode::Balanced;
        }
      }
      if (experimental == nullptr && result.silhouette_guard_requested &&
          enable_intermediate_scatter) {
        // The guard is a conditioned replacement for the released retention
        // kernel. If a locally generated table predates it, preserve the 0.9
        // behavior instead of silently dropping all added retention.
        experimental = internal::MatchScatterVariant(
            fingerprint, candidate.payload, candidate.slot_size,
            "intermediate_scatter");
        if (experimental != nullptr) {
          replacement_data = experimental->data;
          replacement_size = experimental->size;
          result.intermediate_scatter = true;
          result.silhouette_guard_fallback = true;
          result.silhouette_guard_mode_selected = SilhouetteGuardMode::Off;
        }
      }
    }
    if (enable_adaptive_quality &&
        candidate.role == KernelRole::InpaintDecision) {
      const auto fingerprint = internal::ElfFingerprint{
          candidate.replacement->text, candidate.replacement->shared,
          candidate.replacement->regs};
      const bool v3_profile =
          g_adaptive_quality_profile ==
          adaptivequality::Profile::kLuminanceDirectionalV3;
      const auto inpaint_mode =
          qualitybuild::InpaintMode(g_adaptive_quality_v3_inpaint_mode);
      const char* requested_inpaint = AdaptiveInpaintMechanism();
      const auto* adaptive_inpaint = internal::MatchScatterVariant(
          fingerprint, candidate.payload, candidate.slot_size,
          requested_inpaint, !v3_profile || inpaint_mode == 0);
      const char* selected_inpaint =
          adaptive_inpaint != nullptr ? requested_inpaint : nullptr;
      if (adaptive_inpaint == nullptr && v3_profile &&
          inpaint_mode == 2) {
        adaptive_inpaint = internal::MatchScatterVariant(
            fingerprint, candidate.payload, candidate.slot_size,
            "adaptive_inpaint_decision_v3_local", false);
        if (adaptive_inpaint != nullptr) {
          selected_inpaint = "adaptive_inpaint_decision_v3_local";
          result.adaptive_fallback = true;
        }
      }
      if (adaptive_inpaint == nullptr && v3_profile &&
          inpaint_mode != 0) {
        adaptive_inpaint = internal::MatchScatterVariant(
            fingerprint, candidate.payload, candidate.slot_size,
            "adaptive_inpaint_decision_v2");
        if (adaptive_inpaint != nullptr) {
          selected_inpaint = "adaptive_inpaint_decision_v2";
          result.adaptive_fallback = true;
        }
      }
      if (adaptive_inpaint == nullptr &&
          g_adaptive_quality_profile != adaptivequality::Profile::kStableV1) {
        adaptive_inpaint = internal::MatchScatterVariant(
            fingerprint, candidate.payload, candidate.slot_size,
            "adaptive_inpaint_decision_v1");
        if (adaptive_inpaint != nullptr) {
          selected_inpaint = "adaptive_inpaint_decision_v1";
          result.adaptive_fallback = true;
        }
      }
      if (adaptive_inpaint != nullptr) {
        replacement_data = adaptive_inpaint->data;
        replacement_size = adaptive_inpaint->size;
        if (replacement_size > candidate.slot_size)
          redirect_variant = adaptive_inpaint;
        result.adaptive_inpaint_decision = true;
        const bool temporal = std::strcmp(
            selected_inpaint, "adaptive_inpaint_decision_v3_temporal") == 0;
        const bool local = std::strcmp(
            selected_inpaint, "adaptive_inpaint_decision_v3_local") == 0;
        result.adaptive_inpaint_version = temporal || local
            ? adaptivequality::ComponentVersion::kV3
            : std::strcmp(selected_inpaint,
                          "adaptive_inpaint_decision_v2") == 0
                  ? adaptivequality::ComponentVersion::kV2
                  : adaptivequality::ComponentVersion::kV1;
        result.adaptive_inpaint_variant = temporal
            ? AdaptiveInpaintVariant::kTemporal
            : local ? AdaptiveInpaintVariant::kLocal
                    : result.adaptive_inpaint_version ==
                              adaptivequality::ComponentVersion::kV2
                          ? AdaptiveInpaintVariant::kV2Compatibility
                          : AdaptiveInpaintVariant::kNone;
      } else {
        // Keep the already validated Blackwell decision kernel. Adaptive
        // quality is intentionally partial rather than substituting a guessed
        // payload when the exact inpaint profile is unavailable.
        result.adaptive_fallback = true;
      }
    }
#endif
#if MFGUNLOCK_HAS_GENERATED_THIN_GEOMETRY_CUBINS
    if (replacement_size > candidate.slot_size) {
      if (redirect_variant == nullptr ||
          (candidate.role != KernelRole::MotionVector &&
           candidate.role != KernelRole::InpaintDecision)) {
        detail = "oversized cubin is not an authorized Adaptive Quality V3 redirect";
        Restore(patches, allocations);
        return false;
      }
      std::string redirect_detail;
      if (!internal::RedirectOversizedCubin(
              module, candidate, *redirect_variant, patches, allocations,
              redirect_detail)) {
        detail = redirect_detail;
        Restore(patches, allocations);
        return false;
      }
      if (candidate.role == KernelRole::MotionVector) {
        result.adaptive_geometry_redirected = true;
        result.motion_vector = true;
      } else {
        result.adaptive_inpaint_redirected = true;
        result.inpaint_decision = true;
      }
      ++result.kernels;
      continue;
    }
#endif
    Patch patch;
    patch.payload = candidate.payload;
    patch.original.assign(candidate.payload, candidate.payload + candidate.slot_size);

    DWORD old_protection = 0;
    if (!VirtualProtect(candidate.payload, candidate.slot_size, PAGE_READWRITE, &old_protection)) {
      detail = std::string("cubin slot was not writable for ") + RoleName(candidate.role);
      Restore(patches, allocations);
      return false;
    }
    std::memcpy(candidate.payload, replacement_data, replacement_size);
    std::memset(candidate.payload + replacement_size, 0,
                candidate.slot_size - replacement_size);
    DWORD ignored = 0;
    VirtualProtect(candidate.payload, candidate.slot_size, old_protection, &ignored);
    patches.push_back(std::move(patch));

    result.motion_vector |= candidate.role == KernelRole::MotionVector;
    result.inpaint |= candidate.role == KernelRole::Inpaint;
    result.inpaint_decision |= candidate.role == KernelRole::InpaintDecision;
    ++result.kernels;
  }
#endif

  std::ostringstream stream;
  stream << "precompiled Blackwell cubins in original Ada slots: motion-vector="
         << (result.motion_vector ? "yes" : "no") << ", inpaint="
         << (result.inpaint ? "yes" : "no") << ", decision="
         << (result.inpaint_decision ? "yes" : "no") << ", kernels=" << result.kernels;
  if (result.silhouette_guard_requested) {
    stream << "; silhouette boundary guard=";
    if (result.silhouette_guard) {
      if (result.silhouette_guard_fallback) stream << "fallback ";
      stream << SilhouetteGuardName(result.silhouette_guard_mode_selected);
      if (result.adaptive_geometry)
        stream << " (adaptive asymmetric confidence "
               << adaptivequality::ComponentVersionName(
                      result.adaptive_geometry_version)
               << ", "
               << AdaptiveGeometryVariantName(
                      result.adaptive_geometry_variant)
               << ')';
      else if (result.geometry_confidence_v2) stream << " (geometry confidence V2)";
      else if (result.refined_geometry) stream << " (refined confidence V1)";
    } else if (result.silhouette_guard_fallback) {
      stream << "unsupported (0.9 retention fallback applied)";
    } else {
      stream << "unsupported (baseline retained)";
    }
  } else if (enable_intermediate_scatter) {
    stream << "; intermediate scatter retention="
           << (result.intermediate_scatter ? "applied" : "unsupported (baseline retained)");
  }
  if (enable_adaptive_quality) {
    stream << "; adaptive quality geometry="
           << (result.adaptive_geometry
                   ? adaptivequality::ComponentVersionName(
                         result.adaptive_geometry_version)
                   : "native")
           << " ("
           << AdaptiveGeometryVariantName(result.adaptive_geometry_variant)
           << ')'
           << ", directional scatter="
           << (result.adaptive_directional_scatter
                   ? "native motion-adaptive path"
                   : "baseline")
           << ", inpaint decision="
           << (result.adaptive_inpaint_decision
                    ? adaptivequality::ComponentVersionName(
                          result.adaptive_inpaint_version)
                    : "native")
           << " ("
           << AdaptiveInpaintVariantName(result.adaptive_inpaint_variant)
           << ')';
    if (g_adaptive_quality_profile ==
        adaptivequality::Profile::kLuminanceDirectionalV3) {
      stream << ", V3 oriented-geometry A/B="
             << (g_adaptive_quality_v3_oriented_geometry
                     ? "enabled"
                     : "disabled (intentional V2 request)");
      if (result.adaptive_geometry_redirected)
        stream << ", V3 install=redirected oversized ptxas cubin";
      if (result.adaptive_inpaint_redirected)
        stream << ", inpaint V3 install=redirected oversized ptxas cubin";
    }
    if (result.adaptive_fallback) stream << " (one or more exact variants unavailable)";
  }
  detail = stream.str();
  return result.motion_vector;
}

}  // namespace mfgunlock::blackwell
