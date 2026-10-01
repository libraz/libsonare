#include "editing/pitch_editor/pitch_corrector.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "core/convert.h"
#include "effects/pitch_shift.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::editing::pitch_editor {

using constants::kCentsPerSemitone;
using constants::kSemitonesPerOctave;
using constants::kSpectrumEpsilon;
using constants::kTwoPi;

namespace {

bool valid_voiced_frame(const F0Track& track, int frame) {
  const size_t index = static_cast<size_t>(frame);
  return index < track.f0_hz.size() && index < track.voiced.size() && track.voiced[index] &&
         track.f0_hz[index] > 0.0f && std::isfinite(track.f0_hz[index]);
}

// Each PSOLA pass stays within this interval. Larger caller requests are split
// into several passes by PitchCorrector::resynthesize.
constexpr float kPsolaMaxSemitones = 6.0f;
// Cross-fade duration at voiced/unvoiced boundaries.
constexpr float kCrossfadeMs = 10.0f;

// Hann window value at normalized position t in [0, 1].
float hann(float t) noexcept { return 0.5f - 0.5f * std::cos(kTwoPi * t); }

void validate_f0_track(const Audio& audio, const F0Track& track) {
  // Validate in the core so every public surface inherits the same track contract.
  SONARE_CHECK(!track.f0_hz.empty() && track.hop_length > 0 &&
                   track.f0_hz.size() == track.voiced.size() &&
                   (track.voiced_prob.empty() || track.voiced_prob.size() == track.f0_hz.size()),
               ErrorCode::InvalidParameter);
  // hop_length is in samples at the track's rate; a rate mismatch would
  // silently apply the correction curve at the wrong time positions.
  SONARE_CHECK(track.sample_rate == audio.sample_rate(), ErrorCode::InvalidParameter);
  const double samples_per_frame = track.samples_per_frame();
  SONARE_CHECK(std::isfinite(samples_per_frame) && samples_per_frame >= 1.0,
               ErrorCode::InvalidParameter);

  const float nyquist = 0.5f * static_cast<float>(audio.sample_rate());
  for (size_t i = 0; i < track.f0_hz.size(); ++i) {
    const float f0 = track.f0_hz[i];
    if (track.voiced[i]) {
      SONARE_CHECK(std::isfinite(f0) && f0 > 0.0f && f0 <= nyquist, ErrorCode::InvalidParameter);
    } else {
      // pYIN uses NaN for unvoiced F0. A finite non-negative value is also
      // accepted for host-supplied tracks because voiced[] is authoritative.
      SONARE_CHECK((std::isnan(f0) || (std::isfinite(f0) && f0 >= 0.0f && f0 <= nyquist)),
                   ErrorCode::InvalidParameter);
    }
  }
  for (const float probability : track.voiced_prob) {
    SONARE_CHECK(std::isfinite(probability) && probability >= 0.0f && probability <= 1.0f,
                 ErrorCode::InvalidParameter);
  }
}

int rounded_sample(double position, int n_samples) noexcept {
  if (!(position > 0.0)) return 0;
  const double rounded = std::round(position);
  if (rounded >= static_cast<double>(n_samples - 1)) return n_samples - 1;
  return static_cast<int>(rounded);
}

// Return the larger adjacent float gap around a stored value.
double float_spacing(float value) noexcept {
  double spacing = 0.0;
  for (const float direction :
       {-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()}) {
    const float neighbour = std::nextafter(value, direction);
    if (std::isfinite(neighbour)) {
      spacing = std::max(spacing, std::abs(static_cast<double>(neighbour) - value));
    }
  }
  return spacing;
}

// Return the larger log2 gap represented by adjacent positive floats.
double log2_spacing(float value) noexcept {
  const double encoded_log = std::log2(static_cast<double>(value));
  double spacing = 0.0;
  for (const float direction :
       {-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()}) {
    const float neighbour = std::nextafter(value, direction);
    if (neighbour > 0.0f && std::isfinite(neighbour)) {
      spacing =
          std::max(spacing, std::abs(std::log2(static_cast<double>(neighbour)) - encoded_log));
    }
  }
  return spacing;
}

// Budget the rounding steps used to encode a requested endpoint.
double representability_tolerance(double bound, float source_hz, float delta) noexcept {
  const float bound_hz = static_cast<float>(std::exp2(bound));
  const double encoding_error = std::abs(bound - std::log2(static_cast<double>(bound_hz)));
  if (source_hz == 0.0f) return encoding_error + log2_spacing(bound_hz);

  // Budget rounding in the float endpoint expression: 12 * log2(bound_hz / source_hz).
  const float ratio = bound_hz / source_hz;
  const float octaves = std::log2(ratio);
  return encoding_error + log2_spacing(ratio) + float_spacing(octaves) +
         float_spacing(delta) / kSemitonesPerOctave;
}

bool clamp_log2_to_bounds(double& value, double lower, double upper, float source_hz = 0.0f,
                          float delta = 0.0f) noexcept {
  if (!std::isfinite(value)) return false;
  if (value < lower) {
    if (lower - value > representability_tolerance(lower, source_hz, delta)) return false;
    value = lower;
  } else if (value > upper) {
    if (value - upper > representability_tolerance(upper, source_hz, delta)) return false;
    value = upper;
  }
  return true;
}

}  // namespace

