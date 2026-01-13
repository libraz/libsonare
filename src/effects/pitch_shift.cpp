#include "effects/pitch_shift.h"

#include <cmath>

#include "core/resample.h"
#include "effects/time_stretch.h"
#include "util/exception.h"

namespace sonare {

Audio pitch_shift(const Audio& audio, float semitones, const PitchShiftConfig& config) {
  // Convert semitones to frequency ratio
  float ratio = std::pow(2.0f, semitones / 12.0f);
  return pitch_shift_ratio(audio, ratio, config);
}

Audio pitch_shift_ratio(const Audio& audio, float ratio, const PitchShiftConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(ratio > 0.0f, ErrorCode::InvalidParameter);

  // Pitch shifting = time stretch + resample
  // To raise pitch by ratio R:
  // 1. Time stretch by factor R (makes it R times shorter)
  // 2. Resample to original sample rate (stretches it back)

  // Time stretch configuration
  TimeStretchConfig ts_config;
  ts_config.n_fft = config.n_fft;
  ts_config.hop_length = config.hop_length;

  // Step 1: Time stretch (rate = ratio means shorter duration)
  Audio stretched = time_stretch(audio, ratio, ts_config);

  // Step 2: Resample back to original length
  // After time stretch by ratio, we have 1/ratio of original samples
  // We need to resample to get back to original length
  // target_sr = original_sr / ratio would give us the right number of samples
  // But we want to keep the sample rate, so we resample then adjust

  int original_sr = audio.sample_rate();
  int target_sr = static_cast<int>(std::round(static_cast<float>(original_sr) / ratio));

  if (target_sr < 1000) target_sr = 1000;      // Minimum sample rate
  if (target_sr > 192000) target_sr = 192000;  // Maximum sample rate

  // Resample from stretched (at original_sr) to target_sr, then back to original_sr
  Audio resampled = resample(stretched, target_sr);
  Audio result = resample(resampled, original_sr);

  return result;
}

}  // namespace sonare
