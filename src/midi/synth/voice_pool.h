#pragma once

/// @file voice_pool.h
/// @brief Fixed-size polyphonic voice pool with deterministic voice stealing,
///        shared by the SF2 player and the NativeSynth engine.
///
/// Steal policy (deterministic, in priority order):
///   1. a free (inactive) slot,
///   2. the oldest releasing voice (smallest age among voices past note-off),
///   3. the oldest voice overall (smallest age).
///
/// RT contract: prepare() runs on the control thread and is the only place that
/// allocates (it sizes the slot vector). allocate() / iteration / note lookup
/// run on the audio thread and are allocation-free, lock-free and IO-free.
///
/// Determinism: ages are a monotonically increasing counter assigned at
/// allocate(); identical event streams produce identical steal decisions.

#include <cstdint>
#include <type_traits>
#include <vector>

namespace sonare::midi::synth {

/// Bookkeeping every pooled voice embeds (derive your voice type from this).
/// The pool only touches these fields; everything else is the synth's.
struct VoiceState {
  bool active = false;
  /// True once the voice entered its release stage (note-off / steal fade).
  /// Releasing voices are preferred steal targets over held voices.
  bool releasing = false;
  uint8_t note = 0;
  uint8_t channel = 0;
  /// Allocation order; smaller = older. Used for deterministic stealing.
  uint64_t age = 0;
};

template <typename Voice>
class VoicePool {
  static_assert(std::is_base_of_v<VoiceState, Voice>,
                "VoicePool requires Voice to derive from VoiceState");

 public:
  /// CONTROL thread: size the pool. The only allocating call.
  void prepare(int polyphony) {
    voices_.assign(static_cast<size_t>(polyphony > 0 ? polyphony : 1), Voice{});
    next_age_ = 1;
  }

  /// AUDIO thread: reset all slots to silence (keeps the allocation).
  void reset() noexcept {
    for (auto& v : voices_) v = Voice{};
    next_age_ = 1;
  }

  /// AUDIO thread: claim a voice for (channel, note) using the deterministic
  /// steal policy. Returns nullptr only when the pool was never prepared.
  /// The returned voice has its VoiceState fields initialised; the caller
  /// fills in the synth-specific state (phase, envelope, ...).
  Voice* allocate(uint8_t channel, uint8_t note) noexcept {
    if (voices_.empty()) return nullptr;
    Voice* target = nullptr;
    // 1. Free slot.
    for (auto& v : voices_) {
      if (!v.active) {
        target = &v;
        break;
      }
    }
    // 2. Oldest releasing voice.
    if (target == nullptr) {
      for (auto& v : voices_) {
        if (v.releasing && (target == nullptr || v.age < target->age)) target = &v;
      }
    }
    // 3. Oldest voice overall.
    if (target == nullptr) {
      target = &voices_[0];
      for (auto& v : voices_) {
        if (v.age < target->age) target = &v;
      }
    }
    target->active = true;
    target->releasing = false;
    target->note = note;
    target->channel = channel;
    target->age = next_age_++;
    return target;
  }

  Voice* data() noexcept { return voices_.data(); }
  const Voice* data() const noexcept { return voices_.data(); }
  size_t size() const noexcept { return voices_.size(); }

  Voice* begin() noexcept { return voices_.data(); }
  Voice* end() noexcept { return voices_.data() + voices_.size(); }
  const Voice* begin() const noexcept { return voices_.data(); }
  const Voice* end() const noexcept { return voices_.data() + voices_.size(); }

  /// AUDIO thread: number of currently active voices.
  int active_count() const noexcept {
    int n = 0;
    for (const auto& v : voices_) n += v.active ? 1 : 0;
    return n;
  }

 private:
  std::vector<Voice> voices_;
  uint64_t next_age_ = 1;
};

}  // namespace sonare::midi::synth