PitchCorrector::PitchCorrector(PitchCorrectionConfig config) : config_(config) {
  SONARE_CHECK(
      valid_scale_args(config_.scale.root, config_.scale.mode_mask) &&
          std::isfinite(config_.scale.reference_midi) && config_.scale.reference_midi >= 0.0f &&
          config_.scale.reference_midi <= kMaxReferenceMidi &&
          std::isfinite(config_.retune_amount) && config_.retune_amount >= 0.0f &&
          config_.retune_amount <= 1.0f && std::isfinite(config_.max_correction_semitones) &&
          config_.max_correction_semitones >= 0.0f && std::isfinite(config_.retune_speed_ms) &&
          config_.retune_speed_ms >= 0.0f && std::isfinite(config_.vibrato_threshold_cents) &&
          config_.vibrato_threshold_cents >= 0.0f,
      ErrorCode::InvalidParameter);
}

Audio PitchCorrector::shift(const Audio& audio, float semitones) const {
  // NOT clamped by max_correction_semitones. That limit bounds how far the
  // RETUNE paths may drag a MEASURED pitch toward a target the caller did not
  // state; here the caller states the interval outright, so clamping it returns
  // a different transposition than the one asked for and reports success. The
  // clamp stays on correction_to_midi / correction_to_scale and the per-frame
  // pipeline, which is what the config field is named for.
  PitchShiftConfig shift_config;
  shift_config.backend = config_.backend;
  return pitch_shift(audio, std::isfinite(semitones) ? semitones : 0.0f, shift_config);
}

