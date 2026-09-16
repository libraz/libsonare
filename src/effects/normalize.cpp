#include "effects/normalize.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare {

using sonare::constants::kEpsilon;

namespace {

/// @brief Peak amplitude of @p audio in dB; returns -inf for silence or empty input.
/// @details Local helper to keep effects/ free of metering/ dependencies, built
///          from core primitives so the effects layer stays at the `core/`
///          dependency tier.
///
///          It agrees with sonare::metering::peak_db on every input that has
///          signal in it, and deliberately DISAGREES on silence and on an empty
///          buffer: this returns -infinity where the meter returns the shared
///          finite floor (kFloorDb, -120 dB). Each needs its own answer. The
///          meter's floor is JSON-safe (-inf serializes to null and the field
///          silently disappears), while normalize() tests this value with
///          std::isinf to short-circuit and return the input untouched -- given
///          -120 dB instead, it would compute `target_db - (-120)` and apply a
///          gain of that size to silence, amplifying denormals and dither by
///          over a hundred dB. Substituting one for the other is therefore not
///          a refactor.
float audio_peak_db(const Audio& audio) {
  if (audio.empty()) return -std::numeric_limits<float>::infinity();
  const float peak = peak_abs(audio.data(), audio.size());
  if (peak < constants::kEpsilon) return -std::numeric_limits<float>::infinity();
  return linear_to_db(peak);
}

/// @brief RMS level of @p audio in dB; returns -inf for silence or empty input.
float audio_rms_db(const Audio& audio) {
  if (audio.empty()) return -std::numeric_limits<float>::infinity();
  const float r = rms(audio.data(), audio.size());
  if (r < constants::kEpsilon) return -std::numeric_limits<float>::infinity();
  return linear_to_db(r);
}

}  // namespace

