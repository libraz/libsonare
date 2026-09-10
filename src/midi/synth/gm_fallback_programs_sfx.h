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
  o.sfx_guitar_fret.cutoff_hz = 4804.9f;
  o.sfx_guitar_fret.resonance_q = 0.977302f;
  o.sfx_guitar_fret.gain = 0.72f;
  o.sfx_guitar_fret.amp_env.attack_ms = 3.86531f;
  o.sfx_guitar_fret.amp_env.decay_ms = 27.038f;
  o.sfx_guitar_fret.drive = 0.326844f;
  o.sfx_guitar_fret.stereo_spread = 0.365225f;
  o.sfx_guitar_fret.unison = 1;

  // Breath Noise (GM 121): a soft low-passed burst with a gradual mouth onset.
  o.sfx_breath = sfx;
  o.sfx_breath.waveform = VaWaveform::kNoise;
  o.sfx_breath.amp_env = fallback_env(80.0f, 450.0f, 0.2f, 250.0f);
  o.sfx_breath.cutoff_hz = 2946.55f;
  o.sfx_breath.resonance_q = 30.0f;
  o.sfx_breath.filter_env = fallback_env(10.0f, 300.0f, 0.3f, 180.0f);
  o.sfx_breath.env_to_cutoff_cents = 1803.67f;
  o.sfx_breath.gain = 0.62f;
  o.sfx_breath.amp_env.attack_ms = 13.5907f;
  o.sfx_breath.amp_env.decay_ms = 472.764f;
  o.sfx_breath.amp_env.release_ms = 111.154f;
  o.sfx_breath.amp_env.sustain = 0.0f;
  o.sfx_breath.drive = 0.917385f;
  o.sfx_breath.filter_env.attack_ms = 21.0184f;
  o.sfx_breath.filter_env.decay_ms = 147.834f;
  o.sfx_breath.filter_env.sustain = 0.532399f;

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
  o.sfx_seashore.gain = 0.68f;
  o.sfx_seashore.amp_env.attack_ms = 845.17f;
  o.sfx_seashore.amp_env.release_ms = 1135.84f;
  o.sfx_seashore.amp_env.sustain = 0.641407f;
  o.sfx_seashore.drive = 0.174622f;
  o.sfx_seashore.filter_env.attack_ms = 866.014f;
  o.sfx_seashore.filter_env.decay_ms = 59.0997f;
  o.sfx_seashore.filter_env.release_ms = 14443.3f;
  o.sfx_seashore.filter_env.sustain = 0.320674f;
  o.sfx_seashore.unison = 2;

  // Bird Tweet (GM 123): AmpEnv -> PitchCents sweeps the triangle upward as the call opens.
  o.sfx_bird_tweet = sfx;
  o.sfx_bird_tweet.waveform = VaWaveform::kTriangle;
  o.sfx_bird_tweet.amp_env = fallback_env(35.0f, 180.0f, 0.2f, 100.0f);
  o.sfx_bird_tweet.cutoff_hz = 5919.9f;
  o.sfx_bird_tweet.resonance_q = 0.771412f;
  o.sfx_bird_tweet.filter_env = fallback_env(2.0f, 100.0f, 0.2f, 80.0f);
  o.sfx_bird_tweet.env_to_cutoff_cents = 1200.0f;
  o.sfx_bird_tweet.lfo_rate_hz = 11.8509f;
  o.sfx_bird_tweet.lfo_to_pitch_cents = 81.3796f;
  o.sfx_bird_tweet.mod_matrix.routes[0] = {ModSource::kAmpEnv, ModDestination::kPitchCents, 900.0f};
  o.sfx_bird_tweet.gain = 0.62f;
  o.sfx_bird_tweet.amp_env.attack_ms = 281.973f;
  o.sfx_bird_tweet.amp_env.decay_ms = 79.0786f;
  o.sfx_bird_tweet.amp_env.release_ms = 150.056f;
  o.sfx_bird_tweet.amp_env.sustain = 0.268377f;
  o.sfx_bird_tweet.drive = 0.0650624f;
  o.sfx_bird_tweet.filter_env.sustain = 0.664491f;
  o.sfx_bird_tweet.stereo_spread = 0.494862f;
  o.sfx_bird_tweet.unison = 1;

  // Telephone Ring (GM 124): no fixed oscillator, so this is a key-following resonant square
  // approximation.
  o.sfx_telephone_ring = sfx;
  o.sfx_telephone_ring.waveform = VaWaveform::kSquare;
  o.sfx_telephone_ring.unison = 6;
  o.sfx_telephone_ring.detune_cents = 102.208f;
  o.sfx_telephone_ring.amp_env = fallback_env(5.0f, 120.0f, 0.6f, 120.0f);
  o.sfx_telephone_ring.filter_output = SynthFilterOutput::kBandpass;
  o.sfx_telephone_ring.cutoff_hz = 3253.08f;
  o.sfx_telephone_ring.resonance_q = 7.7448f;
  o.sfx_telephone_ring.key_track = 0.4f;
  o.sfx_telephone_ring.lfo2_rate_hz = 2.2f;
  o.sfx_telephone_ring.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, -0.5f};
  o.sfx_telephone_ring.gain = 0.58f;
  o.sfx_telephone_ring.amp_env.attack_ms = 77.8421f;
  o.sfx_telephone_ring.amp_env.decay_ms = 206.978f;
  o.sfx_telephone_ring.amp_env.release_ms = 161.987f;
  o.sfx_telephone_ring.amp_env.sustain = 0.654513f;
  o.sfx_telephone_ring.drive = 0.234501f;
  o.sfx_telephone_ring.stereo_spread = 0.563966f;

  // Helicopter (GM 125): a low noise band with periodic amplitude substitutes for rotor structure.
  o.sfx_helicopter = sfx;
  o.sfx_helicopter.waveform = VaWaveform::kNoise;
  o.sfx_helicopter.amp_env = fallback_env(10.0f, 1000.0f, 0.5f, 220.0f);
  o.sfx_helicopter.filter_output = SynthFilterOutput::kBandpass;
  o.sfx_helicopter.cutoff_hz = 803.366f;
  o.sfx_helicopter.resonance_q = 0.622768f;
  o.sfx_helicopter.lfo2_rate_hz = 6.0f;
  o.sfx_helicopter.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, 0.8f};
  o.sfx_helicopter.gain = 0.64f;
  o.sfx_helicopter.amp_env.attack_ms = 0.678271f;
  o.sfx_helicopter.amp_env.decay_ms = 248.246f;
  o.sfx_helicopter.amp_env.release_ms = 727.761f;
  o.sfx_helicopter.amp_env.sustain = 0.3302f;
  o.sfx_helicopter.drive = 0.200274f;
  o.sfx_helicopter.stereo_spread = 0.739739f;
  o.sfx_helicopter.unison = 3;

  // Applause (GM 126): broad bright noise with random scatter and a gentle envelope flutter.
  o.sfx_applause = sfx;
  o.sfx_applause.waveform = VaWaveform::kNoise;
  o.sfx_applause.amp_env = fallback_env(10.0f, 1800.0f, 0.4f, 500.0f);
  o.sfx_applause.filter_output = SynthFilterOutput::kHighpass;
  o.sfx_applause.cutoff_hz = 985.038f;
  o.sfx_applause.resonance_q = 1.58982f;
  o.sfx_applause.lfo2_rate_hz = 7.0f;
  o.sfx_applause.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, 0.35f};
  o.sfx_applause.stereo_spread = 0.592437f;
  o.sfx_applause.gain = 0.7f;
  o.sfx_applause.amp_env.attack_ms = 1.38342f;
  o.sfx_applause.amp_env.decay_ms = 1686.85f;
  o.sfx_applause.amp_env.release_ms = 10725.7f;
  o.sfx_applause.amp_env.sustain = 0.19827f;
  o.sfx_applause.drive = 0.859315f;
  o.sfx_applause.unison = 2;

  // Gunshot (GM 127): a compressed noise impulse with a bright muzzle envelope and pressure crack.
  o.sfx_gunshot = sfx;
  o.sfx_gunshot.waveform = VaWaveform::kNoise;
  o.sfx_gunshot.amp_env = fallback_env(0.2f, 300.0f, 0.0f, 80.0f);
  o.sfx_gunshot.cutoff_hz = 14104.9f;
  o.sfx_gunshot.resonance_q = 0.567926f;
  o.sfx_gunshot.filter_env = fallback_env(0.2f, 100.0f, 0.0f, 70.0f);
  o.sfx_gunshot.env_to_cutoff_cents = 4187.26f;
  o.sfx_gunshot.drive = 0.588155f;
  o.sfx_gunshot.gain = 0.8f;
  o.sfx_gunshot.amp_env.attack_ms = 1.29705f;
  o.sfx_gunshot.amp_env.decay_ms = 404.595f;
  o.sfx_gunshot.filter_env.decay_ms = 3352.34f;
  o.sfx_gunshot.filter_env.sustain = 0.860338f;
  o.sfx_gunshot.stereo_spread = 0.758886f;
}

}  // namespace sonare::midi::synth::detail
