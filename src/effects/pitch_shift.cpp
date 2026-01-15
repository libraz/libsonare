#include "effects/pitch_shift.h"

#include <cmath>

#include "core/resample.h"
#include "effects/time_stretch.h"
#include "util/exception.h"

namespace sonare {

Audio pitch_shift(const Audio& audio, float semitones, const PitchShiftConfig& config) {
  /// Convert semitones to frequency ratio
  float ratio = std::pow(2.0f, semitones / 12.0f);
  return pitch_shift_ratio(audio, ratio, config);
}

Audio pitch_shift_ratio(const Audio& audio, float ratio, const PitchShiftConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(ratio > 0.0f, ErrorCode::InvalidParameter);

  /// @details Pitch shifting = time stretch + resample.
  /// To raise pitch by ratio R:
  /// 1. Time stretch by factor R (makes it R times shorter)
  /// 2. Resample to restore original duration

  /// Time stretch configuration
  TimeStretchConfig ts_config;
  ts_config.n_fft = config.n_fft;
  ts_config.hop_length = config.hop_length;

  /// Step 1: Time stretch (rate = ratio means shorter duration)
  Audio stretched = time_stretch(audio, ratio, ts_config);

  /// Step 2: Resample back to original length in a single step.
  /// After time stretch by ratio, we have ~1/ratio of original samples.
  /// To restore original duration, we treat stretched audio as if it has
  /// an effective sample rate of (original_sr * ratio), then resample to original_sr.
  /// This achieves the same result as two resamples but in one pass.
  int original_sr = audio.sample_rate();
  int effective_sr = static_cast<int>(std::round(static_cast<float>(original_sr) * ratio));

  /// Clamp effective sample rate to reasonable bounds
  if (effective_sr < 1000) effective_sr = 1000;
  if (effective_sr > 192000) effective_sr = 192000;

  /// Single resample from effective rate to original rate
  std::vector<float> result_samples =
      resample(stretched.data(), stretched.size(), effective_sr, original_sr);

  return Audio::from_vector(std::move(result_samples), original_sr);
}

}  // namespace sonare
