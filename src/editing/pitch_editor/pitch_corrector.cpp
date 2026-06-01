#include "editing/pitch_editor/pitch_corrector.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "effects/pitch_shift.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::editing::pitch_editor {

using constants::kA4Hz;
using constants::kCentsPerSemitone;
using constants::kMidiA4;
using constants::kSemitonesPerOctave;
using constants::kSpectrumEpsilon;
using constants::kTwoPi;

namespace {

bool valid_voiced_frame(const F0Track& track, int frame) {
  const size_t index = static_cast<size_t>(frame);
  return index < track.f0_hz.size() && index < track.voiced.size() && track.voiced[index] &&
         track.f0_hz[index] > 0.0f && std::isfinite(track.f0_hz[index]);
}

// Largest |delta| (semitones) still handled by TD-PSOLA; larger jumps fall back to spectral shift.
constexpr float kPsolaMaxSemitones = 6.0f;
// Cross-fade duration at voiced/unvoiced boundaries.
constexpr float kCrossfadeMs = 10.0f;

// Hann window value at normalized position t in [0, 1].
float hann(float t) noexcept { return 0.5f - 0.5f * std::cos(kTwoPi * t); }

}  // namespace

PitchCorrector::PitchCorrector(PitchCorrectionConfig config) : config_(config) {}

Audio PitchCorrector::shift(const Audio& audio, float semitones) const {
  PitchShiftConfig shift_config;
  shift_config.backend = config_.backend;
  return pitch_shift(audio, apply_limits(semitones), shift_config);
}

Audio PitchCorrector::correct_to_midi(const Audio& audio, const F0Track& track,
                                      float target_midi) const {
  return correct_to_midi_timevarying(audio, track, target_midi);
}

Audio PitchCorrector::correct_to_scale(const Audio& audio, const F0Track& track) const {
  return correct_to_scale_timevarying(audio, track);
}

Audio PitchCorrector::correct_to_midi_timevarying(const Audio& audio, const F0Track& track,
                                                  float target_midi) const {
  SONARE_CHECK(std::isfinite(target_midi), ErrorCode::InvalidParameter);
  return correct_timevarying(audio, track, TargetMode::kFixedMidi, target_midi);
}

Audio PitchCorrector::correct_to_scale_timevarying(const Audio& audio, const F0Track& track) const {
  return correct_timevarying(audio, track, TargetMode::kScale, 0.0f);
}

float PitchCorrector::estimate_median_midi(const F0Track& track) const {
  SONARE_CHECK(track.f0_hz.size() == track.voiced.size(), ErrorCode::InvalidParameter);

  std::vector<float> midi_values;
  midi_values.reserve(track.f0_hz.size());
  for (int frame = 0; frame < track.n_frames(); ++frame) {
    if (valid_voiced_frame(track, frame)) {
      midi_values.push_back(hz_to_midi(track.f0_hz[static_cast<size_t>(frame)]));
    }
  }
  SONARE_CHECK(!midi_values.empty(), ErrorCode::InvalidParameter);

  std::sort(midi_values.begin(), midi_values.end());
  const size_t mid = midi_values.size() / 2;
  if (midi_values.size() % 2 == 0) {
    return 0.5f * (midi_values[mid - 1] + midi_values[mid]);
  }
  return midi_values[mid];
}

float PitchCorrector::correction_to_midi(const F0Track& track, float target_midi) const {
  SONARE_CHECK(std::isfinite(target_midi), ErrorCode::InvalidParameter);
  const float detected_midi = estimate_median_midi(track);
  return apply_limits((target_midi - detected_midi) * config_.retune_amount);
}

float PitchCorrector::correction_to_scale(const F0Track& track) const {
  const float detected_midi = estimate_median_midi(track);
  const ScaleQuantizer quantizer(config_.scale);
  return apply_limits((quantizer.quantize_midi(detected_midi) - detected_midi) *
                      config_.retune_amount);
}

