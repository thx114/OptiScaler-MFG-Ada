/* SPDX-License-Identifier: MIT
 * Adaptive Quality V3.4 inpaint confidence history.
 *
 * Only the decision confidence is retained. Color, depth and motion vectors
 * always come from the current frame. The scalar reference below is shared by
 * unit tests and documents the integer state machine emitted into PTX.
 */
#pragma once

#include <algorithm>
#include <cstdint>

namespace mfgunlock::adaptiveinpaint {

enum class HistoryState : uint8_t {
  kCold = 0,
  kArmed = 1,
  kGraceUsed = 2,
  kRearmSeen = 3,
};

inline constexpr uint8_t kInvalid = 255u;
inline constexpr uint8_t kConfidenceMask = 0x3fu;
inline constexpr uint8_t kStateShift = 6u;
inline constexpr uint8_t kMaxConfidence = 62u;
inline constexpr uint8_t kRecoveryStep = 12u;  // 12 / 62 = 0.1935
inline constexpr uint8_t kArmConfidence = 47u; // 47 / 62 = 0.7581
inline constexpr uint8_t kStableDelta = 4u;    // 4 / 62 = 0.0645

struct HistorySample {
  uint8_t confidence = 0;
  HistoryState state = HistoryState::kCold;
  bool valid = false;
};

inline constexpr uint8_t Encode(uint8_t confidence, HistoryState state) {
  confidence = (std::min)(confidence, kMaxConfidence);
  return static_cast<uint8_t>(confidence |
                              (static_cast<uint8_t>(state) << kStateShift));
}

inline constexpr HistorySample Decode(uint8_t value) {
  if (value == kInvalid) return {};
  return HistorySample{
      static_cast<uint8_t>(value & kConfidenceMask),
      static_cast<HistoryState>(value >> kStateShift), true};
}

inline constexpr uint8_t Quantize(float confidence) {
  confidence = std::clamp(confidence, 0.0f, 1.0f);
  const auto rounded = static_cast<uint32_t>(confidence * 62.0f + 0.5f);
  return static_cast<uint8_t>((std::min)(rounded, uint32_t{kMaxConfidence}));
}

inline constexpr float Dequantize(uint8_t confidence) {
  return static_cast<float>((std::min)(confidence, kMaxConfidence)) / 62.0f;
}

struct UpdateResult {
  uint8_t encoded = Encode(0, HistoryState::kCold);
  uint8_t effective_confidence = 0;
};

inline constexpr UpdateResult UpdateHistoryReference(
    uint8_t current_confidence, uint8_t previous, bool hard_reject) {
  current_confidence = (std::min)(current_confidence, kMaxConfidence);
  if (hard_reject)
    return {Encode(0, HistoryState::kCold), 0};

  const HistorySample old = Decode(previous);
  if (!old.valid) {
    const auto initial_state = current_confidence >= kArmConfidence
                                   ? HistoryState::kRearmSeen
                                   : HistoryState::kCold;
    return {Encode(current_confidence, initial_state), current_confidence};
  }

  uint8_t effective = current_confidence;
  HistoryState next_state = old.state;
  if (current_confidence > old.confidence) {
    effective = static_cast<uint8_t>((std::min)(
        static_cast<uint32_t>(current_confidence),
        static_cast<uint32_t>(old.confidence) + kRecoveryStep));
  } else if (current_confidence < old.confidence) {
    if (old.state == HistoryState::kArmed) {
      effective = static_cast<uint8_t>((std::min)(
          static_cast<uint32_t>(old.confidence),
          static_cast<uint32_t>(current_confidence) + kRecoveryStep));
      next_state = HistoryState::kGraceUsed;
    } else {
      effective = current_confidence;
    }
  }

  const uint8_t delta = current_confidence > old.confidence
                            ? current_confidence - old.confidence
                            : old.confidence - current_confidence;
  const bool stable_high = current_confidence >= kArmConfidence &&
                           delta <= kStableDelta;
  if (stable_high) {
    if (old.state == HistoryState::kRearmSeen ||
        old.state == HistoryState::kArmed)
      next_state = HistoryState::kArmed;
    else
      next_state = HistoryState::kRearmSeen;
  } else if (next_state == HistoryState::kRearmSeen) {
    next_state = HistoryState::kCold;
  }

  return {Encode(effective, next_state), effective};
}

}  // namespace mfgunlock::adaptiveinpaint
