#include "effects/pitch_shift.h"

#include <cmath>

#include "core/resample.h"
#include "effects/native_spectral_stretch.h"
#include "effects/time_stretch.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

Audio pitch_shift(const Audio& audio, float semitones, const PitchShiftConfig& config) {
  /// Convert semitones to frequency ratio
  float ratio = std::pow(2.0f, semitones / constants::kSemitonesPerOctave);
  return pitch_shift_ratio(audio, ratio, config);
}

Audio pitch_shift_ratio(const Audio& audio, float ratio, const PitchShiftConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(ratio > 0.0f, ErrorCode::InvalidParameter);

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
  int effective_sr = static_cast<int>(std::round(static_cast<float>(original_sr) * ratio));

  /// The resample step treats the stretched signal as if sampled at sr*ratio.
  /// If that effective rate falls outside the supported resampler range, the
  /// old code silently clamped it, which changed the effective ratio and
  /// returned wrong-pitch audio. Reject such ratios explicitly instead so the
  /// caller learns the request is unsupported rather than getting bad output.
  /// (In-range ratios — roughly +/-2 octaves at 44.1/48 kHz — are unaffected.)
  constexpr int kMinEffectiveSr = 1000;
  constexpr int kMaxEffectiveSr = 192000;
  if (effective_sr < kMinEffectiveSr || effective_sr > kMaxEffectiveSr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "pitch_shift: ratio out of supported range for this sample rate");
  }

  /// Single resample from effective rate to original rate
  std::vector<float> result_samples =
      resample(stretched.data(), stretched.size(), effective_sr, original_sr);

  return Audio::from_vector(std::move(result_samples), original_sr);
}

}  // namespace sonare
