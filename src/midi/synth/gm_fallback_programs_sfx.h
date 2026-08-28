#pragma once

#include "midi/synth/gm_fallback_data.h"

namespace sonare::midi::synth::detail {

/// GM 120-127 sound effects use the nearest subtractive gesture: no sample player, delay or fixed
/// oscillator.
constexpr void configure_sfx_programs(ProgramOverrides& o) noexcept {
  NativeSynthPatch sfx{};
  sfx.filter_model = SynthFilterModel::kSvf;
  sfx.gain = 0.5f;

  // Guitar Fret Noise (GM 120): a short, bright scrape with no pitched body.
  o.sfx_guitar_fret = sfx;
  o.sfx_guitar_fret.waveform = VaWaveform::kNoise;
  o.sfx_guitar_fret.amp_env = fallback_env(0.2f, 75.0f, 0.0f, 40.0f);
  o.sfx_guitar_fret.filter_output = SynthFilterOutput::kHighpass;
  o.sfx_guitar_fret.cutoff_hz = 3200.0f;
  o.sfx_guitar_fret.resonance_q = 1.5f;
  o.sfx_guitar_fret.gain = 0.72f;

  // Breath Noise (GM 121): a soft low-passed burst with a gradual mouth onset.
  o.sfx_breath = sfx;
  o.sfx_breath.waveform = VaWaveform::kNoise;
  o.sfx_breath.amp_env = fallback_env(80.0f, 450.0f, 0.2f, 250.0f);
  o.sfx_breath.cutoff_hz = 1400.0f;
  o.sfx_breath.resonance_q = 0.7f;
  o.sfx_breath.filter_env = fallback_env(10.0f, 300.0f, 0.3f, 180.0f);
  o.sfx_breath.env_to_cutoff_cents = 1200.0f;
  o.sfx_breath.gain = 0.62f;

  // Seashore (GM 122): a long noise swell with a high-pass edge that recedes.
  o.sfx_seashore = sfx;
  o.sfx_seashore.waveform = VaWaveform::kNoise;
  o.sfx_seashore.amp_env = fallback_env(1000.0f, 1600.0f, 0.35f, 900.0f);
  o.sfx_seashore.filter_output = SynthFilterOutput::kHighpass;
  o.sfx_seashore.cutoff_hz = 2800.0f;
  o.sfx_seashore.resonance_q = 1.1f;
  o.sfx_seashore.filter_env = fallback_env(500.0f, 1300.0f, 0.15f, 700.0f);
  o.sfx_seashore.env_to_cutoff_cents = 2400.0f;
  o.sfx_seashore.stereo_spread = 0.4f;
  o.sfx_seashore.gain = 0.68f;

  // Bird Tweet (GM 123): AmpEnv -> PitchCents sweeps the triangle upward as the call opens.
  o.sfx_bird_tweet = sfx;
  o.sfx_bird_tweet.waveform = VaWaveform::kTriangle;
  o.sfx_bird_tweet.amp_env = fallback_env(35.0f, 180.0f, 0.2f, 100.0f);
  o.sfx_bird_tweet.cutoff_hz = 5200.0f;
  o.sfx_bird_tweet.resonance_q = 1.4f;
  o.sfx_bird_tweet.filter_env = fallback_env(2.0f, 100.0f, 0.2f, 80.0f);
  o.sfx_bird_tweet.env_to_cutoff_cents = 1200.0f;
  o.sfx_bird_tweet.lfo_rate_hz = 6.5f;
  o.sfx_bird_tweet.lfo_to_pitch_cents = 12.0f;
  o.sfx_bird_tweet.mod_matrix.routes[0] = {ModSource::kAmpEnv, ModDestination::kPitchCents, 900.0f};
  o.sfx_bird_tweet.gain = 0.62f;

  // Telephone Ring (GM 124): no fixed oscillator, so this is a key-following resonant square
  // approximation.
  o.sfx_telephone_ring = sfx;
  o.sfx_telephone_ring.waveform = VaWaveform::kSquare;
  o.sfx_telephone_ring.unison = 2;
  o.sfx_telephone_ring.detune_cents = 150.0f;
  o.sfx_telephone_ring.amp_env = fallback_env(5.0f, 120.0f, 0.6f, 120.0f);
  o.sfx_telephone_ring.filter_output = SynthFilterOutput::kBandpass;
  o.sfx_telephone_ring.cutoff_hz = 1800.0f;
  o.sfx_telephone_ring.resonance_q = 10.0f;
  o.sfx_telephone_ring.key_track = 0.4f;
  o.sfx_telephone_ring.lfo2_rate_hz = 2.2f;
  o.sfx_telephone_ring.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, -0.5f};
  o.sfx_telephone_ring.gain = 0.58f;

  // Helicopter (GM 125): a low noise band with periodic amplitude substitutes for rotor structure.
  o.sfx_helicopter = sfx;
  o.sfx_helicopter.waveform = VaWaveform::kNoise;
  o.sfx_helicopter.amp_env = fallback_env(10.0f, 1000.0f, 0.5f, 220.0f);
  o.sfx_helicopter.filter_output = SynthFilterOutput::kBandpass;
  o.sfx_helicopter.cutoff_hz = 700.0f;
  o.sfx_helicopter.resonance_q = 3.0f;
  o.sfx_helicopter.lfo2_rate_hz = 6.0f;
  o.sfx_helicopter.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, 0.8f};
  o.sfx_helicopter.gain = 0.64f;

  // Applause (GM 126): broad bright noise with random scatter and a gentle envelope flutter.
  o.sfx_applause = sfx;
  o.sfx_applause.waveform = VaWaveform::kNoise;
  o.sfx_applause.amp_env = fallback_env(10.0f, 1800.0f, 0.4f, 500.0f);
  o.sfx_applause.filter_output = SynthFilterOutput::kHighpass;
  o.sfx_applause.cutoff_hz = 2200.0f;
  o.sfx_applause.resonance_q = 0.8f;
  o.sfx_applause.lfo2_rate_hz = 7.0f;
  o.sfx_applause.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, 0.35f};
  o.sfx_applause.stereo_spread = 0.8f;
  o.sfx_applause.gain = 0.7f;

  // Gunshot (GM 127): a compressed noise impulse with a bright muzzle envelope and pressure crack.
  o.sfx_gunshot = sfx;
  o.sfx_gunshot.waveform = VaWaveform::kNoise;
  o.sfx_gunshot.amp_env = fallback_env(0.2f, 300.0f, 0.0f, 80.0f);
  o.sfx_gunshot.cutoff_hz = 1700.0f;
  o.sfx_gunshot.resonance_q = 1.4f;
  o.sfx_gunshot.filter_env = fallback_env(0.2f, 100.0f, 0.0f, 70.0f);
  o.sfx_gunshot.env_to_cutoff_cents = 3600.0f;
  o.sfx_gunshot.drive = 0.25f;
  o.sfx_gunshot.gain = 0.8f;
}

}  // namespace sonare::midi::synth::detail
