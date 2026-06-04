#pragma once

/// @file sf2_voice.h
/// @brief SF2 generator resolution and the per-voice playback state used by
///        Sf2Player: zone-pair (preset x instrument) generator stacking per
///        the SoundFont 2.04 model, sample playback with root-key tuning,
///        loop modes and linear interpolation, the volume DAHDSR and
///        pan/attenuation.
///
/// Generator semantics: INSTRUMENT-level generators are absolute values that
/// override the spec defaults; PRESET-level generators are RELATIVE additions
/// on top. A zone's effective value therefore stacks
///   default -> instrument global zone -> instrument zone (override)
///   + preset global zone + preset zone (add).
///
/// RT contract: resolve_voice_params() / Sf2Voice methods are allocation-free
/// (audio thread); the referenced Sf2File data is read-only.

#include <cstdint>

#include "midi/synth/envelope.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/voice_pool.h"

namespace sonare::midi::synth {

/// Effective generator values for one (preset zone, instrument zone) pair.
/// Values are stored in raw SF2 units (timecents, centibels, cents, ...).
class Sf2GenSet {
 public:
  static constexpr int kNumGens = 61;

  Sf2GenSet() noexcept;  // spec defaults

  /// Instrument-level zone: absolute override.
  void apply_absolute(const Sf2Zone& zone) noexcept;
  /// Preset-level zone: relative addition (index/range/sample generators are
  /// never additive and are skipped).
  void add_relative(const Sf2Zone& zone) noexcept;

  int32_t get(uint16_t oper) const noexcept { return oper < kNumGens ? values_[oper] : 0; }

 private:
  int32_t values_[kNumGens];
};

/// SF2 timecents -> seconds (2^(tc/1200)); tc <= -12000 is ~1 ms or less.
float timecents_to_seconds(int32_t timecents) noexcept;
/// SF2 centibels (attenuation) -> linear gain (10^(-cb/200)).
float centibels_to_gain(float centibels) noexcept;
/// SF2 absolute cents -> Hz (8.176 Hz * 2^(cents/1200)).
float abs_cents_to_hz(int32_t cents) noexcept;

/// Playback parameters resolved from a zone pair for one note-on.
struct Sf2VoiceParams {
  // Sample addressing (pool indices, offsets already applied).
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t loop_start = 0;
  uint32_t loop_end = 0;
  /// 0 = no loop, 1 = continuous loop, 3 = loop while key down.
  int loop_mode = 0;
  /// Sample-position increment per output sample at the voice's pitch.
  double pitch_increment = 1.0;
  /// Linear gain from initialAttenuation (velocity handling is separate).
  float attenuation_gain = 1.0f;
  /// Constant-power pan gains from the pan generator (-500..500 -> L..R).
  float gain_left = 0.70710678f;
  float gain_right = 0.70710678f;
  /// Volume envelope (already converted to ms / level units).
  DahdsrConfig volume_env;
  /// Exclusive class (hi-hat choke groups); 0 = none.
  int exclusive_class = 0;
};

/// Resolves the effective parameters for @p key at @p sample_rate from the
/// stacked generators and the sample header (root key / correction / loops).
Sf2VoiceParams resolve_voice_params(const Sf2GenSet& gens, const Sf2Sample& sample, uint8_t key,
                                    double output_sample_rate) noexcept;

/// One playing SF2 voice (lives in a VoicePool inside Sf2Player).
struct Sf2Voice : VoiceState {
  const float* data = nullptr;  // sample pool base (read-only)
  Sf2VoiceParams params;
  double pos = 0.0;
  float velocity_gain = 1.0f;
  DahdsrEnvelope env;
  bool key_down = false;

  /// Starts playback of @p p (pool base @p pool_data) at @p velocity_gain_in.
  void start(const float* pool_data, const Sf2VoiceParams& p, double sample_rate,
             float velocity_gain_in) noexcept;
  /// Renders one mono sample (envelope applied; pan is applied by the mixer).
  /// Returns 0 and deactivates once the sample / envelope ends.
  float render() noexcept;
  /// Note-off: release the envelope; mode-3 loops stop looping.
  void release() noexcept;
};

}  // namespace sonare::midi::synth