float PitchCorrector::hz_to_midi(float hz) {
  SONARE_CHECK(hz > 0.0f && std::isfinite(hz), ErrorCode::InvalidParameter);
  return kMidiA4 + kSemitonesPerOctave * std::log2(hz / kA4Hz);
}

float PitchCorrector::midi_to_hz(float midi) {
  SONARE_CHECK(std::isfinite(midi), ErrorCode::InvalidParameter);
  return kA4Hz * std::pow(2.0f, (midi - kMidiA4) / kSemitonesPerOctave);
}

float PitchCorrector::apply_limits(float semitones) const noexcept {
  if (!std::isfinite(semitones)) {
    return 0.0f;
  }
  const float max_correction = std::max(0.0f, config_.max_correction_semitones);
  return std::clamp(semitones, -max_correction, max_correction);
}

// ---------------------------------------------------------------------------
// Per-frame correction pipeline
// ---------------------------------------------------------------------------

Audio PitchCorrector::correct_timevarying(const Audio& audio, const F0Track& track, TargetMode mode,
                                          float fixed_target_midi) const {
  SONARE_CHECK(track.f0_hz.size() == track.voiced.size(), ErrorCode::InvalidParameter);
  if (audio.empty() || track.n_frames() == 0) {
    return audio.to_mono();
  }
  const std::vector<float> smooth = compute_smooth_deltas(track, mode, fixed_target_midi);
  return resynthesize(audio, track, smooth);
}

// Phase 1 (per-frame target) + Phase 2 (retune IIR with vibrato bypass and unvoiced decay).
std::vector<float> PitchCorrector::compute_smooth_deltas(const F0Track& track, TargetMode mode,
                                                         float fixed_target_midi) const {
  const int n = track.n_frames();
  std::vector<float> raw(static_cast<size_t>(n), 0.0f);
  std::vector<bool> voiced(static_cast<size_t>(n), false);

  const ScaleQuantizer quantizer(config_.scale);
  for (int f = 0; f < n; ++f) {
    if (!valid_voiced_frame(track, f)) {
      continue;
    }
    voiced[static_cast<size_t>(f)] = true;
    const float current_midi = hz_to_midi(track.f0_hz[static_cast<size_t>(f)]);
    const float target_midi =
        (mode == TargetMode::kScale) ? quantizer.quantize_midi(current_midi) : fixed_target_midi;
    raw[static_cast<size_t>(f)] = target_midi - current_midi;  // semitones
  }

  // Phase 2: retune IIR. alpha derived from time constant in frames.
  const float sr = static_cast<float>(track.sample_rate);
  const float hop = static_cast<float>(std::max(1, track.hop_length));
  const float tau_frames = std::max(1e-6f, config_.retune_speed_ms * 0.001f * sr / hop);
  const float alpha = std::exp(-1.0f / tau_frames);

  const float max_corr = std::max(0.0f, config_.max_correction_semitones);
  const float vib_threshold_st = config_.vibrato_threshold_cents / kCentsPerSemitone;

  std::vector<float> smooth(static_cast<size_t>(n), 0.0f);
  float prev = 0.0f;
  for (int f = 0; f < n; ++f) {
    const size_t i = static_cast<size_t>(f);
    if (!voiced[i]) {
      prev = alpha * prev;  // decay toward zero through unvoiced regions
      smooth[i] = prev;
      continue;
    }
    float target_delta = raw[i];
    // Vibrato bypass: small deviations are mostly natural pitch, so preserve
    // the original (no correction). Larger deviations are genuine pitch errors,
    // pulled toward the target by retune_amount.
    if (std::abs(target_delta) < vib_threshold_st) {
      target_delta = 0.0f;  // within natural-pitch band: keep original
    } else {
      target_delta *= config_.retune_amount;  // true error: scale toward target
    }
    prev = alpha * prev + (1.0f - alpha) * target_delta;
    smooth[i] = std::clamp(prev, -max_corr, max_corr);
    prev = smooth[i];
  }
  return smooth;
}

