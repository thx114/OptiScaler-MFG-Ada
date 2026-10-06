// Upstream tests adapted to the vendored MIT headers; see mavis/UPSTREAM.md.
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "../../OptiScaler/framegen/dlssg/mavis/adaptive_inpaint_v3.hpp"

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition   \
                << '\n';                                                       \
      return EXIT_FAILURE;                                                     \
    }                                                                          \
  } while (false)

int main() {
  using namespace mfgunlock::adaptiveinpaint;

  CHECK(!Decode(kInvalid).valid);
  for (uint8_t confidence = 0; confidence <= kMaxConfidence; ++confidence) {
    for (const auto state : {HistoryState::kCold, HistoryState::kArmed,
                             HistoryState::kGraceUsed,
                             HistoryState::kRearmSeen}) {
      const auto decoded = Decode(Encode(confidence, state));
      CHECK(decoded.valid);
      CHECK(decoded.confidence == confidence);
      CHECK(decoded.state == state);
      CHECK(Encode(confidence, state) != kInvalid);
    }
  }
  CHECK(std::abs(Dequantize(Quantize(0.5f)) - 0.5f) <= 0.5f / 62.0f);
  CHECK(Quantize(-1.0f) == 0);
  CHECK(Quantize(2.0f) == 62);

  auto update = UpdateHistoryReference(62, kInvalid, false);
  CHECK(update.effective_confidence == 62);
  CHECK(Decode(update.encoded).state == HistoryState::kRearmSeen);
  update = UpdateHistoryReference(62, update.encoded, false);
  CHECK(Decode(update.encoded).state == HistoryState::kArmed);

  // One ambiguous soft drop can retain at most 12/62 confidence. The next
  // identical observation falls immediately because GraceUsed is not Armed.
  update = UpdateHistoryReference(0, update.encoded, false);
  CHECK(update.effective_confidence == kRecoveryStep);
  CHECK(Decode(update.encoded).state == HistoryState::kGraceUsed);
  update = UpdateHistoryReference(0, update.encoded, false);
  CHECK(update.effective_confidence == 0);

  // Recovery is monotonic and bounded to <= 0.20 per source-frame update.
  uint8_t history = Encode(0, HistoryState::kCold);
  for (int index = 0; index < 6; ++index) {
    const auto before = Decode(history).confidence;
    update = UpdateHistoryReference(62, history, false);
    CHECK(update.effective_confidence >= before);
    CHECK(update.effective_confidence - before <= kRecoveryStep);
    history = update.encoded;
  }

  // Hard rejection is immediate even from a fully armed/high-confidence byte.
  update = UpdateHistoryReference(62,
                                  Encode(62, HistoryState::kArmed), true);
  CHECK(update.effective_confidence == 0);
  CHECK(Decode(update.encoded).state == HistoryState::kCold);

  // Rearming requires two stable high observations.
  history = Encode(62, HistoryState::kGraceUsed);
  update = UpdateHistoryReference(62, history, false);
  CHECK(Decode(update.encoded).state == HistoryState::kRearmSeen);
  update = UpdateHistoryReference(62, update.encoded, false);
  CHECK(Decode(update.encoded).state == HistoryState::kArmed);

  std::cout << "Adaptive inpaint V3.4 tests passed\n";
  return EXIT_SUCCESS;
}
