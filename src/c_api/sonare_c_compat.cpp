#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
#include "util/parameter_checks.h"
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
//
// Four entry points below are stricter than this and reject an empty input:
// sonare_trim_silence, sonare_split_silence, sonare_fix_frames and
// sonare_tempogram_ratio, whose empty results already encode a real measurement.
// Those rejections live in the core functions they wrap (sonare::trim /
// sonare::split / sonare::fix_frames / sonare::tempogram_ratio) so that the WASM
// bindings, which call the core directly and do not go through this layer, share
// one policy instead of a hand-mirrored copy. They arrive here as a
// SonareException and map to SONARE_ERROR_INVALID_PARAMETER via SONARE_C_CATCH.
void refuse_invalid_buffer(const char* name, const char* length_name, const float* values,
                           size_t length, bool scan_finite) {
  util::check::non_null_when_count(values, length, name, length_name);
  if (!scan_finite) return;
  for (size_t i = 0; i < length; ++i) {
    if (!std::isfinite(values[i])) {
      util::check::refuse(name, "contains a non-finite sample at index " + std::to_string(i));
    }
  }
}

// Refuses a NULL buffer with a non-zero length, and (for an entry whose core function does not
// refuse a non-finite value itself) a non-finite sample.
SonareError validate_buffer(const char* name, const float* values, size_t length,
                            bool scan_finite = true) {
  return run_validation(
      [&] { refuse_invalid_buffer(name, "length", values, length, scan_finite); });
}

// The array -> array transform for a core function that refuses its own non-finite input.
template <typename T, typename Compute>
SonareError transform_core_buffer(const char* name, const float* values, size_t length, T** out,
                                  size_t* out_length, Compute&& compute) {
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err = validate_buffer(name, values, length, false); err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_TRY
  return copy_vector(compute(), out, out_length);
  SONARE_C_CATCH
}

// Shared body of the array -> array transforms: zero the outputs, validate the
// input buffer, then marshal the vector produced by @p compute.
template <typename T, typename Compute>
SonareError transform_buffer(const char* name, const float* values, size_t length, T** out,
                             size_t* out_length, Compute&& compute) {
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err = validate_buffer(name, values, length); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_TRY
  return copy_vector(compute(), out, out_length);
  SONARE_C_CATCH
}

// Refuses a row-major matrix whose element count would pass the buffer size limit.
void check_matrix_size(int rows, int columns, const char* rows_name, const char* columns_name) {
  util::check::integer_at_least(rows, 0, rows_name);
  util::check::integer_at_least(columns, 0, columns_name);
  if (columns != 0 && static_cast<size_t>(rows) > kMaxBufferSize / static_cast<size_t>(columns)) {
    util::check::refuse_relation({rows_name, columns_name},
                                 "{0} times {1} must be at most " + std::to_string(kMaxBufferSize));
  }
}

// Shared by both interleaved LUFS entry points.
// The interleaved buffer goes through the same audio input policy as the mono sonare_lufs.
SonareError validate_interleaved_lufs_input(const float* samples, size_t frames, int num_channels,
                                            int sample_rate) {
  if (const SonareError err = run_validation([&] {
        util::check::integer_at_least(num_channels, 1, "num_channels");
        if (frames != 0 && static_cast<size_t>(num_channels) > kMaxBufferSize / frames) {
          util::check::refuse_relation(
              {"frames", "num_channels"},
              "{0} times {1} must be at most " + std::to_string(kMaxBufferSize));
        }
      });
      err != SONARE_OK) {
    return err;
  }
  return validate_audio_params("lufs", "samples", samples,
                               frames * static_cast<size_t>(num_channels), sample_rate);
}

// SonareNormType carries the NormType ordinal itself.
static_assert(static_cast<int>(NormType::Inf) == 0 && static_cast<int>(NormType::L1) == 1 &&
                  static_cast<int>(NormType::L2) == 2 && static_cast<int>(NormType::Power) == 3,
              "sonare_vector_normalize's norm_type ordinals must match NormType");

}  // namespace

int sonare_frames_to_samples(int frames, int hop_length, int n_fft) {
  return frames_to_samples(frames, hop_length, n_fft);
}

