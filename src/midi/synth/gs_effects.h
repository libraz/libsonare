#pragma once

/// @file gs_effects.h
/// @brief GS system effect bus for the SF2 player: the reverb / chorus /
///        delay send-return units behind the per-part send levels (CC91 /
///        CC93 / CC94 and the SF2 reverb/chorusEffectsSend generators).
///
/// SF2 zones only carry SEND AMOUNTS; the effect bodies are ours. This bus
/// reuses the existing effect suite — effects/reverb (Dattorro plate),
/// effects/modulation (chorus) and effects/delay (stereo feedback delay) —
/// run wet-only as send-return units. All three are deterministic (no RNG,
/// fixed LFO phases), preserving the bit-identical bounce contract.
///
/// RT contract: the constructor and prepare() run on the control thread and
/// own all allocation (effect delay lines + the fixed-size chunk buses);
/// begin_chunk() / render_returns() run on the audio thread and are
/// allocation-free. Voices accumulate into the send buses in chunks of at
/// most kBlockFrames.
///
/// Only compiled when the FX suite is built (SONARE_MIDI_WITH_FX); without it
/// the player simply renders dry.

#include <cstdint>
#include <vector>

#include "effects/delay/stereo_delay.h"
#include "effects/modulation/chorus.h"
#include "effects/reverb/dattorro_reverb.h"

namespace sonare::midi::synth {

/// System effect parameters (GS-flavoured defaults: a plate-ish hall, a
/// gentle chorus and the SC-88 style single delay).
struct GsEffectsConfig {
  bool enable_reverb = true;
  bool enable_chorus = true;
  bool enable_delay = true;
  float reverb_decay = 0.7f;    ///< Tank feedback, [0, 0.98].
  float reverb_damping = 0.4f;  ///< HF damping, [0, 1].
  float chorus_rate_hz = 0.8f;
  float chorus_depth_ms = 6.0f;
  float delay_time_ms = 340.0f;
  float delay_feedback = 0.25f;  ///< [0, 0.9].
};

class GsEffectBus {
 public:
  /// Internal chunk size; senders accumulate at most this many frames between
  /// begin_chunk() and render_returns().
  static constexpr int kBlockFrames = 256;

  explicit GsEffectBus(const GsEffectsConfig& config);

  /// CONTROL thread: prepares the effect units and chunk buses.
  void prepare(double sample_rate);
  void reset();

  /// AUDIO thread: zero the send buses for the next chunk.
  void begin_chunk() noexcept;

  /// Send-bus accumulation targets for the current chunk (stereo planar).
  float* reverb_in(int ch) noexcept { return reverb_bus_[ch & 1].data(); }
  float* chorus_in(int ch) noexcept { return chorus_bus_[ch & 1].data(); }
  float* delay_in(int ch) noexcept { return delay_bus_[ch & 1].data(); }

  bool reverb_enabled() const noexcept { return config_.enable_reverb; }
  bool chorus_enabled() const noexcept { return config_.enable_chorus; }
  bool delay_enabled() const noexcept { return config_.enable_delay; }

  /// AUDIO thread: run the wet-only effect units over the accumulated send
  /// buses and ADD the returns into @p out_l / @p out_r (n <= kBlockFrames).
  void render_returns(float* out_l, float* out_r, int n) noexcept;

  /// Decay-tail bound of the slowest enabled effect at @p sample_rate, so the
  /// player's tail_samples() covers reverb/delay ring-out.
  int64_t tail_samples(double sample_rate) const noexcept;

 private:
  GsEffectsConfig config_{};
  effects::reverb::DattorroReverb reverb_;
  effects::modulation::Chorus chorus_;
  effects::delay::StereoDelay delay_;
  std::vector<float> reverb_bus_[2];
  std::vector<float> chorus_bus_[2];
  std::vector<float> delay_bus_[2];
};

}  // namespace sonare::midi::synth
