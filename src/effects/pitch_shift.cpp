#include "effects/pitch_shift.h"

#include <algorithm>
#include <climits>
#include <cmath>

#include "core/resample.h"
#include "effects/native_spectral_stretch.h"
#include "effects/time_stretch.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare {

bool make_pitch_shift_ratio_plan(std::size_t input_samples, int sample_rate, float ratio,
                                 PitchShiftPlan* out) noexcept {
  if (out == nullptr || input_samples == 0 || input_samples > kMaxAudioBufferSize ||
      sample_rate <= 0 || !numeric::finite_positive(ratio)) {
    return false;
  }

  const long double effective_sr_wide =
      std::round(static_cast<long double>(sample_rate) * static_cast<long double>(ratio));
  constexpr int kMinEffectiveSr = 1000;
  constexpr int kMaxEffectiveSr = 192000;
  if (!std::isfinite(effective_sr_wide) || effective_sr_wide < kMinEffectiveSr ||
      effective_sr_wide > kMaxEffectiveSr) {
    return false;
  }

  constexpr std::size_t kMaxStretchedSamples =
      std::min<std::size_t>(kMaxAudioBufferSize, static_cast<std::size_t>(INT_MAX));
  std::size_t stretched_samples = 0;
  const long double stretch_rate = 1.0L / static_cast<long double>(ratio);
  if (!numeric::checked_projected_count(input_samples, stretch_rate, kMaxStretchedSamples,
                                        &stretched_samples)) {
    return false;
  }

  *out = {ratio, static_cast<int>(effective_sr_wide), stretched_samples};
  return true;
}

bool make_pitch_shift_plan(std::size_t input_samples, int sample_rate, float semitones,
                           PitchShiftPlan* out) noexcept {
  if (!numeric::finite(semitones)) return false;
  const float ratio =
      std::pow(2.0f, semitones / static_cast<float>(constants::kSemitonesPerOctave));
  return make_pitch_shift_ratio_plan(input_samples, sample_rate, ratio, out);
}

Audio pitch_shift(const Audio& audio, float semitones, const PitchShiftConfig& config) {
  PitchShiftPlan plan;
  SONARE_CHECK(make_pitch_shift_plan(audio.size(), audio.sample_rate(), semitones, &plan),
               ErrorCode::InvalidParameter);
  return pitch_shift_ratio(audio, plan.ratio, config);
}

Audio pitch_shift_ratio(const Audio& audio, float ratio, const PitchShiftConfig& config) {
  PitchShiftPlan plan;
  SONARE_CHECK(make_pitch_shift_ratio_plan(audio.size(), audio.sample_rate(), ratio, &plan),
               ErrorCode::InvalidParameter);

  if (config.backend == StretchBackend::NativeSpectral) {
    return native_spectral_pitch_shift_ratio(audio, ratio, config.n_fft, config.hop_length);
  }

  /// @details Pitch shifting = time stretch + resample (librosa-compatible:
  /// pitch changes, duration is preserved). To raise pitch by ratio R:
  /// 1. Time-stretch by 1/R so the result is R times LONGER (same pitch).
  /// 2. Resample as if it were sampled at sr*R back to sr: this plays it R
  ///    times faster, raising pitch by R and restoring the original length.

  /// Time stretch configuration
  TimeStretchConfig ts_config;
  ts_config.n_fft = config.n_fft;
  ts_config.hop_length = config.hop_length;
  ts_config.backend = StretchBackend::PhaseVocoder;

  /// Step 1: Time stretch longer by 1/ratio (preserves pitch).
  Audio stretched = time_stretch(audio, 1.0f / ratio, ts_config);

  /// Step 2: Resample the stretched signal (treated as if sampled at sr*ratio)
  /// back to the original sample rate. Length: (N*ratio) * sr/(sr*ratio) = N.
  int original_sr = audio.sample_rate();
  /// The resample step treats the stretched signal as if sampled at sr*ratio.
  /// If that effective rate falls outside the supported resampler range, the
  /// old code silently clamped it, which changed the effective ratio and
  /// returned wrong-pitch audio. Reject such ratios explicitly instead so the
  /// caller learns the request is unsupported rather than getting bad output.
  /// (In-range ratios — roughly +/-2 octaves at 44.1/48 kHz — are unaffected.)
  /// Single resample from effective rate to original rate
  std::vector<float> result_samples =
      resample(stretched.data(), stretched.size(), plan.effective_sample_rate, original_sr);

  return Audio::from_vector(std::move(result_samples), original_sr);
}

}  // namespace sonare