int sonare_samples_to_frames(int samples, int hop_length, int n_fft) {
  return samples_to_frames(samples, hop_length, n_fft);
}

SonareError sonare_power_to_db(const float* values, size_t length, float ref, float amin,
                               float top_db, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return transform_buffer("values", values, length, out, out_length,
                          [&] { return power_to_db(values, length, ref, amin, top_db); });
}

SonareError sonare_amplitude_to_db(const float* values, size_t length, float ref, float amin,
                                   float top_db, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return transform_buffer("values", values, length, out, out_length,
                          [&] { return amplitude_to_db(values, length, ref, amin, top_db); });
}

SonareError sonare_db_to_power(const float* values, size_t length, float ref, float** out,
                               size_t* out_length) {
  SONARE_C_API_ENTRY;
  return transform_buffer("values", values, length, out, out_length,
                          [&] { return db_to_power(values, length, ref); });
}

SonareError sonare_db_to_amplitude(const float* values, size_t length, float ref, float** out,
                                   size_t* out_length) {
  SONARE_C_API_ENTRY;
  return transform_buffer("values", values, length, out, out_length,
                          [&] { return db_to_amplitude(values, length, ref); });
}

SonareError sonare_preemphasis(const float* samples, size_t length, float coef, float zi,
                               int use_zi, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return transform_core_buffer("samples", samples, length, out, out_length, [&] {
    const std::optional<float> state = use_zi ? std::optional<float>(zi) : std::nullopt;
    return preemphasis(samples, length, coef, state);
  });
}

SonareError sonare_deemphasis(const float* samples, size_t length, float coef, float zi, int use_zi,
                              float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return transform_core_buffer("samples", samples, length, out, out_length, [&] {
    const std::optional<float> state = use_zi ? std::optional<float>(zi) : std::nullopt;
    return deemphasis(samples, length, coef, state);
  });
}

SonareError sonare_trim_silence(const float* samples, size_t length, float top_db, int frame_length,
                                int hop_length, float** out, size_t* out_length, int* start_sample,
                                int* end_sample) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, start_sample, end_sample);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_REQUIRE_NONNULL(start_sample);
  SONARE_C_REQUIRE_NONNULL(end_sample);
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
  SONARE_C_API_ENTRY;
  return transform_core_buffer("samples", samples, length, out_intervals, out_interval_count, [&] {
    auto ranges = split(samples, length, top_db, frame_length, hop_length);
    std::vector<int> flat;
    flat.reserve(ranges.size() * 2);
    for (const auto& range : ranges) {
      flat.push_back(range.first);
      flat.push_back(range.second);
    }
    return flat;
  });
}

SonareError sonare_split_silence_common(const float* const* signals, size_t signal_count,
                                        const size_t* lengths, float top_db, int frame_length,
                                        int hop_length, int** out_intervals,
                                        size_t* out_interval_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_intervals, out_interval_count);
  return sonare_split_silence_common_ex(signals, signal_count, lengths, top_db, frame_length,
                                        hop_length, out_intervals, out_interval_count, nullptr);
}