Audio apply_gain(const Audio& audio, float gain_db, bool clip) {
  SONARE_CHECK_MSG(std::isfinite(gain_db), ErrorCode::InvalidParameter,
                   "gain_db must be finite, got " + util::to_text(gain_db));
  if (audio.empty()) return audio;

  float gain_linear = db_to_linear(gain_db);

  std::vector<float> samples(audio.size());
  const float* data = audio.data();

  for (size_t i = 0; i < audio.size(); ++i) {
    samples[i] = data[i] * gain_linear;
    if (clip) {
      // Hard-clip to [-1, 1] (opt-out via the clip flag). std::clamp rather than
      // a nested min/max: a comparison against NaN is false, so std::min(1.0f,
      // NaN) returns 1.0f and launders the NaN into a peak this function meant
      // to produce. clamp returns NaN unchanged and the caller is the
      // downstream, so the NaN reports itself (SampleDestination::kCallerReturn).
      // An infinity is not substituted here either -- it takes the clip's own
      // bound, which is this transfer function rather than a replacement value.
      samples[i] = std::clamp(samples[i], -1.0f, 1.0f);
    }
  }

  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

Audio normalize(const Audio& audio, float target_db, bool clip) {
  SONARE_CHECK_MSG(std::isfinite(target_db), ErrorCode::InvalidParameter,
                   "target_db must be finite, got " + util::to_text(target_db));
  SONARE_CHECK_MSG(!clip || target_db <= 0.0f, ErrorCode::InvalidParameter,
                   "target_db must be <= 0 when clip is set, got " + util::to_text(target_db));
  if (audio.empty()) return audio;

  float current_peak = audio_peak_db(audio);

  if (std::isinf(current_peak)) {
    return audio;  ///< Silent audio, nothing to normalize
  }

  float gain = target_db - current_peak;
  return apply_gain(audio, gain, clip);
}

Audio normalize_rms(const Audio& audio, float target_db, bool clip) {
  SONARE_CHECK_MSG(std::isfinite(target_db), ErrorCode::InvalidParameter,
                   "target_db must be finite, got " + util::to_text(target_db));
  SONARE_CHECK_MSG(!clip || target_db <= 0.0f, ErrorCode::InvalidParameter,
                   "target_db must be <= 0 when clip is set, got " + util::to_text(target_db));
  if (audio.empty()) return audio;

  float current_rms = audio_rms_db(audio);

  if (std::isinf(current_rms)) {
    return audio;  ///< Silent audio
  }

  float gain = target_db - current_rms;
  return apply_gain(audio, gain, clip);
}

std::pair<size_t, size_t> detect_silence_boundaries(const Audio& audio, float threshold_db,
                                                    int frame_length, int hop_length) {
  // Reject non-positive frame/hop before any loop or division: a zero/negative
  // hop_length spins the scan forever and a zero frame_length divides by zero.
  SONARE_CHECK_MSG(frame_length > 0, ErrorCode::InvalidParameter, "frame_length must be > 0");
  SONARE_CHECK_MSG(hop_length > 0, ErrorCode::InvalidParameter, "hop_length must be > 0");
  if (audio.empty()) return {0, 0};

  float threshold_linear = db_to_linear(threshold_db);
  const float* data = audio.data();
  size_t n_samples = audio.size();

  /// Find start (first frame above threshold)
  size_t start = 0;
  for (size_t pos = 0; pos + frame_length <= n_samples; pos += hop_length) {
    float rms = 0.0f;
    for (int i = 0; i < frame_length; ++i) {
      rms += data[pos + i] * data[pos + i];
    }
    rms = std::sqrt(rms / static_cast<float>(frame_length));

    if (rms > threshold_linear) {
      start = pos;
      break;
    }
  }

  /// Find end (last frame above threshold), scanning backward.
  /// Use a while-loop to guard against size_t underflow: the break-before-subtract
  /// pattern ensures `pos -= hop_length` never wraps an unsigned value.
  size_t end = n_samples;
  if (n_samples >= static_cast<size_t>(frame_length)) {
    size_t pos = n_samples - frame_length;
    while (true) {
      float rms = 0.0f;
      for (int i = 0; i < frame_length; ++i) {
        rms += data[pos + i] * data[pos + i];
      }
      rms = std::sqrt(rms / static_cast<float>(frame_length));

      if (rms > threshold_linear) {
        end = pos + frame_length;
        break;
      }

      if (pos < static_cast<size_t>(hop_length)) break;
      pos -= hop_length;
    }
  }

  if (start >= end) {
    return {0, n_samples};  ///< All silent or invalid, return original bounds
  }

  return {start, end};
}

Audio trim_absolute(const Audio& audio, float threshold_db, int frame_length, int hop_length) {
  if (audio.empty()) return audio;

  auto [start, end] = detect_silence_boundaries(audio, threshold_db, frame_length, hop_length);

  if (start == 0 && end == audio.size()) {
    return audio;  ///< No trimming needed
  }

  return audio.slice_samples(start, end);
}

namespace {

/// @brief Applies a cosine fade to audio.
/// @param audio Input audio
/// @param duration_sec Fade duration in seconds
/// @param is_fade_in If true, fade in; if false, fade out
Audio apply_fade(const Audio& audio, float duration_sec, bool is_fade_in) {
  const double requested_samples = static_cast<double>(duration_sec) * audio.sample_rate();
  SONARE_CHECK_MSG(std::isfinite(duration_sec) && duration_sec >= 0.0f, ErrorCode::InvalidParameter,
                   "duration_sec must be finite and >= 0, got " + util::to_text(duration_sec));
  SONARE_CHECK_MSG(
      requested_samples <= static_cast<double>(std::numeric_limits<size_t>::max()),
      ErrorCode::InvalidParameter,
      "duration_sec is too long for this sample rate, got " + util::to_text(duration_sec));
  if (audio.empty()) return audio;

  size_t fade_samples = static_cast<size_t>(requested_samples);
  fade_samples = std::min(fade_samples, audio.size());

  std::vector<float> samples(audio.size());
  const float* data = audio.data();

  size_t fade_start = is_fade_in ? 0 : audio.size() - fade_samples;

  for (size_t i = 0; i < audio.size(); ++i) {
    float gain = 1.0f;
    if (is_fade_in && i < fade_samples) {
      gain = cosine_fade_in_gain(i, fade_samples);
    } else if (!is_fade_in && i >= fade_start) {
      gain = cosine_fade_out_gain(i - fade_start, fade_samples);
    }
    samples[i] = data[i] * gain;
  }

  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace

Audio fade_in(const Audio& audio, float duration_sec) {
  return apply_fade(audio, duration_sec, true);
}

Audio fade_out(const Audio& audio, float duration_sec) {
  return apply_fade(audio, duration_sec, false);
}

}  // namespace sonare
