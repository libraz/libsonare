#include "tts/tts.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "analysis/meter/basic.h"
#include "effects/normalize.h"
#include "util/exception.h"

namespace sonare::tts {
namespace {

float db_to_amplitude(float db) { return std::pow(10.0f, db / 20.0f); }

size_t leading_silent_samples(const Audio& audio, float threshold) {
  const float* data = audio.data();
  size_t index = 0;
  while (index < audio.size() && std::abs(data[index]) < threshold) {
    ++index;
  }
  return index;
}

size_t trailing_silent_samples(const Audio& audio, float threshold) {
  const float* data = audio.data();
  size_t count = 0;
  while (count < audio.size() && std::abs(data[audio.size() - 1 - count]) < threshold) {
    ++count;
  }
  return count;
}

}  // namespace

TtsQualityResult analyze_tts_quality(const Audio& audio, float silence_threshold_db,
                                     int frame_length, int hop_length) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(frame_length > 0 && hop_length > 0, ErrorCode::InvalidParameter);

  TtsQualityResult result;
  result.duration_sec = audio.duration();
  result.peak_db = analysis::meter::peak_db(audio);
  result.rms_db = analysis::meter::rms_db(audio);
  result.clipping_ratio = analysis::meter::clipping_ratio(audio);
  result.silence_ratio =
      analysis::meter::silence_ratio(audio, silence_threshold_db, frame_length, hop_length);

  const float silence_amp = db_to_amplitude(silence_threshold_db);
  result.leading_silence_sec =
      static_cast<float>(leading_silent_samples(audio, silence_amp)) / audio.sample_rate();
  result.trailing_silence_sec =
      static_cast<float>(trailing_silent_samples(audio, silence_amp)) / audio.sample_rate();
  return result;
}

Audio prepare_tts(const Audio& audio, float target_rms_db, float silence_threshold_db,
                  float peak_limit_db, float fade_sec) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(fade_sec >= 0.0f, ErrorCode::InvalidParameter);

  Audio result = trim(audio, silence_threshold_db);
  if (result.empty()) return result;

  result = normalize_rms(result, target_rms_db);
  const float current_peak = analysis::meter::peak_db(result);
  if (std::isfinite(current_peak) && current_peak > peak_limit_db) {
    result = apply_gain(result, peak_limit_db - current_peak);
  }
  if (fade_sec > 0.0f) {
    result = fade_out(fade_in(result, fade_sec), fade_sec);
  }
  return result;
}

Audio compress_pauses(const Audio& audio, float max_pause_sec, float silence_threshold_db) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(max_pause_sec >= 0.0f, ErrorCode::InvalidParameter);

  const float threshold = db_to_amplitude(silence_threshold_db);
  const size_t max_pause_samples =
      static_cast<size_t>(max_pause_sec * static_cast<float>(audio.sample_rate()));
  const float* data = audio.data();
  std::vector<float> out;
  out.reserve(audio.size());

  size_t i = 0;
  while (i < audio.size()) {
    if (std::abs(data[i]) >= threshold) {
      out.push_back(data[i]);
      ++i;
      continue;
    }

    const size_t silence_start = i;
    while (i < audio.size() && std::abs(data[i]) < threshold) {
      ++i;
    }
    const size_t silence_len = i - silence_start;
    const size_t keep = std::min(silence_len, max_pause_samples);
    out.insert(out.end(), data + silence_start, data + silence_start + keep);
  }

  return Audio::from_vector(std::move(out), audio.sample_rate());
}

}  // namespace sonare::tts