SonareError sonare_split_silence_common_ex(const float* const* signals, size_t signal_count,
                                           const size_t* lengths, float top_db, int frame_length,
                                           int hop_length, int** out_intervals,
                                           size_t* out_interval_count,
                                           SonareSilenceCommonReport* out_report) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_intervals, out_interval_count, out_report);
  SONARE_C_REQUIRE_NONNULL(signals);
  if (const SonareError err = run_validation([&] {
        util::check::integer_at_least(signal_count, 1, "signal_count");
        util::check::non_null(lengths, "lengths");
        for (size_t index = 0; index < signal_count; ++index) {
          const std::string name = "signals[" + std::to_string(index) + "]";
          const std::string length_name = "lengths[" + std::to_string(index) + "]";
          refuse_invalid_buffer(name.c_str(), length_name.c_str(), signals[index], lengths[index],
                                true);
        }
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_intervals);
  SONARE_C_REQUIRE_NONNULL(out_interval_count);
  SONARE_C_TRY
  // The union rule and its report live in the core because the WebAssembly
  // binding needs them too and calls the core directly, never this layer.
  const CommonSplitReport report =
      split_common_with_report(signals, lengths, signal_count, top_db, frame_length, hop_length);
  if (out_report != nullptr) {
    out_report->silence_ceiling_db = report.silence_ceiling_db;
    out_report->max_signal_intervals = static_cast<int32_t>(report.max_signal_intervals);
    out_report->min_signal_intervals = static_cast<int32_t>(report.min_signal_intervals);
  }
  std::vector<int> flat;
  flat.reserve(report.intervals.size() * 2);
  for (const auto& range : report.intervals) {
    flat.push_back(range.first);
    flat.push_back(range.second);
  }
  return copy_vector(flat, out_intervals, out_interval_count);
  SONARE_C_CATCH
}

SonareError sonare_frame_signal(const float* samples, size_t length, int frame_length,
                                int hop_length, float** out, size_t* out_length,
                                int* out_n_frames) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_n_frames);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_REQUIRE_NONNULL(out_n_frames);
  SONARE_C_TRY
  *out_n_frames = frame_count(length, frame_length, hop_length);
  return copy_vector(frame(samples, length, frame_length, hop_length), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_pad_center(const float* values, size_t length, size_t target_size,
                              float pad_value, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return transform_buffer("values", values, length, out, out_length,
                          [&] { return pad_center(values, length, target_size, pad_value); });
}

SonareError sonare_fix_length(const float* values, size_t length, size_t target_size,
                              float pad_value, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return transform_buffer("values", values, length, out, out_length,
                          [&] { return fix_length(values, length, target_size, pad_value); });
}

SonareError sonare_fix_frames(const int* frames, size_t length, int x_min, int x_max, int pad,
                              int** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(frames, length);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_TRY
  std::vector<int> input(frames, frames + length);
  return copy_vector(fix_frames(input, x_min, x_max, pad != 0), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_peak_pick(const float* values, size_t length, int pre_max, int post_max,
                             int pre_avg, int post_avg, float delta, int wait, int** out,
                             size_t* out_length) {
  SONARE_C_API_ENTRY;
  return transform_buffer("values", values, length, out, out_length, [&] {
    return peak_pick(values, length, pre_max, post_max, pre_avg, post_avg, delta, wait);
  });
}

SonareError sonare_vector_normalize(const float* values, size_t length, SonareNormType norm,
                                    float threshold, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err = run_validation([&] {
        refuse_invalid_buffer("values", "length", values, length, true);
        util::check::one_of(
            static_cast<int>(norm),
            std::array<int, 4>{static_cast<int>(NormType::Inf), static_cast<int>(NormType::L1),
                               static_cast<int>(NormType::L2), static_cast<int>(NormType::Power)},
            "norm");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  const NormType norm_kind = admitted_ordinal_to_enum<NormType>(static_cast<int>(norm));
  SONARE_C_TRY
  return copy_vector(normalize(values, length, norm_kind, threshold), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_pcen(const float* values, int n_bins, int n_frames, int sample_rate,
                        int hop_length, float time_constant, float gain, float bias, float power,
                        float eps, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err = run_validation([&] {
        check_matrix_size(n_bins, n_frames, "n_bins", "n_frames");
        if (n_bins != 0 && n_frames != 0 && values == nullptr) {
          util::check::refuse("values", "must not be NULL when n_bins and n_frames are not 0");
        }
        refuse_invalid_buffer("values", "length", values,
                              static_cast<size_t>(n_bins) * static_cast<size_t>(n_frames), true);
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
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
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err = run_validation([&] {
        check_matrix_size(n_chroma, n_frames, "n_chroma", "n_frames");
        if (n_chroma != 0 && n_frames != 0 && chromagram == nullptr) {
          util::check::refuse("chromagram",
                              "must not be NULL when n_chroma and n_frames are not 0");
        }
        refuse_invalid_buffer("chromagram", "length", chromagram,
                              static_cast<size_t>(n_chroma) * static_cast<size_t>(n_frames), true);
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_TRY
  return copy_vector(tonnetz(chromagram, n_chroma, n_frames), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_tempogram_with_mode(const float* onset_envelope, size_t length, int sample_rate,
                                       int hop_length, int win_length, int center, int norm,
                                       int mode, float** out, size_t* out_length,
                                       int* out_n_frames) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_n_frames);
  if (const SonareError err = run_validation([&] {
        refuse_invalid_buffer("onset_envelope", "length", onset_envelope, length, true);
        util::check::one_of(
            mode, std::array<int, 2>{SONARE_TEMPOGRAM_AUTOCORRELATION, SONARE_TEMPOGRAM_COSINE},
            "mode");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_REQUIRE_NONNULL(out_n_frames);
  SONARE_C_TRY
  TempogramConfig config;
  config.hop_length = hop_length;
  config.win_length = win_length;
  config.center = center != 0;
  config.norm = norm != 0;
  config.mode =
      mode == SONARE_TEMPOGRAM_COSINE ? TempogramMode::kCosine : TempogramMode::kAutocorrelation;
  std::vector<float> input(onset_envelope, onset_envelope + length);
  const std::vector<float> result = tempogram(input, sample_rate, config);
  // The core frames the envelope (one frame per window when not centered), so read the count back.
  *out_n_frames = static_cast<int>(result.size() / static_cast<std::size_t>(win_length));
  return copy_vector(result, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_tempogram(const float* onset_envelope, size_t length, int sample_rate,
                             int hop_length, int win_length, int center, int norm, float** out,
                             size_t* out_length, int* out_n_frames) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_n_frames);
  return sonare_tempogram_with_mode(onset_envelope, length, sample_rate, hop_length, win_length,
                                    center, norm, SONARE_TEMPOGRAM_AUTOCORRELATION, out, out_length,
                                    out_n_frames);
}

SonareError sonare_cyclic_tempogram(const float* onset_envelope, size_t length, int sample_rate,
                                    int hop_length, int win_length, float bpm_min, int n_bins,
                                    float** out, size_t* out_length, int* out_n_frames) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_n_frames);
  if (const SonareError err = validate_buffer("onset_envelope", onset_envelope, length);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_REQUIRE_NONNULL(out_n_frames);
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
                       float bpm_min, float bpm_max, int win_length, float** out,
                       size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err = validate_buffer("onset_envelope", onset_envelope, length);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_TRY
  PlpConfig config;
  config.sr = sample_rate;
  config.hop_length = hop_length;
  config.tempo_min = bpm_min;
  config.tempo_max = bpm_max;
  config.win_length = win_length;
  std::vector<float> input(onset_envelope, onset_envelope + length);
  return copy_vector(plp(input, config), out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_fourier_tempogram(const float* onset_envelope, size_t length, int sample_rate,
                                     int hop_length, int win_length, int center, int norm,
                                     float** out, size_t* out_length, int* out_n_frames) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_n_frames);
  if (const SonareError err = validate_buffer("onset_envelope", onset_envelope, length, false);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_REQUIRE_NONNULL(out_n_frames);
  SONARE_C_TRY
  TempogramConfig config;
  config.hop_length = hop_length;
  config.win_length = win_length;
  config.center = center != 0;
  config.norm = norm != 0;
  std::vector<float> input(onset_envelope, onset_envelope + length);
  const std::vector<float> result = fourier_tempogram(input, sample_rate, config);
  *out_n_frames = static_cast<int>(result.size() / static_cast<std::size_t>(win_length / 2 + 1));
  return copy_vector(result, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_tempogram_ratio(const float* tempogram, size_t length, int win_length,
                                   int sample_rate, int hop_length, const float* factors,
                                   size_t n_factors, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err = validate_buffer("tempogram", tempogram, length, false);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(factors, n_factors);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_TRY
  std::vector<float> input(tempogram, tempogram + length);
  std::vector<float> result;
  if (factors != nullptr && n_factors > 0) {
    std::vector<float> factor_vec(factors, factors + n_factors);
    result = tempogram_ratio(input, win_length, sample_rate, hop_length, factor_vec);
  } else {
    result = tempogram_ratio(input, win_length, sample_rate, hop_length);
  }
  return copy_vector(result, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_lufs(const float* samples, size_t length, int sample_rate,
                        SonareLufsResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(out);
  return run_offline("lufs", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       metering::LufsResult result = metering::lufs(audio);
                       out->integrated_lufs = result.integrated_lufs;
                       out->momentary_lufs = result.momentary_lufs;
                       out->short_term_lufs = result.short_term_lufs;
                       out->max_momentary_lufs = result.max_momentary_lufs;
                       out->max_short_term_lufs = result.max_short_term_lufs;
                       out->loudness_range_lu = result.loudness_range_lu;
                       return SONARE_OK;
                     });
}

SonareError sonare_momentary_lufs(const float* samples, size_t length, int sample_rate, float** out,
                                  size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  return run_offline("momentary_lufs", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       return copy_vector(metering::momentary_lufs(audio), out, out_length);
                     });
}

SonareError sonare_short_term_lufs(const float* samples, size_t length, int sample_rate,
                                   float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  return run_offline("short_term_lufs", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       return copy_vector(metering::short_term_lufs(audio), out, out_length);
                     });
}

SonareError sonare_lufs_interleaved(const float* samples, size_t frames, int num_channels,
                                    int sample_rate, SonareLufsResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  if (const SonareError err =
          validate_interleaved_lufs_input(samples, frames, num_channels, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  metering::LufsResult result =
      metering::lufs_interleaved(samples, frames, num_channels, sample_rate);
  out->integrated_lufs = result.integrated_lufs;
  out->momentary_lufs = result.momentary_lufs;
  out->short_term_lufs = result.short_term_lufs;
  out->max_momentary_lufs = result.max_momentary_lufs;
  out->max_short_term_lufs = result.max_short_term_lufs;
  out->loudness_range_lu = result.loudness_range_lu;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_lufs_series_interleaved(const float* samples, size_t frames, int num_channels,
                                           int sample_rate, float** out_momentary,
                                           size_t* out_momentary_length, float** out_short_term,
                                           size_t* out_short_term_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_momentary, out_momentary_length, out_short_term, out_short_term_length);
  if (const SonareError err =
          validate_interleaved_lufs_input(samples, frames, num_channels, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  // A pointer without its length cannot be consumed, so a half-given pair is a
  // caller error rather than a request to skip the series.
  if (const SonareError err = run_validation([&] {
        if ((out_momentary == nullptr) != (out_momentary_length == nullptr)) {
          util::check::refuse_relation({"out_momentary", "out_momentary_length"},
                                       "{0} and {1} must be given together");
        }
        if ((out_short_term == nullptr) != (out_short_term_length == nullptr)) {
          util::check::refuse_relation({"out_short_term", "out_short_term_length"},
                                       "{0} and {1} must be given together");
        }
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  std::vector<float> momentary;
  std::vector<float> short_term;
  metering::lufs_interleaved(samples, frames, num_channels, sample_rate, {},
                             out_momentary ? &momentary : nullptr,
                             out_short_term ? &short_term : nullptr);
  // Both copies are made before either pair is published, so a failed second
  // copy cannot leave the first series behind an error return.
  std::unique_ptr<float[]> momentary_copy(copy_vector(momentary));
  std::unique_ptr<float[]> short_term_copy(copy_vector(short_term));
  if (out_momentary) {
    *out_momentary = momentary_copy.release();
    *out_momentary_length = momentary.size();
  }
  if (out_short_term) {
    *out_short_term = short_term_copy.release();
    *out_short_term_length = short_term.size();
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_ebur128_loudness_range_lu(const float* samples, size_t length, int sample_rate,
                                             float* out_loudness_range_lu) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_loudness_range_lu);
  SONARE_C_REQUIRE_NONNULL(out_loudness_range_lu);
  return run_offline("ebur128_loudness_range_lu", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       *out_loudness_range_lu = metering::ebur128_loudness_range_lu(audio);
                       return SONARE_OK;
                     });
}

/// Handle form; it follows its buffer twin here rather than joining the other ten
/// in sonare_c_editing.cpp, which is where the twin lives.
SonareError sonare_audio_ebur128_loudness_range_lu(const SonareAudio* audio,
                                                   float* out_loudness_range_lu) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_loudness_range_lu);
  SONARE_C_REQUIRE_NONNULL(audio);
  SONARE_C_REQUIRE_NONNULL(out_loudness_range_lu);
  SONARE_C_TRY
  *out_loudness_range_lu = metering::ebur128_loudness_range_lu(audio->audio);
  return SONARE_OK;
  SONARE_C_CATCH
}