Audio PitchCorrector::correct_to_midi(const Audio& audio, float current_midi,
                                      float target_midi) const {
  SONARE_CHECK(std::isfinite(current_midi) && current_midi >= 0.0f && current_midi <= 127.0f &&
                   std::isfinite(target_midi) && target_midi >= 0.0f && target_midi <= 127.0f,
               ErrorCode::InvalidParameter);
  const Audio shifted = shift(audio, target_midi - current_midi);
  if (shifted.size() == audio.size()) {
    return shifted;
  }

  // A spectral pitch shift can round the reconstructed extent by one sample for
  // some buffer sizes. Constant correction is duration-preserving, so normalize
  // that backend detail at this shared API boundary.
  std::vector<float> samples(audio.size(), 0.0f);
  std::copy_n(shifted.begin(), std::min(shifted.size(), samples.size()), samples.begin());
  return Audio::from_vector(std::move(samples), audio.sample_rate());
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
  // Reject a non-finite or out-of-range target so a bad value cannot turn into a
  // garbage shift. Validated here in the core so every surface (C ABI, Node,
  // Python, WASM) inherits the same contract rather than each re-checking.
  SONARE_CHECK(std::isfinite(target_midi) && target_midi >= 0.0f && target_midi <= 127.0f,
               ErrorCode::InvalidParameter);
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

  return sonare::median(midi_values.data(), midi_values.size());
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

float PitchCorrector::hz_to_midi(float hz) { return sonare::hz_to_midi(hz); }

float PitchCorrector::midi_to_hz(float midi) { return sonare::midi_to_hz(midi); }

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
  validate_f0_track(audio, track);
  if (audio.empty()) {
    return audio;
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
    // voiced_prob is NOT a correction weight. It used to scale the per-frame
    // correction amount, which silently made the correction fall short of its
    // target whenever the caller passed a probability track: pYIN's
    // voiced_prob is a frame's voiced observation mass, so it varies with F0
    // and frame length rather than with confidence, and a low-register note
    // barely moved. Voicing is decided by `voiced` alone (see
    // valid_voiced_frame); voiced_prob only derives that flag when the caller
    // supplies no explicit one.
    raw[static_cast<size_t>(f)] = target_midi - current_midi;  // semitones
  }

  // Phase 2: retune IIR. alpha derived from time constant in frames. The
  // frames-per-second factor is the track's own cadence (F0Track::frame_rate),
  // not a second sample_rate / hop_length derivation: a host-supplied
  // frame_rate_hz would otherwise set the retune time constant by a cadence the
  // track does not actually have.
  const float tau_frames = std::max(1e-6f, config_.retune_speed_ms * 0.001f * track.frame_rate());
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

namespace {

// One bounded TD-PSOLA pass. Public resynthesize validates the complete curve
// and splits requests larger than kPsolaMaxSemitones before calling this helper.
Audio resynthesize_psola_pass(const Audio& audio, const F0Track& track,
                              const std::vector<float>& deltas_semitones) {
  SONARE_CHECK(audio.size() <= static_cast<size_t>(std::numeric_limits<int>::max()),
               ErrorCode::InvalidParameter);
  const int n_samples = static_cast<int>(audio.size());
  const int sr = audio.sample_rate();
  const double sr_f = static_cast<double>(sr);
  // Same cadence rule as every other frame<->sample conversion on this track.
  // double: this is the unit frame_at converts to/from, and frame_at feeds the
  // grain clock below, which must not lose precision past 2^24 samples.
  const double hop = std::max(1.0, static_cast<double>(track.samples_per_frame()));
  const int n_frames = track.n_frames();

  const float* input = audio.data();

  // Helper: frame index (real) for a sample position. double throughout: this
  // sits directly under the grain clock (output_epoch/analysis_epoch), whose
  // own accumulation is why this function takes and returns double rather
  // than float -- see the comment above those two variables below.
  auto frame_at = [hop](double sample_pos) -> double { return sample_pos / hop; };

  auto nearest_frame = [&](double sample_pos) -> int {
    int frame = rounded_sample(frame_at(sample_pos), n_samples);
    return std::clamp(frame, 0, n_frames - 1);
  };

  // Hold an active frame at a dry boundary instead of tapering its correction through dry audio.
  auto interp_frame = [&](const std::vector<float>& curve, double sample_pos) -> float {
    const double ff = frame_at(sample_pos);
    int f0 = static_cast<int>(std::floor(ff));
    f0 = std::clamp(f0, 0, n_frames - 1);
    const int f1 = std::clamp(f0 + 1, 0, n_frames - 1);
    const float frac = static_cast<float>(ff - static_cast<double>(f0));
    const bool first_active = valid_voiced_frame(track, f0);
    const bool second_active = valid_voiced_frame(track, f1);
    float value = 0.0f;
    if (first_active && second_active) {
      const float first = curve[static_cast<size_t>(f0)];
      const float second = curve[static_cast<size_t>(f1)];
      // a+(b-a)*t keeps equal ±6 endpoints exact; FMA can make a*(1-t)+b*t overshoot by one ULP.
      value = first + (second - first) * frac;
    } else if (first_active) {
      value = curve[static_cast<size_t>(f0)];
    } else if (second_active) {
      value = curve[static_cast<size_t>(f1)];
    }
    return std::clamp(value, -kPsolaMaxSemitones, kPsolaMaxSemitones);
  };

  // Build a per-sample voiced flag and per-sample f0 (for epoch spacing).
  auto sample_voiced = [&](int sample_pos) -> bool {
    return valid_voiced_frame(track, nearest_frame(static_cast<double>(sample_pos)));
  };
  auto sample_f0 = [&](double sample_pos) -> float {
    const int f = nearest_frame(sample_pos);
    if (!valid_voiced_frame(track, f)) return 0.0f;
    const float hz = track.f0_hz[static_cast<size_t>(f)];
    return hz;
  };

  std::vector<float> out(static_cast<size_t>(n_samples), 0.0f);
  std::vector<float> norm(static_cast<size_t>(n_samples), 0.0f);

  // TD-PSOLA over voiced regions, driven by the SYNTHESIS (output) timeline.
  //
  // The output epoch is the master clock: it advances by the (pitch-shifted)
  // local period_out each grain. The source/analysis center is taken from a
  // separately tracked analysis pointer that advances by period_in only as the
  // synthesis position catches up to it, so synthesis time stays aligned with
  // input time (analysis_epoch ~= output_epoch). This is standard PSOLA grain
  // duplication when ratio > 1 (output period shorter, so the same source epoch
  // feeds several grains) and grain skipping when ratio < 1 (output period
  // longer, so the analysis pointer steps over epochs). The result is
  // duration-preserving: a region of input duration D maps to output duration D,
  // only the pitch changes. Each grain reads its period/correction from the
  // analysis center so the output stays time-locked to the input pitch contour.
  // double, not float: float's 24-bit mantissa can only represent whole-2
  // increments past 2^24 samples (~5.8 min at 48 kHz) and whole-4 past 2^25,
  // so a non-integer period_in/period_out accumulated here in float rounds to
  // that grid every grain -- a directional, growing pitch and timing drift on
  // long audio, not a zero-mean rounding error. double's 53-bit mantissa holds
  // sub-sample precision to roughly 2^52 samples, well past any real buffer.
  double output_epoch = 0.0;
  double analysis_epoch = 0.0;
  bool have_psola = false;

  while (output_epoch < static_cast<double>(n_samples)) {
    // Keep the analysis pointer time-aligned with the synthesis position: it
    // marks the input sample whose pitch period we are about to reproduce.
    const int center = rounded_sample(analysis_epoch, n_samples);
    if (!sample_voiced(center)) {
      // Dry frames retain the input in the output blend; advance both timelines
      // together (no pitch change) so they re-sync across the gap.
      output_epoch += hop;
      analysis_epoch += hop;
      continue;
    }
    const float f0 = sample_f0(analysis_epoch);
    if (f0 <= 0.0f) {
      output_epoch += hop;
      analysis_epoch += hop;
      continue;
    }
    const double period_in = std::max(1.0, sr_f / static_cast<double>(f0));
    const float delta = interp_frame(deltas_semitones, analysis_epoch);

    // Public resynthesize splits every request before reaching this helper.
    // Keep this guard as a local invariant in case a future caller bypasses
    // that boundary.
    SONARE_CHECK(std::isfinite(delta) && std::abs(delta) <= kPsolaMaxSemitones,
                 ErrorCode::InvalidParameter);
    have_psola = true;

    const double ratio = std::exp2(static_cast<double>(delta) / kSemitonesPerOctave);
    const double period_out = std::max(1.0, period_in / ratio);  // higher pitch -> shorter period

    // Two-period Hann grain copied from the analysis center to the output
    // epoch. Source and destination share the same half-width so the grain is
    // pitch-shifted (spacing changes) but not time-stretched.
    const int max_half = std::max(1, (n_samples - 1) / 2);
    const long long rounded_period = std::llround(period_in);
    const int half =
        static_cast<int>(std::clamp(rounded_period, 1LL, static_cast<long long>(max_half)));
    const int grain_len = 2 * half + 1;
    const int out_center = rounded_sample(output_epoch, n_samples);
    for (int k = 0; k < grain_len; ++k) {
      const int src = center - half + k;
      if (src < 0 || src >= n_samples) {
        continue;
      }
      const int dst = out_center - half + k;
      if (dst < 0 || dst >= n_samples) {
        continue;
      }
      // A grain centred at the edge of an active run can extend into an
      // unvoiced or ineligible run. Never let that source grain repitch dry
      // output; the boundary cross-fade below then fades the active result to
      // the untouched input on its own side of the nearest-frame boundary.
      if (!sample_voiced(dst)) {
        continue;
      }
      const float w = hann(static_cast<float>(k) / static_cast<float>(grain_len - 1));
      out[static_cast<size_t>(dst)] += w * input[static_cast<size_t>(src)];
      norm[static_cast<size_t>(dst)] += w;
    }

    // Output advances by the synthesis period; the analysis pointer advances by
    // input periods only while it lags the synthesis clock. When ratio > 1 the
    // output period is shorter, so several output grains may reuse one analysis
    // epoch before it steps (grain duplication); when ratio < 1 the analysis
    // pointer skips epochs to keep pace (grain skipping). Either way the two
    // timelines track each other, preserving duration.
    output_epoch += period_out;
    for (;;) {
      const float next_f0 = sample_f0(analysis_epoch);
      if (next_f0 <= 0.0f) {
        break;  // next epoch is unvoiced: let the outer hop logic re-sync.
      }
      const double next_period_in = std::max(1.0, sr_f / static_cast<double>(next_f0));
      // Map the synthesis epoch to the NEAREST analysis pitch mark, not the
      // floor. Stepping only while the next mark stays at least half a period
      // below the synthesis clock keeps |analysis_epoch - output_epoch| within
      // half a period, so each grain is time-centred on the synthesis position
      // with no systematic bias. A floor comparison (analysis + period >
      // output) would leave the source up to a full period behind the output,
      // shifting envelope features forward by ~half a period and reintroducing
      // duration drift.
      if (analysis_epoch + 0.5f * next_period_in >= output_epoch) {
        break;  // nearest analysis mark reached; advancing would overshoot.
      }
      analysis_epoch += next_period_in;
    }
  }

  // Normalize the PSOLA overlap-add result.
  for (int i = 0; i < n_samples; ++i) {
    const size_t idx = static_cast<size_t>(i);
    if (norm[idx] > kSpectrumEpsilon) {
      out[idx] /= norm[idx];
    }
  }

  const int xfade = std::max(1, static_cast<int>(std::lround(kCrossfadeMs * 0.001f * sr_f)));
  std::vector<float> result(static_cast<size_t>(n_samples), 0.0f);
  for (int i = 0; i < n_samples; ++i) {
    const size_t idx = static_cast<size_t>(i);
    const bool psola_here = have_psola && norm[idx] > kSpectrumEpsilon;
    if (!psola_here) {
      result[idx] = input[idx];
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
    result[idx] = w * out[idx] + (1.0f - w) * input[idx];
  }

  return Audio::from_vector(std::move(result), sr);
}

}  // namespace

Audio PitchCorrector::resynthesize(const Audio& audio, const F0Track& track,
                                   const std::vector<float>& deltas_semitones) const {
  SONARE_CHECK(!audio.empty() && track.n_frames() > 0, ErrorCode::InvalidParameter);
  // One delta per track frame prevents interpolation from reading past the correction curve.
  SONARE_CHECK(deltas_semitones.size() == static_cast<size_t>(track.n_frames()),
               ErrorCode::InvalidParameter);
  SONARE_CHECK(audio.size() <= static_cast<size_t>(std::numeric_limits<int>::max()),
               ErrorCode::InvalidParameter);
  validate_f0_track(audio, track);

  const int n_samples = static_cast<int>(audio.size());
  const double sample_rate = static_cast<double>(audio.sample_rate());
  const double min_target_log2 = std::log2(sample_rate / static_cast<double>(n_samples));
  const double max_target_log2 = std::log2(0.5 * sample_rate);
  const int n_frames = track.n_frames();

  // Normalize dry-frame deltas to zero so arbitrary values cannot bleed into eligible grains.
  std::vector<float> effective_deltas(static_cast<size_t>(n_frames), 0.0f);
  std::vector<double> source_log2(static_cast<size_t>(n_frames), 0.0);
  std::vector<bool> eligible(static_cast<size_t>(n_frames), false);
  double max_abs_delta = 0.0;
  for (int frame = 0; frame < n_frames; ++frame) {
    const size_t index = static_cast<size_t>(frame);
    const float requested = deltas_semitones[index];
    SONARE_CHECK(std::isfinite(requested), ErrorCode::InvalidParameter);
    if (!track.voiced[index]) continue;

    double source = std::log2(static_cast<double>(track.f0_hz[index]));
    // Leave overlong source periods dry; validate_f0_track already rejected malformed F0.
    if (!clamp_log2_to_bounds(source, min_target_log2, max_target_log2)) continue;

    double target = source + static_cast<double>(requested) / kSemitonesPerOctave;
    // Dry-pass out-of-range targets before calculating the number of passes.
    if (!clamp_log2_to_bounds(target, min_target_log2, max_target_log2, track.f0_hz[index],
                              requested))
      continue;

    const double effective = (target - source) * kSemitonesPerOctave;
    if (!std::isfinite(effective)) continue;
    source_log2[index] = source;
    effective_deltas[index] = static_cast<float>(effective);
    eligible[index] = true;
    max_abs_delta = std::max(max_abs_delta, std::abs(effective));
  }

  // Complete validation before the identity return so malformed input cannot silently succeed.
  if (!(max_abs_delta > 0.0)) return audio;

  const double pass_count = std::ceil(max_abs_delta / static_cast<double>(kPsolaMaxSemitones));
  SONARE_CHECK(std::isfinite(pass_count) &&
                   pass_count <= static_cast<double>(std::numeric_limits<int>::max()),
               ErrorCode::InvalidParameter);
  const int passes = std::max(1, static_cast<int>(pass_count));
  const double pass_scale = 1.0 / static_cast<double>(passes);

  Audio working = audio;
  F0Track working_track = track;
  std::vector<float> pass_deltas(static_cast<size_t>(n_frames), 0.0f);
  for (int frame = 0; frame < n_frames; ++frame) {
    working_track.voiced[static_cast<size_t>(frame)] = eligible[static_cast<size_t>(frame)];
  }
  for (int pass = 0; pass < passes; ++pass) {
    const double progress = static_cast<double>(pass) * pass_scale;
    for (int frame = 0; frame < n_frames; ++frame) {
      const size_t index = static_cast<size_t>(frame);
      if (!eligible[index]) {
        working_track.voiced[index] = false;
        pass_deltas[index] = 0.0f;
        continue;
      }

      double current_log2 = source_log2[index] + progress *
                                                     static_cast<double>(effective_deltas[index]) /
                                                     kSemitonesPerOctave;
      // Clamp intermediate F0 rounding at the inclusive synthesis bounds.
      SONARE_CHECK(std::isfinite(current_log2), ErrorCode::InvalidParameter);
      current_log2 = std::clamp(current_log2, min_target_log2, max_target_log2);
      const double current_hz = std::exp2(current_log2);
      SONARE_CHECK(std::isfinite(current_hz) && current_hz > 0.0, ErrorCode::InvalidParameter);
      working_track.voiced[index] = true;
      working_track.f0_hz[index] = static_cast<float>(current_hz);
      const float pass_delta =
          static_cast<float>(static_cast<double>(effective_deltas[index]) * pass_scale);
      pass_deltas[index] = std::clamp(pass_delta, -kPsolaMaxSemitones, kPsolaMaxSemitones);
    }
    working = resynthesize_psola_pass(working, working_track, pass_deltas);
  }
  return working;
}

}  // namespace sonare::editing::pitch_editor
