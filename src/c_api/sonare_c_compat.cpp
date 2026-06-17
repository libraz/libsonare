#include <sonare/sonare_c.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include "core/audio.h"
#include "core/convert.h"
#include "core/db_convert.h"
#include "core/pcen.h"
#include "effects/preemphasis.h"
#include "effects/silence.h"
#include "feature/rhythm.h"
#include "feature/tonnetz.h"
#include "metering/lufs.h"
#include "sonare_c_internal.h"
#include "util/frame.h"
#include "util/padding.h"
#include "util/peak.h"
#include "util/vector_normalize.h"

using namespace sonare;
using namespace sonare_c_detail;

namespace {

// Vector -> caller-owned C array copies funnel through copy_vector<T> in
// sonare_c_internal.h (single canonical owner).

// Shared input-buffer guard for the compat / transform functions. EMPTY
// (length == 0) is allowed (these are pure array transforms with a well-defined
// empty result); a NULL buffer with length > 0 is rejected; and any non-finite
// (NaN / Inf) sample is rejected so a NaN can never silently propagate through a
// transform. The non-finite policy is uniform with validate_audio_params; see
// the INPUT-BUFFER POLICY block in sonare_c_features.h.
SonareError validate_buffer(const float* values, size_t length) {
  if (length > 0 && values == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  for (size_t i = 0; i < length; ++i) {
    if (!std::isfinite(values[i])) return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
}

NormType c_norm_type(int norm_type) {
  switch (norm_type) {
    case 1:
      return NormType::L1;
    case 2:
      return NormType::L2;
    case 3:
      return NormType::Power;
    default:
      return NormType::Inf;
  }
}

}  // namespace

int sonare_frames_to_samples(int frames, int hop_length, int n_fft) {
  return frames_to_samples(frames, hop_length, n_fft);
}

int sonare_samples_to_frames(int samples, int hop_length, int n_fft) {
  return samples_to_frames(samples, hop_length, n_fft);
}

SonareError sonare_power_to_db(const float* values, size_t length, float ref, float amin,
                               float top_db, float** out, size_t* out_length) {
  if (validate_buffer(values, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  return copy_vector(power_to_db(values, length, ref, amin, top_db), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_amplitude_to_db(const float* values, size_t length, float ref, float amin,
                                   float top_db, float** out, size_t* out_length) {
  if (validate_buffer(values, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  return copy_vector(amplitude_to_db(values, length, ref, amin, top_db), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_db_to_power(const float* values, size_t length, float ref, float** out,
                               size_t* out_length) {
  if (validate_buffer(values, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  return copy_vector(db_to_power(values, length, ref), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_db_to_amplitude(const float* values, size_t length, float ref, float** out,
                                   size_t* out_length) {
  if (validate_buffer(values, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  return copy_vector(db_to_amplitude(values, length, ref), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_preemphasis(const float* samples, size_t length, float coef, float zi,
                               int use_zi, float** out, size_t* out_length) {
  if (validate_buffer(samples, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  const std::optional<float> state = use_zi ? std::optional<float>(zi) : std::nullopt;
  return copy_vector(preemphasis(samples, length, coef, state), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_deemphasis(const float* samples, size_t length, float coef, float zi, int use_zi,
                              float** out, size_t* out_length) {
  if (validate_buffer(samples, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  const std::optional<float> state = use_zi ? std::optional<float>(zi) : std::nullopt;
  return copy_vector(deemphasis(samples, length, coef, state), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_trim_silence(const float* samples, size_t length, float top_db, int frame_length,
                                int hop_length, float** out, size_t* out_length, int* start_sample,
                                int* end_sample) {
  if (!start_sample || !end_sample) return SONARE_ERROR_INVALID_PARAMETER;
  if (validate_buffer(samples, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  TrimResult result = trim(samples, length, top_db, frame_length, hop_length);
  *start_sample = result.start_sample;
  *end_sample = result.end_sample;
  return copy_vector(result.audio, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_split_silence(const float* samples, size_t length, float top_db,
                                 int frame_length, int hop_length, int** out_intervals,
                                 size_t* out_interval_count) {
  if (validate_buffer(samples, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto ranges = split(samples, length, top_db, frame_length, hop_length);
  std::vector<int> flat;
  flat.reserve(ranges.size() * 2);
  for (const auto& range : ranges) {
    flat.push_back(range.first);
    flat.push_back(range.second);
  }
  return copy_vector(flat, out_intervals, out_interval_count);
  SONARE_C_CATCH
}

SonareError sonare_frame_signal(const float* samples, size_t length, int frame_length,
                                int hop_length, float** out, size_t* out_length,
                                int* out_n_frames) {
  if (!out_n_frames) return SONARE_ERROR_INVALID_PARAMETER;
  if (validate_buffer(samples, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_n_frames = frame_count(length, frame_length, hop_length);
  return copy_vector(frame(samples, length, frame_length, hop_length), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_pad_center(const float* values, size_t length, size_t target_size,
                              float pad_value, float** out, size_t* out_length) {
  if (validate_buffer(values, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  return copy_vector(pad_center(values, length, target_size, pad_value), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_fix_length(const float* values, size_t length, size_t target_size,
                              float pad_value, float** out, size_t* out_length) {
  if (validate_buffer(values, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  return copy_vector(fix_length(values, length, target_size, pad_value), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_fix_frames(const int* frames, size_t length, int x_min, int x_max, int pad,
                              int** out, size_t* out_length) {
  if (length > 0 && frames == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  std::vector<int> input(frames, frames + length);
  return copy_vector(fix_frames(input, x_min, x_max, pad != 0), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_peak_pick(const float* values, size_t length, int pre_max, int post_max,
                             int pre_avg, int post_avg, float delta, int wait, int** out,
                             size_t* out_length) {
  if (validate_buffer(values, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  return copy_vector(peak_pick(values, length, pre_max, post_max, pre_avg, post_avg, delta, wait),
                     out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_vector_normalize(const float* values, size_t length, int norm_type,
                                    float threshold, float** out, size_t* out_length) {
  if (validate_buffer(values, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  return copy_vector(normalize(values, length, c_norm_type(norm_type), threshold), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_pcen(const float* values, int n_bins, int n_frames, int sample_rate,
                        int hop_length, float time_constant, float gain, float bias, float power,
                        float eps, float** out, size_t* out_length) {
  if (n_bins < 0 || n_frames < 0) return SONARE_ERROR_INVALID_PARAMETER;
  // Guard against size_t overflow (32-bit on WASM) and bound the product against
  // kMaxBufferSize before using it as the claimed buffer length.
  if (n_frames != 0 &&
      static_cast<size_t>(n_bins) > kMaxBufferSize / static_cast<size_t>(n_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (validate_buffer(values, static_cast<size_t>(n_bins) * static_cast<size_t>(n_frames)) !=
      SONARE_OK) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  PcenConfig config;
  config.sr = sample_rate;
  config.hop_length = hop_length;
  config.time_constant = time_constant;
  config.gain = gain;
  config.bias = bias;
  config.power = power;
  config.eps = eps;
  return copy_vector(pcen(values, n_bins, n_frames, config), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_tonnetz(const float* chromagram, int n_chroma, int n_frames, float** out,
                           size_t* out_length) {
  if (n_chroma < 0 || n_frames < 0) return SONARE_ERROR_INVALID_PARAMETER;
  // Guard against size_t overflow (32-bit on WASM) and bound the product against
  // kMaxBufferSize before using it as the claimed buffer length.
  if (n_frames != 0 &&
      static_cast<size_t>(n_chroma) > kMaxBufferSize / static_cast<size_t>(n_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (validate_buffer(chromagram, static_cast<size_t>(n_chroma) * static_cast<size_t>(n_frames)) !=
      SONARE_OK) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  return copy_vector(tonnetz(chromagram, n_chroma, n_frames), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_tempogram_with_mode(const float* onset_envelope, size_t length, int sample_rate,
                                       int hop_length, int win_length, int center, int norm,
                                       int mode, float** out, size_t* out_length,
                                       int* out_n_frames) {
  if (!out_n_frames) return SONARE_ERROR_INVALID_PARAMETER;
  if (validate_buffer(onset_envelope, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  if (mode != SONARE_TEMPOGRAM_AUTOCORRELATION && mode != SONARE_TEMPOGRAM_COSINE) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  TempogramConfig config;
  config.hop_length = hop_length;
  config.win_length = win_length;
  config.center = center != 0;
  config.norm = norm != 0;
  config.mode =
      mode == SONARE_TEMPOGRAM_COSINE ? TempogramMode::kCosine : TempogramMode::kAutocorrelation;
  std::vector<float> input(onset_envelope, onset_envelope + length);
  *out_n_frames = static_cast<int>(input.size());
  return copy_vector(tempogram(input, sample_rate, config), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_tempogram(const float* onset_envelope, size_t length, int sample_rate,
                             int hop_length, int win_length, int center, int norm, float** out,
                             size_t* out_length, int* out_n_frames) {
  return sonare_tempogram_with_mode(onset_envelope, length, sample_rate, hop_length, win_length,
                                    center, norm, SONARE_TEMPOGRAM_AUTOCORRELATION, out, out_length,
                                    out_n_frames);
}

SonareError sonare_cyclic_tempogram(const float* onset_envelope, size_t length, int sample_rate,
                                    int hop_length, int win_length, float bpm_min, int n_bins,
                                    float** out, size_t* out_length, int* out_n_frames) {
  if (!out_n_frames) return SONARE_ERROR_INVALID_PARAMETER;
  if (validate_buffer(onset_envelope, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  TempogramConfig config;
  config.hop_length = hop_length;
  config.win_length = win_length;
  config.center = true;
  config.norm = false;
  std::vector<float> input(onset_envelope, onset_envelope + length);
  *out_n_frames = static_cast<int>(input.size());
  return copy_vector(cyclic_tempogram(input, sample_rate, config, bpm_min, n_bins), out,
                     out_length);
  SONARE_C_CATCH
}

SonareError sonare_plp(const float* onset_envelope, size_t length, int sample_rate, int hop_length,
                       float tempo_min, float tempo_max, int win_length, float** out,
                       size_t* out_length) {
  if (validate_buffer(onset_envelope, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  PlpConfig config;
  config.sr = sample_rate;
  config.hop_length = hop_length;
  config.tempo_min = tempo_min;
  config.tempo_max = tempo_max;
  config.win_length = win_length;
  std::vector<float> input(onset_envelope, onset_envelope + length);
  return copy_vector(plp(input, config), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_fourier_tempogram(const float* onset_envelope, size_t length, int sr,
                                     int hop_length, int win_length, int center, int norm,
                                     float** out, size_t* out_length, int* out_n_frames) {
  if (!out_n_frames) return SONARE_ERROR_INVALID_PARAMETER;
  if (validate_buffer(onset_envelope, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  TempogramConfig config;
  config.hop_length = hop_length;
  config.win_length = win_length;
  config.center = center != 0;
  config.norm = norm != 0;
  std::vector<float> input(onset_envelope, onset_envelope + length);
  *out_n_frames = static_cast<int>(input.size());
  return copy_vector(fourier_tempogram(input, sr, config), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_tempogram_ratio(const float* tempogram_data, size_t length, int win_length,
                                   int sr, int hop_length, const float* factors, size_t n_factors,
                                   float** out, size_t* out_length) {
  if (validate_buffer(tempogram_data, length) != SONARE_OK) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_factors > 0 && factors == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  std::vector<float> input(tempogram_data, tempogram_data + length);
  std::vector<float> result;
  if (factors != nullptr && n_factors > 0) {
    std::vector<float> factor_vec(factors, factors + n_factors);
    result = tempogram_ratio(input, win_length, sr, hop_length, factor_vec);
  } else {
    result = tempogram_ratio(input, win_length, sr, hop_length);
  }
  return copy_vector(result, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_lufs(const float* samples, size_t length, int sr, SonareLufsResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  return run_offline(samples, length, sr, [&](const Audio& audio) -> SonareError {
    metering::LufsResult result = metering::lufs(audio);
    out->integrated_lufs = result.integrated_lufs;
    out->momentary_lufs = result.momentary_lufs;
    out->short_term_lufs = result.short_term_lufs;
    out->loudness_range = result.loudness_range;
    return SONARE_OK;
  });
}

SonareError sonare_momentary_lufs(const float* samples, size_t length, int sr, float** out,
                                  size_t* out_length) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  return run_offline(samples, length, sr, [&](const Audio& audio) -> SonareError {
    return copy_vector(metering::momentary_lufs(audio), out, out_length);
  });
}

SonareError sonare_short_term_lufs(const float* samples, size_t length, int sr, float** out,
                                   size_t* out_length) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  return run_offline(samples, length, sr, [&](const Audio& audio) -> SonareError {
    return copy_vector(metering::short_term_lufs(audio), out, out_length);
  });
}

SonareError sonare_lufs_interleaved(const float* samples, size_t frames, int channels,
                                    int sample_rate, SonareLufsResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  // Mirror the mono sonare_lufs input contract (validate_audio_params): reject
  // empty audio, out-of-range sample rate, oversized buffers, and non-finite
  // samples so both LUFS entry points share one validation policy.
  if (channels <= 0 || frames == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (sample_rate < kMinSampleRate || sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (samples == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (static_cast<size_t>(channels) > kMaxBufferSize / frames) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const size_t total = frames * static_cast<size_t>(channels);
  if (total > kMaxBufferSize) return SONARE_ERROR_INVALID_PARAMETER;
  for (size_t i = 0; i < total; ++i) {
    if (!std::isfinite(samples[i])) return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  metering::LufsResult result = metering::lufs_interleaved(samples, frames, channels, sample_rate);
  out->integrated_lufs = result.integrated_lufs;
  out->momentary_lufs = result.momentary_lufs;
  out->short_term_lufs = result.short_term_lufs;
  out->loudness_range = result.loudness_range;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_ebur128_loudness_range(const float* samples, size_t length, int sample_rate,
                                          float* out_lra) {
  if (!out_lra) return SONARE_ERROR_INVALID_PARAMETER;
  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    *out_lra = metering::ebur128_loudness_range(audio);
    return SONARE_OK;
  });
}
