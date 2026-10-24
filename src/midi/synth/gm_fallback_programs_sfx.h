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
  o.sfx_guitar_fret.cutoff_hz = 2122.55f;
  o.sfx_guitar_fret.resonance_q = 4.87184f;
  o.sfx_guitar_fret.gain = 0.5919f;
  o.sfx_guitar_fret.amp_env.attack_ms = 2.23697f;
  o.sfx_guitar_fret.amp_env.decay_ms = 94.4725f;
  o.sfx_guitar_fret.unison = 1;
  o.sfx_guitar_fret.drive = 0.238832f;
  o.sfx_guitar_fret.stereo_spread = 0.0985691f;
  o.sfx_guitar_fret.va.velocity_exponent = 1.18f;

  // Breath Noise (GM 121): a soft low-passed burst with a gradual mouth onset.
  o.sfx_breath = sfx;
  o.sfx_breath.waveform = VaWaveform::kNoise;
  o.sfx_breath.amp_env = fallback_env(80.0f, 450.0f, 0.2f, 250.0f);
  o.sfx_breath.cutoff_hz = 2946.55f;
  o.sfx_breath.resonance_q = 30.0f;
  o.sfx_breath.filter_env = fallback_env(10.0f, 300.0f, 0.3f, 180.0f);
  o.sfx_breath.env_to_cutoff_cents = 1803.67f;
  o.sfx_breath.gain = 0.5757f;
  o.sfx_breath.amp_env.attack_ms = 13.5907f;
  // A breath is one exhalation and does not hold: the sustain floor kept it
  // sounding for a second where the reference is done in 0.4, falling at
  // 145.5 dB/s against the 12.1 the held tail was producing.
  o.sfx_breath.amp_env.sustain = 0.0f;
  o.sfx_breath.amp_env.decay_ms = 180.0f;
  o.sfx_breath.amp_env.release_ms = 111.154f;
  o.sfx_breath.filter_env.attack_ms = 21.0184f;
  o.sfx_breath.filter_env.decay_ms = 147.834f;
  o.sfx_breath.filter_env.sustain = 0.532399f;
  o.sfx_breath.va.velocity_exponent = 1.69f;

  // Seashore (GM 122): a long noise swell with a high-pass edge that recedes.
  o.sfx_seashore = sfx;
  o.sfx_seashore.waveform = VaWaveform::kNoise;
  o.sfx_seashore.amp_env = fallback_env(1000.0f, 1600.0f, 0.35f, 900.0f);
  o.sfx_seashore.filter_output = SynthFilterOutput::kHighpass;
  o.sfx_seashore.cutoff_hz = 802.472f;
  o.sfx_seashore.resonance_q = 12.706f;
  o.sfx_seashore.filter_env = fallback_env(500.0f, 1300.0f, 0.15f, 700.0f);
  o.sfx_seashore.env_to_cutoff_cents = 596.857f;
  o.sfx_seashore.stereo_spread = 0.261447f;
  o.sfx_seashore.gain = 0.5367f;
  o.sfx_seashore.amp_env.attack_ms = 845.17f;
  o.sfx_seashore.amp_env.release_ms = 3404.28f;
  o.sfx_seashore.amp_env.sustain = 0.672047f;
  o.sfx_seashore.filter_env.attack_ms = 866.014f;
  o.sfx_seashore.filter_env.decay_ms = 59.0997f;
  o.sfx_seashore.filter_env.release_ms = 14443.3f;
  o.sfx_seashore.filter_env.sustain = 0.320674f;
  o.sfx_seashore.unison = 2;
  o.sfx_seashore.amp_env.decay_ms = 533.84f;
  o.sfx_seashore.va.velocity_exponent = 1.01f;

  // Bird Tweet (GM 123): AmpEnv -> PitchCents sweeps the triangle upward as the call opens.
  o.sfx_bird_tweet = sfx;
  o.sfx_bird_tweet.waveform = VaWaveform::kTriangle;
  o.sfx_bird_tweet.amp_env = fallback_env(35.0f, 180.0f, 0.2f, 100.0f);
  o.sfx_bird_tweet.cutoff_hz = 7329.37f;
  o.sfx_bird_tweet.resonance_q = 1.49558f;
  o.sfx_bird_tweet.filter_env = fallback_env(2.0f, 100.0f, 0.2f, 80.0f);
  o.sfx_bird_tweet.env_to_cutoff_cents = 262.303f;
  o.sfx_bird_tweet.lfo_rate_hz = 35.4734f;
  o.sfx_bird_tweet.lfo_to_pitch_cents = 614.177f;
  o.sfx_bird_tweet.mod_matrix.routes[0] = {ModSource::kAmpEnv, ModDestination::kPitchCents, 900.0f};
  o.sfx_bird_tweet.gain = 0.5245f;
  o.sfx_bird_tweet.amp_env.attack_ms = 339.109f;
  o.sfx_bird_tweet.amp_env.decay_ms = 17.7284f;
  o.sfx_bird_tweet.amp_env.release_ms = 1183.12f;
  o.sfx_bird_tweet.amp_env.sustain = 0.0728634f;
  o.sfx_bird_tweet.filter_env.sustain = 0.563241f;
  o.sfx_bird_tweet.unison = 2;
  o.sfx_bird_tweet.filter_env.attack_ms = 1.06624f;
  o.sfx_bird_tweet.filter_env.decay_ms = 81.5462f;
  o.sfx_bird_tweet.filter_env.release_ms = 630.762f;
  o.sfx_bird_tweet.stereo_spread = 0.0119364f;
  o.sfx_bird_tweet.va.velocity_exponent = 1.3f;

  // Telephone Ring (GM 124): no fixed oscillator, so this is a key-following resonant square
  // approximation.
  o.sfx_telephone_ring = sfx;
  o.sfx_telephone_ring.waveform = VaWaveform::kSquare;
  o.sfx_telephone_ring.unison = 3;
  o.sfx_telephone_ring.detune_cents = 151.702f;
  o.sfx_telephone_ring.amp_env = fallback_env(5.0f, 120.0f, 0.6f, 120.0f);
  o.sfx_telephone_ring.filter_output = SynthFilterOutput::kBandpass;
  o.sfx_telephone_ring.cutoff_hz = 4946.83f;
  o.sfx_telephone_ring.resonance_q = 4.92292f;
  o.sfx_telephone_ring.key_track = 0.4f;
  o.sfx_telephone_ring.lfo2_rate_hz = 2.2f;
  o.sfx_telephone_ring.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, -0.5f};
  o.sfx_telephone_ring.gain = 0.4567f;
  o.sfx_telephone_ring.amp_env.attack_ms = 178.532f;
  o.sfx_telephone_ring.amp_env.decay_ms = 126.687f;
  o.sfx_telephone_ring.amp_env.release_ms = 99.1493f;
  o.sfx_telephone_ring.amp_env.sustain = 0.479105f;
  o.sfx_telephone_ring.drive = 0.513932f;
  o.sfx_telephone_ring.stereo_spread = 0.059017f;
  o.sfx_telephone_ring.va.velocity_exponent = 1.0f;

  // Helicopter (GM 125): a low noise band with periodic amplitude substitutes for rotor structure.
  o.sfx_helicopter = sfx;
  o.sfx_helicopter.waveform = VaWaveform::kNoise;
  o.sfx_helicopter.amp_env = fallback_env(10.0f, 1000.0f, 0.5f, 220.0f);
  o.sfx_helicopter.filter_output = SynthFilterOutput::kBandpass;
  o.sfx_helicopter.cutoff_hz = 259.944f;
  o.sfx_helicopter.resonance_q = 0.510817f;
  o.sfx_helicopter.lfo2_rate_hz = 6.0f;
  o.sfx_helicopter.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, 0.8f};
  o.sfx_helicopter.gain = 0.5051f;
  o.sfx_helicopter.amp_env.attack_ms = 0.603522f;
  o.sfx_helicopter.amp_env.decay_ms = 176.254f;
  o.sfx_helicopter.amp_env.release_ms = 2132.05f;
  o.sfx_helicopter.amp_env.sustain = 0.317108f;
  o.sfx_helicopter.unison = 3;
  o.sfx_helicopter.drive = 0.244159f;
  o.sfx_helicopter.stereo_spread = 0.00537227f;
  o.sfx_helicopter.va.velocity_exponent = 1.01f;

  // Applause (GM 126): broad bright noise with random scatter and a gentle envelope flutter.
  o.sfx_applause = sfx;
  o.sfx_applause.waveform = VaWaveform::kNoise;
  o.sfx_applause.amp_env = fallback_env(10.0f, 1800.0f, 0.4f, 500.0f);
  o.sfx_applause.filter_output = SynthFilterOutput::kHighpass;
  o.sfx_applause.cutoff_hz = 124.028f;
  o.sfx_applause.resonance_q = 0.622486f;
  o.sfx_applause.lfo2_rate_hz = 7.0f;
  o.sfx_applause.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, 0.35f};
  o.sfx_applause.stereo_spread = 0.149f;
  o.sfx_applause.gain = 0.5486f;
  o.sfx_applause.amp_env.attack_ms = 1.10334f;
  o.sfx_applause.amp_env.decay_ms = 6331.34f;
  o.sfx_applause.amp_env.release_ms = 2908.98f;
  o.sfx_applause.amp_env.sustain = 0.691363f;
  o.sfx_applause.unison = 5;
  o.sfx_applause.va.velocity_exponent = 0.98f;

  // Gunshot (GM 127): a compressed noise impulse with a bright muzzle envelope and pressure crack.
  o.sfx_gunshot = sfx;
  o.sfx_gunshot.waveform = VaWaveform::kNoise;
  o.sfx_gunshot.amp_env = fallback_env(0.2f, 300.0f, 0.0f, 80.0f);
  o.sfx_gunshot.cutoff_hz = 5865.21f;
  o.sfx_gunshot.resonance_q = 1.26163f;
  o.sfx_gunshot.filter_env = fallback_env(0.2f, 100.0f, 0.0f, 70.0f);
  o.sfx_gunshot.env_to_cutoff_cents = 3144.8f;
  o.sfx_gunshot.drive = 0.979878f;
  o.sfx_gunshot.gain = 0.6405f;
  o.sfx_gunshot.amp_env.attack_ms = 4.83103f;
  o.sfx_gunshot.amp_env.decay_ms = 410.849f;
  o.sfx_gunshot.filter_env.decay_ms = 3352.34f;
  o.sfx_gunshot.filter_env.sustain = 0.860338f;
  o.sfx_gunshot.filter_env.attack_ms = 0.191351f;
  o.sfx_gunshot.stereo_spread = 0.262831f;
  o.sfx_gunshot.unison = 2;
  o.sfx_gunshot.va.velocity_exponent = 1.07f;
}

}  // namespace sonare::midi::synth::detail