// Phase 3: TD-PSOLA driven by a per-frame delta curve, with a spectral fallback
// for large shifts and pass-through (with cross-fade) for unvoiced regions.
Audio PitchCorrector::resynthesize(const Audio& audio, const F0Track& track,
                                   const std::vector<float>& smooth_deltas) const {
  const int n_samples = static_cast<int>(audio.size());
  const int sr = audio.sample_rate();
  const float sr_f = static_cast<float>(sr);
  const float hop = static_cast<float>(std::max(1, track.hop_length));
  const int n_frames = track.n_frames();

  const std::vector<float> input(audio.begin(), audio.end());

  // Helper: frame index (real) for a sample position.
  auto frame_at = [hop](float sample_pos) -> float { return sample_pos / hop; };

  // Linear interpolation of a per-frame curve at a sample position.
  auto interp_frame = [&](const std::vector<float>& curve, float sample_pos) -> float {
    const float ff = frame_at(sample_pos);
    int f0 = static_cast<int>(std::floor(ff));
    const float frac = ff - static_cast<float>(f0);
    f0 = std::clamp(f0, 0, n_frames - 1);
    const int f1 = std::clamp(f0 + 1, 0, n_frames - 1);
    return curve[static_cast<size_t>(f0)] * (1.0f - frac) + curve[static_cast<size_t>(f1)] * frac;
  };

  // Build a per-sample voiced flag and per-sample f0 (for epoch spacing).
  auto sample_voiced = [&](int sample_pos) -> bool {
    int f = static_cast<int>(std::lround(frame_at(static_cast<float>(sample_pos))));
    f = std::clamp(f, 0, n_frames - 1);
    return valid_voiced_frame(track, f);
  };
  auto sample_f0 = [&](float sample_pos) -> float {
    int f = static_cast<int>(std::lround(frame_at(sample_pos)));
    f = std::clamp(f, 0, n_frames - 1);
    const float hz = track.f0_hz[static_cast<size_t>(f)];
    return (hz > 0.0f && std::isfinite(hz)) ? hz : 0.0f;
  };

  std::vector<float> out(static_cast<size_t>(n_samples), 0.0f);
  std::vector<float> norm(static_cast<size_t>(n_samples), 0.0f);

  // TD-PSOLA over voiced regions. We walk input epochs spaced by the local
  // period and place Hann-windowed two-period grains at output epochs whose
  // spacing is shortened/lengthened by the interpolated correction.
  float input_epoch = 0.0f;
  float output_epoch = 0.0f;
  bool have_psola = false;

  while (input_epoch < static_cast<float>(n_samples)) {
    const int center = static_cast<int>(std::lround(input_epoch));
    if (!sample_voiced(center)) {
      // Skip ahead one default hop worth; unvoiced handled separately below.
      input_epoch += hop;
      output_epoch = input_epoch;  // keep timelines aligned across gaps
      continue;
    }
    const float f0 = sample_f0(input_epoch);
    if (f0 <= 0.0f) {
      input_epoch += hop;
      output_epoch = input_epoch;
      continue;
    }
    const float period_in = sr_f / f0;
    const float delta = interp_frame(smooth_deltas, input_epoch);

    // Large shifts: leave to the spectral fallback pass (skip here).
    if (std::abs(delta) > kPsolaMaxSemitones) {
      input_epoch += period_in;
      output_epoch += period_in;
      continue;
    }
    have_psola = true;

    const float ratio = std::pow(2.0f, delta / kSemitonesPerOctave);
    const float period_out = period_in / ratio;  // higher pitch -> shorter period

    // Two-period Hann grain centered at the input epoch.
    const int half = std::max(1, static_cast<int>(std::lround(period_in)));
    const int grain_len = 2 * half + 1;
    const int out_center = static_cast<int>(std::lround(output_epoch));
    for (int k = 0; k < grain_len; ++k) {
      const int src = center - half + k;
      if (src < 0 || src >= n_samples) {
        continue;
      }
      const int dst = out_center - half + k;
      if (dst < 0 || dst >= n_samples) {
        continue;
      }
      const float w = hann(static_cast<float>(k) / static_cast<float>(grain_len - 1));
      out[static_cast<size_t>(dst)] += w * input[static_cast<size_t>(src)];
      norm[static_cast<size_t>(dst)] += w;
    }

    input_epoch += period_in;
    output_epoch += std::max(1.0f, period_out);
  }

  // Normalize the PSOLA overlap-add result.
  for (int i = 0; i < n_samples; ++i) {
    const size_t idx = static_cast<size_t>(i);
    if (norm[idx] > kSpectrumEpsilon) {
      out[idx] /= norm[idx];
    }
  }

  // Fallback / pass-through pass: any sample not covered by PSOLA (unvoiced
  // regions, large-shift regions) is filled from a spectral shift or the
  // dry signal, then cross-faded against the PSOLA output near boundaries.
  Audio fallback_audio;
  bool have_fallback = false;
  {
    // Median delta over large-shift voiced frames drives a single spectral shift.
    std::vector<float> big;
    for (int f = 0; f < n_frames; ++f) {
      if (valid_voiced_frame(track, f) &&
          std::abs(smooth_deltas[static_cast<size_t>(f)]) > kPsolaMaxSemitones) {
        big.push_back(smooth_deltas[static_cast<size_t>(f)]);
      }
    }
    if (!big.empty()) {
      std::sort(big.begin(), big.end());
      const float med = big[big.size() / 2];
      PitchShiftConfig shift_config;
      shift_config.backend = config_.backend;
      fallback_audio = pitch_shift(audio, apply_limits(med), shift_config);
      have_fallback = true;
    }
  }

  const int xfade = std::max(1, static_cast<int>(std::lround(kCrossfadeMs * 0.001f * sr_f)));
  std::vector<float> result(static_cast<size_t>(n_samples), 0.0f);
  for (int i = 0; i < n_samples; ++i) {
    const size_t idx = static_cast<size_t>(i);
    const bool psola_here = have_psola && norm[idx] > kSpectrumEpsilon;
    float dry;
    if (have_fallback && i < static_cast<int>(fallback_audio.size())) {
      dry = fallback_audio[idx];
    } else {
      dry = input[idx];
    }
    if (!psola_here) {
      result[idx] = dry;
      continue;
    }
    // Cross-fade weight ramps from dry to PSOLA across boundary samples.
    float w = 1.0f;
    int run_back = 0;
    while (run_back < xfade && i - run_back - 1 >= 0 &&
           norm[static_cast<size_t>(i - run_back - 1)] > kSpectrumEpsilon) {
      ++run_back;
    }
    int run_fwd = 0;
    while (run_fwd < xfade && i + run_fwd + 1 < n_samples &&
           norm[static_cast<size_t>(i + run_fwd + 1)] > kSpectrumEpsilon) {
      ++run_fwd;
    }
    const int edge = std::min(run_back, run_fwd);
    if (edge < xfade) {
      w = static_cast<float>(edge) / static_cast<float>(xfade);
    }
    result[idx] = w * out[idx] + (1.0f - w) * dry;
  }

  // The PSOLA path is OLA-normalized and stays in range, but the spectral
  // fallback can overshoot. Peak-normalize (instead of hard-clipping) so a
  // large-shift fallback region does not introduce clipping distortion.
  if (have_fallback) {
    float peak = 0.0f;
    for (const float s : result) {
      peak = std::max(peak, std::abs(s));
    }
    if (peak > 1.0f) {
      const float gain = 1.0f / peak;
      for (float& s : result) {
        s *= gain;
      }
    }
  }
  return Audio::from_vector(std::move(result), sr);
}

}  // namespace sonare::editing::pitch_editor
