#pragma once

/// @file envelope.h
/// @brief Exponential DAHDSR envelope generator for the shared voice toolkit.
///
/// Linear envelope segments sound synthetic; real instruments decay
/// exponentially. Attack rises along a one-pole curve aimed ABOVE 1.0 (analog
/// envelope generators overshoot their comparator threshold), giving the
/// snappy convex attack shape; decay/release fall along true exponentials
/// toward sustain / zero. Stage times use the one-pole time constants from
/// util/dsp_primitives.h.
///
/// RT contract: configure() / note_on() / note_off() / next() are all
/// allocation-free and may run on the audio thread. Determinism: pure
/// arithmetic, no RNG, no wall clock.

#include <cstdint>

namespace sonare::midi::synth {

/// Stage times in milliseconds, sustain in [0,1]. A default-initialised config
/// is a usable organ-ish envelope.
struct DahdsrConfig {
  float delay_ms = 0.0f;
  float attack_ms = 5.0f;
  float hold_ms = 0.0f;
  float decay_ms = 60.0f;
  float sustain = 0.7f;
  float release_ms = 120.0f;
};

class DahdsrEnvelope {
 public:
  enum class Stage : uint8_t { kIdle = 0, kDelay, kAttack, kHold, kDecay, kSustain, kRelease };

  /// Level below which a releasing envelope snaps to idle (~ -80 dBFS).
  static constexpr float kSilenceLevel = 1.0e-4f;

  /// Derive per-stage one-pole rates for @p config at @p sample_rate. Pure
  /// arithmetic — callable from either thread. Does not change the run state.
  void configure(double sample_rate, const DahdsrConfig& config) noexcept;

  /// Start (or retrigger) the envelope from its current level (click-free).
  void note_on() noexcept;
  /// Enter the release stage from the current level.
  void note_off() noexcept;
  /// Immediately silence (All Sound Off / steal-kill).
  void kill() noexcept;

  /// Advance one sample and return the new envelope level in [0,1].
  float next() noexcept;

  bool active() const noexcept { return stage_ != Stage::kIdle; }
  bool releasing() const noexcept { return stage_ == Stage::kRelease; }
  Stage stage() const noexcept { return stage_; }
  float level() const noexcept { return level_; }

  /// Longest tail after note-off in samples (time for the release exponential
  /// to fall from 1.0 to kSilenceLevel) for @p release_ms at @p sample_rate.
  static int64_t release_tail_samples(double sample_rate, float release_ms) noexcept;

 private:
  Stage stage_ = Stage::kIdle;
  float level_ = 0.0f;
  float sustain_ = 0.7f;
  // One-pole "new sample weight" rates per stage.
  float attack_rate_ = 1.0f;
  float decay_rate_ = 1.0f;
  float release_rate_ = 1.0f;
  // Delay / hold lengths in samples (counted down on the audio thread).
  int32_t delay_samples_ = 0;
  int32_t hold_samples_ = 0;
  int32_t stage_counter_ = 0;
};

}  // namespace sonare::midi::synth
