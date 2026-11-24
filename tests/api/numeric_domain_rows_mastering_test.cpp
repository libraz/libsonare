/// @file numeric_domain_rows_mastering_test.cpp
/// @brief The mastering partition of the numeric-domain table against its C entries.

#include "c_api/numeric_domain_rows_mastering.h"

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "numeric_domain_rows_support.h"

namespace {

using namespace sonare::test::numeric_domains;
using sonare_c_detail::kMasteringDomainRows;
using sonare_c_detail::numeric_domain_table;

int as_int(double value) { return static_cast<int>(value); }

// A negative value wraps to the largest size_t, which the domain refuses from above.
size_t as_size(double value) { return static_cast<size_t>(static_cast<long long>(value)); }

bool starts_with(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

const std::vector<float>& values() {
  static const std::vector<float> data(4096, 0.5f);
  return data;
}

constexpr size_t kInterleavedFrames = 2048;

const std::vector<float>& interleaved_values() {
  static const std::vector<float> data(kInterleavedFrames * 8, 0.1f);
  return data;
}

size_t product(int a, int b) {
  return a > 0 && b > 0 ? static_cast<size_t>(a) * static_cast<size_t>(b) : 0;
}

// ---------------------------------------------------------------------------
// Segmentation
// ---------------------------------------------------------------------------

SonareError probe_segment_cross_similarity(const std::string& param, double value) {
  int x_rows = 4;
  int x_cols = 4;
  int y_rows = 4;
  int y_cols = 4;
  int k = 0;
  if (is_param(param, "x_rows")) x_rows = as_int(value);
  if (is_param(param, "x_cols")) x_cols = as_int(value);
  if (is_param(param, "y_rows")) y_rows = as_int(value);
  if (is_param(param, "y_cols")) y_cols = as_int(value);
  if (is_param(param, "k")) k = as_int(value);
  SonareSegmentMatrix out{};
  const SonareError err =
      sonare_segment_cross_similarity(values().data(), x_rows, x_cols, values().data(), y_rows,
                                      y_cols, k, "cosine", "connectivity", &out);
  if (err == SONARE_OK) sonare_free_segment_matrix(&out);
  return err;
}

SonareError probe_segment_recurrence_matrix(const std::string& param, double value) {
  int rows = 4;
  int cols = 4;
  int k = 0;
  int width = 1;
  if (is_param(param, "rows")) rows = as_int(value);
  if (is_param(param, "cols")) cols = as_int(value);
  if (is_param(param, "k")) k = as_int(value);
  if (is_param(param, "width")) width = as_int(value);
  SonareSegmentMatrix out{};
  const SonareError err = sonare_segment_recurrence_matrix(values().data(), rows, cols, k, width, 1,
                                                           "euclidean", "connectivity", &out);
  if (err == SONARE_OK) sonare_free_segment_matrix(&out);
  return err;
}

SonareError probe_segment_recurrence_to_lag(const std::string& param, double value) {
  const int n = is_param(param, "n") ? as_int(value) : 4;
  SonareSegmentMatrix out{};
  const SonareError err = sonare_segment_recurrence_to_lag(values().data(), n, 1, &out);
  if (err == SONARE_OK) sonare_free_segment_matrix(&out);
  return err;
}

SonareError probe_segment_lag_to_recurrence(const std::string& param, double value) {
  int n_rows = 4;
  int n_lags = 4;
  if (is_param(param, "n_rows")) n_rows = as_int(value);
  if (is_param(param, "n_lags")) n_lags = as_int(value);
  SonareSegmentMatrix out{};
  const SonareError err = sonare_segment_lag_to_recurrence(values().data(), n_rows, n_lags, &out);
  if (err == SONARE_OK) sonare_free_segment_matrix(&out);
  return err;
}

SonareError probe_segment_subsegment(const std::string& param, double value) {
  int rows = 4;
  int cols = 4;
  int n_segments = 2;
  if (is_param(param, "rows")) rows = as_int(value);
  if (is_param(param, "cols")) cols = as_int(value);
  if (is_param(param, "n_segments")) n_segments = as_int(value);
  const int boundaries[2] = {0, 4};
  SonareSegmentIndices out{};
  const SonareError err =
      sonare_segment_subsegment(values().data(), rows, cols, boundaries, 2, n_segments, &out);
  if (err == SONARE_OK) sonare_free_segment_indices(&out);
  return err;
}

SonareError probe_segment_agglomerative(const std::string& param, double value) {
  int rows = 4;
  int cols = 4;
  int k = 2;
  if (is_param(param, "rows")) rows = as_int(value);
  if (is_param(param, "cols")) cols = as_int(value);
  if (is_param(param, "k")) k = as_int(value);
  SonareSegmentIndices out{};
  const SonareError err =
      sonare_segment_agglomerative(values().data(), rows, cols, k, "average", &out);
  if (err == SONARE_OK) sonare_free_segment_indices(&out);
  return err;
}

SonareError probe_segment_path_enhance(const std::string& param, double value) {
  int n = 4;
  int win = 1;
  int max_ratio = 2;
  int min_ratio = 0;
  int n_filters = 7;
  if (is_param(param, "n")) n = as_int(value);
  if (is_param(param, "win")) win = as_int(value);
  if (is_param(param, "max_ratio")) max_ratio = as_int(value);
  if (is_param(param, "min_ratio")) min_ratio = as_int(value);
  if (is_param(param, "n_filters")) n_filters = as_int(value);
  SonareSegmentMatrix out{};
  const SonareError err =
      sonare_segment_path_enhance(values().data(), n, win, max_ratio, min_ratio, n_filters, &out);
  if (err == SONARE_OK) sonare_free_segment_matrix(&out);
  return err;
}

// ---------------------------------------------------------------------------
// Inverse features
// ---------------------------------------------------------------------------

struct InverseArguments {
  int n_mels = 8;
  int n_frames = 4;
  int n_mfcc = 4;
  int n_fft = 64;
  size_t hop_length = 16;
  int sample_rate = 22050;
  int n_iter = 1;
};

InverseArguments inverse_arguments(const std::string& param, double value) {
  InverseArguments a;
  if (is_param(param, "n_mels")) a.n_mels = as_int(value);
  if (is_param(param, "n_frames")) a.n_frames = as_int(value);
  if (is_param(param, "n_mfcc")) a.n_mfcc = as_int(value);
  if (is_param(param, "n_fft")) a.n_fft = as_int(value);
  if (is_param(param, "sample_rate")) a.sample_rate = as_int(value);
  if (is_param(param, "n_iter")) a.n_iter = as_int(value);
  return a;
}

SonareError finish_inverse(SonareError err, SonareInverseResult* out) {
  if (err == SONARE_OK) sonare_free_inverse_result(out);
  return err;
}

SonareError finish_audio(SonareError err, float* out) {
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

SonareError probe_mel_to_stft(const std::string& param, double value) {
  const InverseArguments a = inverse_arguments(param, value);
  SonareInverseResult out{};
  return finish_inverse(
      sonare_mel_to_stft(values().data(), product(a.n_mels, a.n_frames), a.n_mels, a.n_frames,
                         a.sample_rate, a.n_fft, 0.0f, 0.0f, nullptr, &out),
      &out);
}

SonareError probe_mel_to_audio(const std::string& param, double value) {
  const InverseArguments a = inverse_arguments(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  return finish_audio(sonare_mel_to_audio(values().data(), product(a.n_mels, a.n_frames), a.n_mels,
                                          a.n_frames, a.sample_rate, a.n_fft, a.hop_length, 0.0f,
                                          0.0f, a.n_iter, nullptr, &out, &out_length),
                      out);
}

SonareError probe_mfcc_to_mel(const std::string& param, double value) {
  const InverseArguments a = inverse_arguments(param, value);
  SonareInverseResult out{};
  return finish_inverse(sonare_mfcc_to_mel(values().data(), product(a.n_mfcc, a.n_frames), a.n_mfcc,
                                           a.n_frames, a.n_mels, nullptr, &out),
                        &out);
}

SonareError probe_mfcc_to_audio(const std::string& param, double value) {
  const InverseArguments a = inverse_arguments(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  return finish_audio(
      sonare_mfcc_to_audio(values().data(), product(a.n_mfcc, a.n_frames), a.n_mfcc, a.n_frames,
                           a.n_mels, a.sample_rate, a.n_fft, a.hop_length, 0.0f, 0.0f, a.n_iter,
                           nullptr, &out, &out_length),
      out);
}

struct SpectrogramArguments {
  int n_bins = 24;
  int n_frames = 8;
  int sample_rate = 22050;
  size_t hop_length = 512;
  int bins_per_octave = 12;
  int n_iter = 1;
};

SpectrogramArguments spectrogram_arguments(const std::string& param, double value) {
  SpectrogramArguments a;
  if (is_param(param, "n_frames")) a.n_frames = as_int(value);
  if (is_param(param, "sample_rate")) a.sample_rate = as_int(value);
  if (is_param(param, "n_iter")) a.n_iter = as_int(value);
  return a;
}

SonareError probe_cqt_to_audio(const std::string& param, double value) {
  const SpectrogramArguments a = spectrogram_arguments(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  return finish_audio(sonare_cqt_to_audio(values().data(), product(a.n_bins, a.n_frames), a.n_bins,
                                          a.n_frames, a.sample_rate, a.hop_length, 32.7f,
                                          a.bins_per_octave, a.n_iter, &out, &out_length),
                      out);
}

SonareError probe_vqt_to_audio(const std::string& param, double value) {
  const SpectrogramArguments a = spectrogram_arguments(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  return finish_audio(sonare_vqt_to_audio(values().data(), product(a.n_bins, a.n_frames), a.n_bins,
                                          a.n_frames, a.sample_rate, a.hop_length, 32.7f,
                                          a.bins_per_octave, 0.0f, a.n_iter, &out, &out_length),
                      out);
}

SonareError probe_griffin_lim(const std::string& param, double value) {
  int n_bins = 33;
  int n_fft = 64;
  SpectrogramArguments a = spectrogram_arguments(param, value);
  if (is_param(param, "n_bins")) n_bins = as_int(value);
  if (is_param(param, "n_fft")) n_fft = as_int(value);
  float* out = nullptr;
  size_t out_length = 0;
  return finish_audio(
      sonare_griffin_lim(values().data(), product(n_bins, a.n_frames), n_bins, a.n_frames, n_fft,
                         16, a.sample_rate, a.n_iter, 0.99f, &out, &out_length),
      out);
}

// ---------------------------------------------------------------------------
// Framing, metering and matrices
// ---------------------------------------------------------------------------

SonareError probe_frame_signal(const std::string& param, double value) {
  size_t frame_length = 512;
  size_t hop_length = 256;
  if (is_param(param, "frame_length")) frame_length = as_size(value);
  if (is_param(param, "hop_length")) hop_length = as_size(value);
  float* out = nullptr;
  size_t out_length = 0;
  int n_frames = 0;
  return finish_audio(sonare_frame_signal(values().data(), values().size(), frame_length,
                                          hop_length, &out, &out_length, &n_frames),
                      out);
}

SonareError probe_metering_silence_ratio(const std::string& param, double value) {
  size_t frame_length = 1024;
  size_t hop_length = 512;
  if (is_param(param, "frame_length")) frame_length = as_size(value);
  if (is_param(param, "hop_length")) hop_length = as_size(value);
  float ratio = 0.0f;
  return sonare_metering_silence_ratio(analysis_samples().data(), analysis_samples().size(),
                                       kAnalysisRate, -45.0f, frame_length, hop_length, &ratio);
}

SonareError probe_audio_silence_ratio(const std::string& param, double value) {
  size_t frame_length = 1024;
  size_t hop_length = 512;
  if (is_param(param, "frame_length")) frame_length = as_size(value);
  if (is_param(param, "hop_length")) hop_length = as_size(value);
  SonareAudio* audio = nullptr;
  REQUIRE(sonare_audio_from_buffer(analysis_samples().data(), analysis_samples().size(),
                                   kAnalysisRate, &audio) == SONARE_OK);
  float ratio = 0.0f;
  const SonareError err =
      sonare_audio_silence_ratio(audio, -45.0f, frame_length, hop_length, &ratio);
  sonare_audio_destroy(audio);
  return err;
}

SonareError probe_dynamic_range(bool handle_form, const std::string& param, double value) {
  float low_percentile = 0.1f;
  float high_percentile = 0.95f;
  if (is_param(param, "low_percentile")) low_percentile = static_cast<float>(value);
  if (is_param(param, "high_percentile")) high_percentile = static_cast<float>(value);
  SonareDynamicRangeResult out{};
  SonareError err = SONARE_OK;
  if (handle_form) {
    SonareAudio* audio = nullptr;
    REQUIRE(sonare_audio_from_buffer(analysis_samples().data(), analysis_samples().size(),
                                     kAnalysisRate, &audio) == SONARE_OK);
    err = sonare_audio_dynamic_range(audio, 0.05f, 0.025f, low_percentile, high_percentile, &out);
    sonare_audio_destroy(audio);
  } else {
    err = sonare_metering_dynamic_range(analysis_samples().data(), analysis_samples().size(),
                                        kAnalysisRate, 0.05f, 0.025f, low_percentile,
                                        high_percentile, &out);
  }
  if (err == SONARE_OK) sonare_free_dynamic_range_result(&out);
  return err;
}

SonareError probe_metering_dynamic_range(const std::string& param, double value) {
  return probe_dynamic_range(false, param, value);
}

SonareError probe_audio_dynamic_range(const std::string& param, double value) {
  return probe_dynamic_range(true, param, value);
}

/// Runs one spectrum entry with the row's member set and the other at its default (0).
template <typename Call>
SonareError probe_spectrum(const std::string& param, double value, Call call) {
  const int n_fft = is_param(param, "n_fft") ? as_int(value) : 0;
  const int octave_fraction = is_param(param, "octave_fraction") ? as_int(value) : 0;
  SonareSpectrumResult out{};
  const SonareError err = call(n_fft, octave_fraction, &out);
  sonare_free_spectrum_result(&out);
  return err;
}

SonareError probe_metering_spectrum(const std::string& param, double value) {
  return probe_spectrum(param, value, [](int n_fft, int fraction, SonareSpectrumResult* out) {
    return sonare_metering_spectrum(analysis_samples().data(), analysis_samples().size(),
                                    kAnalysisRate, n_fft, 1, fraction, 0.0f, 0.0f, out);
  });
}

SonareError probe_metering_spectrum_frame(const std::string& param, double value) {
  return probe_spectrum(param, value, [](int n_fft, int fraction, SonareSpectrumResult* out) {
    return sonare_metering_spectrum_frame(analysis_samples().data(), analysis_samples().size(),
                                          kAnalysisRate, 0, n_fft, 1, fraction, 0.0f, 0.0f, out);
  });
}

SonareError probe_audio_spectrum_entry(const std::string& param, double value, bool frame) {
  SonareAudio* audio = nullptr;
  REQUIRE(sonare_audio_from_buffer(analysis_samples().data(), analysis_samples().size(),
                                   kAnalysisRate, &audio) == SONARE_OK);
  const SonareError err =
      probe_spectrum(param, value, [&](int n_fft, int fraction, SonareSpectrumResult* out) {
        return frame ? sonare_audio_spectrum_frame(audio, 0, n_fft, 1, fraction, 0.0f, 0.0f, out)
                     : sonare_audio_spectrum(audio, n_fft, 1, fraction, 0.0f, 0.0f, out);
      });
  sonare_audio_destroy(audio);
  return err;
}

SonareError probe_audio_spectrum(const std::string& param, double value) {
  return probe_audio_spectrum_entry(param, value, false);
}

SonareError probe_audio_spectrum_frame(const std::string& param, double value) {
  return probe_audio_spectrum_entry(param, value, true);
}

SonareError probe_metering_detect_clipping(const std::string& param, double value) {
  const float threshold_db = is_param(param, "threshold_db") ? static_cast<float>(value) : -1.0f;
  SonareClippingResult out{};
  const SonareError err = sonare_metering_detect_clipping(
      analysis_samples().data(), analysis_samples().size(), kAnalysisRate, threshold_db, 1, &out);
  sonare_free_clipping_result(&out);
  return err;
}

SonareError probe_audio_detect_clipping(const std::string& param, double value) {
  const float threshold_db = is_param(param, "threshold_db") ? static_cast<float>(value) : -1.0f;
  SonareAudio* audio = nullptr;
  REQUIRE(sonare_audio_from_buffer(analysis_samples().data(), analysis_samples().size(),
                                   kAnalysisRate, &audio) == SONARE_OK);
  SonareClippingResult out{};
  const SonareError err = sonare_audio_detect_clipping(audio, threshold_db, 1, &out);
  sonare_free_clipping_result(&out);
  sonare_audio_destroy(audio);
  return err;
}

SonareError probe_lufs_interleaved(const std::string& param, double value) {
  const int channels = is_param(param, "num_channels") ? as_int(value) : 2;
  SonareLufsResult out{};
  return sonare_lufs_interleaved(interleaved_values().data(), kInterleavedFrames, channels,
                                 kAnalysisRate, &out);
}

SonareError probe_lufs_series_interleaved(const std::string& param, double value) {
  const int channels = is_param(param, "num_channels") ? as_int(value) : 2;
  float* momentary = nullptr;
  size_t momentary_length = 0;
  float* short_term = nullptr;
  size_t short_term_length = 0;
  const SonareError err = sonare_lufs_series_interleaved(
      interleaved_values().data(), kInterleavedFrames, channels, kAnalysisRate, &momentary,
      &momentary_length, &short_term, &short_term_length);
  if (err == SONARE_OK) {
    sonare_free_floats(momentary);
    sonare_free_floats(short_term);
  }
  return err;
}

SonareError probe_waveform_peaks(const std::string& param, double value) {
  const int channels = is_param(param, "num_channels") ? as_int(value) : 1;
  const size_t bucket = is_param(param, "samples_per_bucket") ? as_size(value) : 256;
  SonareWaveformPeaksResult out{};
  const SonareError err = sonare_waveform_peaks(interleaved_values().data(), kInterleavedFrames,
                                                channels, bucket, &out);
  if (err == SONARE_OK) sonare_free_waveform_peaks_result(&out);
  return err;
}

SonareError probe_waveform_peak_pyramid(const std::string& param, double value) {
  const int channels = is_param(param, "num_channels") ? as_int(value) : 1;
  const size_t levels[1] = {is_param(param, "samples_per_bucket") ? as_size(value) : 256};
  SonareWaveformPeakPyramidResult out{};
  const SonareError err = sonare_waveform_peak_pyramid(
      interleaved_values().data(), kInterleavedFrames, channels, levels, 1, &out);
  if (err == SONARE_OK) sonare_free_waveform_peak_pyramid_result(&out);
  return err;
}

SonareError probe_pcen(const std::string& param, double value) {
  int n_bins = 1;
  int n_frames = 1;
  if (is_param(param, "n_bins")) n_bins = as_int(value);
  if (is_param(param, "n_frames")) n_frames = as_int(value);
  float* out = nullptr;
  size_t out_length = 0;
  return finish_audio(sonare_pcen(values().data(), n_bins, n_frames, kAnalysisRate, 512, 0.4f,
                                  0.98f, 2.0f, 0.5f, 1e-6f, &out, &out_length),
                      out);
}

SonareError probe_tonnetz(const std::string& param, double value) {
  int n_chroma = 12;
  int n_frames = 4;
  if (is_param(param, "n_chroma")) n_chroma = as_int(value);
  if (is_param(param, "n_frames")) n_frames = as_int(value);
  float* out = nullptr;
  size_t out_length = 0;
  return finish_audio(sonare_tonnetz(values().data(), n_chroma, n_frames, &out, &out_length), out);
}

// ---------------------------------------------------------------------------
// Streaming analyzer
// ---------------------------------------------------------------------------

SonareError probe_stream_analyzer_create(const std::string& param, double value) {
  SonareStreamConfig config;
  sonare_stream_config_init(&config);
  if (is_param(param, "config.sample_rate")) config.sample_rate = as_int(value);
  if (is_param(param, "config.n_fft")) config.n_fft = as_int(value);
  if (is_param(param, "config.hop_length")) config.hop_length = as_size(value);
  if (is_param(param, "config.n_mels")) config.n_mels = as_int(value);
  if (is_param(param, "config.emit_every_n_frames")) config.emit_every_n_frames = as_int(value);
  if (is_param(param, "config.max_pending_frames")) config.max_pending_frames = as_size(value);
  if (is_param(param, "config.max_progression_entries")) {
    config.max_progression_entries = as_size(value);
  }
  SonareStreamAnalyzer* analyzer = nullptr;
  const SonareError err = sonare_stream_analyzer_create(&config, &analyzer);
  if (err == SONARE_OK) sonare_stream_analyzer_destroy(analyzer);
  return err;
}

#if defined(SONARE_WITH_ACOUSTIC_SIM)

// ---------------------------------------------------------------------------
// Room acoustics
// ---------------------------------------------------------------------------

SonareError probe_estimate_room(const std::string& param, double value) {
  SonareRoomEstimateConfig config;
  sonare_room_estimate_config_init(&config);
  if (is_param(param, "config.n_octave_bands")) config.n_octave_bands = as_int(value);
  SonareRoomEstimate out{};
  const SonareError err = sonare_estimate_room(analysis_samples().data(), analysis_samples().size(),
                                               kAnalysisRate, &config, &out);
  if (err == SONARE_OK) sonare_free_room_estimate(&out);
  return err;
}

/// Sets the room field @p param names on either room config; the air fields switch air
/// absorption on, since only then are they checked.
template <typename RoomConfig>
void set_room_field(RoomConfig& config, const std::string& param, double value) {
  const auto f = static_cast<float>(value);
  if (is_param(param, "config.absorption")) config.absorption = f;
  if (is_param(param, "config.ism_order")) config.ism_order = as_int(value);
  if (is_param(param, "config.length_m")) config.length_m = f;
  if (is_param(param, "config.width_m")) config.width_m = f;
  if (is_param(param, "config.height_m")) config.height_m = f;
  if (is_param(param, "config.max_sec")) config.max_sec = f;
  if (is_param(param, "config.mixing_time_ms")) config.mixing_time_ms = f;
  if (is_param(param, "config.crossfade_ms")) config.crossfade_ms = f;
  if (is_param(param, "config.air_temperature_c")) {
    config.air_absorption_enabled = 1;
    config.air_temperature_c = f;
  }
  if (is_param(param, "config.air_humidity_percent")) {
    config.air_absorption_enabled = 1;
    config.air_humidity_percent = f;
  }
}

// Bound synthesis and rendering even when a swept field asks for a natural tail.
constexpr std::initializer_list<ProbeField> kRoomProbeFields{{"config.max_sec", 0.0625},
                                                             {"config.absorption", 0.8}};

template <typename RoomConfig>
void set_room_probe_fields(RoomConfig& config, const std::string& param, double value) {
  set_probe_fields(param, value, kRoomProbeFields, [&](const std::string& field, double amount) {
    set_room_field(config, field, amount);
  });
}

SonareError probe_synthesize_rir(const std::string& param, double value) {
  SonareRirSynthConfig config;
  sonare_rir_synth_config_init(&config);
  int sample_rate = 48000;
  if (is_param(param, "sample_rate")) sample_rate = as_int(value);
  set_room_probe_fields(config, param, value);
  SonareRirSynthResult out{};
  const SonareError err = sonare_synthesize_rir(&config, sample_rate, &out);
  if (err == SONARE_OK) sonare_free_rir_synth_result(&out);
  return err;
}

SonareRoomMorphConfig morph_config(const std::string& param, double value) {
  SonareRoomMorphConfig config;
  sonare_room_morph_config_init(&config);
  set_room_probe_fields(config, param, value);
  if (is_param(param, "config.receiver_spacing_m")) {
    config.receiver_spacing_m = static_cast<float>(value);
  }
  return config;
}

SonareError probe_room_morph(const std::string& param, double value) {
  const SonareRoomMorphConfig config = morph_config(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  return finish_audio(sonare_room_morph(analysis_samples().data(), analysis_samples().size(),
                                        kAnalysisRate, &config, &out, &out_length),
                      out);
}

SonareError probe_room_morph_stereo(const std::string& param, double value) {
  const SonareRoomMorphConfig config = morph_config(param, value);
  float* out_left = nullptr;
  float* out_right = nullptr;
  size_t out_length = 0;
  const SonareError err = sonare_room_morph_stereo(
      analysis_samples().data(), analysis_samples().data(), analysis_samples().size(),
      kAnalysisRate, &config, &out_left, &out_right, &out_length);
  if (err == SONARE_OK) {
    sonare_free_floats(out_left);
    sonare_free_floats(out_right);
  }
  return err;
}

#endif  // SONARE_WITH_ACOUSTIC_SIM

#if defined(SONARE_WITH_MASTERING)

// ---------------------------------------------------------------------------
// Mastering repair
// ---------------------------------------------------------------------------

template <typename Result, typename Config, typename Entry, typename Release>
SonareError run_mono(Entry entry, Release release, const Config& config) {
  Result out{};
  const SonareError err =
      entry(analysis_samples().data(), analysis_samples().size(), kAnalysisRate, &config, &out);
  if (err == SONARE_OK) release(&out);
  return err;
}

template <typename Result, typename Config, typename Entry, typename Release>
SonareError run_stereo(Entry entry, Release release, const Config& config) {
  Result out{};
  const SonareError err = entry(analysis_samples().data(), analysis_samples().data(),
                                analysis_samples().size(), kAnalysisRate, &config, &out);
  if (err == SONARE_OK) release(&out);
  return err;
}

template <typename Result, typename Config, typename Entry>
SonareError run_detection(Entry entry, const Config& config) {
  Result out{};
  return entry(analysis_samples().data(), analysis_samples().size(), kAnalysisRate, &config, &out);
}

template <typename Result, typename Config, typename Entry>
SonareError run_detection_stereo(Entry entry, const Config& config) {
  Result out{};
  return entry(analysis_samples().data(), analysis_samples().data(), analysis_samples().size(),
               kAnalysisRate, &config, &out);
}

template <typename Result, typename Config, typename Entry, typename Release>
SonareError run_linked(Entry entry, Release release, const Config& config, const std::string& param,
                       double value) {
  const float* channels[2] = {analysis_samples().data(), analysis_samples().data()};
  // The array holds two planes; a probed count other than 2 is refused before it is read.
  const size_t num_channels = is_param(param, "num_channels") ? as_size(value) : 2;
  Result out{};
  const SonareError err =
      entry(channels, num_channels, analysis_samples().size(), kAnalysisRate, &config, &out);
  if (err == SONARE_OK) release(&out);
  return err;
}

SonareDeclickConfig declick_config(const std::string& param, double value) {
  SonareDeclickConfig config;
  sonare_declick_config_init(&config);
  if (is_param(param, "config.lpc_order")) config.lpc_order = as_int(value);
  if (is_param(param, "config.max_click_samples")) config.max_click_samples = as_size(value);
  return config;
}

SonareDeclipConfig declip_config(const std::string& param, double value) {
  SonareDeclipConfig config;
  sonare_declip_config_init(&config);
  if (is_param(param, "config.lpc_order")) config.lpc_order = as_int(value);
  if (is_param(param, "config.iterations")) config.iterations = as_int(value);
  return config;
}

SonareDecrackleConfig decrackle_config(const std::string& param, double value) {
  SonareDecrackleConfig config;
  sonare_decrackle_config_init(&config);
  if (is_param(param, "config.levels")) config.levels = as_int(value);
  return config;
}

SonareDehumConfig dehum_config(const std::string& param, double value) {
  SonareDehumConfig config;
  sonare_dehum_config_init(&config);
  if (is_param(param, "config.harmonics")) config.harmonics = as_int(value);
  if (is_param(param, "config.frame_size")) config.frame_size = as_int(value);
  return config;
}

SonareDenoiseClassicalConfig denoise_config(const std::string& param, double value) {
  SonareDenoiseClassicalConfig config;
  sonare_denoise_classical_config_init(&config);
  if (is_param(param, "config.n_fft")) config.n_fft = as_int(value);
  return config;
}

SonareDereverbClassicalConfig dereverb_config(const std::string& param, double value) {
  SonareDereverbClassicalConfig config;
  sonare_dereverb_classical_config_init(&config);
  if (is_param(param, "config.n_fft")) config.n_fft = as_int(value);
  if (is_param(param, "config.wpe_iterations")) config.wpe_iterations = as_int(value);
  if (is_param(param, "config.wpe_taps")) config.wpe_taps = as_int(value);
  return config;
}

SonareTrimSilenceConfig trim_config(const std::string& param, double value) {
  SonareTrimSilenceConfig config;
  sonare_trim_silence_config_init(&config);
  if (is_param(param, "config.padding_samples")) config.padding_samples = as_size(value);
  return config;
}

SonareError probe_repair_declick(const std::string& param, double value) {
  return run_mono<SonareDeclickResult>(sonare_mastering_repair_declick, sonare_free_declick_result,
                                       declick_config(param, value));
}

SonareError probe_repair_declick_stereo(const std::string& param, double value) {
  return run_stereo<SonareDeclickStereoResult>(sonare_mastering_repair_declick_stereo,
                                               sonare_free_declick_stereo_result,
                                               declick_config(param, value));
}

SonareError probe_repair_detect_clicks(const std::string& param, double value) {
  return run_detection<SonareClickDetection>(sonare_mastering_repair_detect_clicks,
                                             declick_config(param, value));
}

SonareError probe_repair_declip(const std::string& param, double value) {
  return run_mono<SonareDeclipResult>(sonare_mastering_repair_declip, sonare_free_declip_result,
                                      declip_config(param, value));
}

SonareError probe_repair_declip_stereo(const std::string& param, double value) {
  return run_stereo<SonareDeclipStereoResult>(sonare_mastering_repair_declip_stereo,
                                              sonare_free_declip_stereo_result,
                                              declip_config(param, value));
}

SonareError probe_repair_detect_clipping(const std::string& param, double value) {
  return run_detection<SonareClipDetection>(sonare_mastering_repair_detect_clipping,
                                            declip_config(param, value));
}

SonareError probe_repair_decrackle(const std::string& param, double value) {
  return run_mono<SonareDecrackleResult>(sonare_mastering_repair_decrackle,
                                         sonare_free_decrackle_result,
                                         decrackle_config(param, value));
}

SonareError probe_repair_decrackle_stereo(const std::string& param, double value) {
  return run_stereo<SonareDecrackleStereoResult>(sonare_mastering_repair_decrackle_stereo,
                                                 sonare_free_decrackle_stereo_result,
                                                 decrackle_config(param, value));
}

SonareError probe_repair_detect_crackle(const std::string& param, double value) {
  return run_detection<SonareCrackleDetection>(sonare_mastering_repair_detect_crackle,
                                               decrackle_config(param, value));
}

SonareError probe_repair_dehum(const std::string& param, double value) {
  return run_mono<SonareDehumResult>(sonare_mastering_repair_dehum, sonare_free_dehum_result,
                                     dehum_config(param, value));
}

SonareError probe_repair_dehum_stereo(const std::string& param, double value) {
  return run_stereo<SonareDehumStereoResult>(sonare_mastering_repair_dehum_stereo,
                                             sonare_free_dehum_stereo_result,
                                             dehum_config(param, value));
}

SonareError probe_repair_detect_hum(const std::string& param, double value) {
  return run_detection<SonareHumDetection>(sonare_mastering_repair_detect_hum,
                                           dehum_config(param, value));
}

SonareError probe_repair_denoise(const std::string& param, double value) {
  return run_mono<SonareDenoiseResult>(sonare_mastering_repair_denoise_classical,
                                       sonare_free_denoise_result, denoise_config(param, value));
}

SonareError probe_repair_denoise_stereo(const std::string& param, double value) {
  return run_stereo<SonareDenoiseStereoResult>(sonare_mastering_repair_denoise_classical_stereo,
                                               sonare_free_denoise_stereo_result,
                                               denoise_config(param, value));
}

SonareError probe_repair_denoise_linked(const std::string& param, double value) {
  return run_linked<SonareDenoiseLinkedResult>(sonare_mastering_repair_denoise_classical_linked,
                                               sonare_free_denoise_linked_result,
                                               denoise_config(param, value), param, value);
}

SonareError probe_repair_detect_noise_floor(const std::string& param, double value) {
  return run_detection<SonareNoiseDetection>(sonare_mastering_repair_detect_noise_floor,
                                             denoise_config(param, value));
}

SonareError probe_repair_dereverb(const std::string& param, double value) {
  return run_mono<SonareDereverbResult>(sonare_mastering_repair_dereverb_classical,
                                        sonare_free_dereverb_result, dereverb_config(param, value));
}

SonareError probe_repair_dereverb_stereo(const std::string& param, double value) {
  return run_stereo<SonareDereverbStereoResult>(sonare_mastering_repair_dereverb_classical_stereo,
                                                sonare_free_dereverb_stereo_result,
                                                dereverb_config(param, value));
}

SonareError probe_repair_dereverb_linked(const std::string& param, double value) {
  return run_linked<SonareDereverbLinkedResult>(sonare_mastering_repair_dereverb_classical_linked,
                                                sonare_free_dereverb_linked_result,
                                                dereverb_config(param, value), param, value);
}

SonareError probe_repair_detect_reverb(const std::string& param, double value) {
  return run_detection<SonareReverbDetection>(sonare_mastering_repair_detect_reverb,
                                              dereverb_config(param, value));
}

SonareError probe_repair_trim_silence(const std::string& param, double value) {
  return run_mono<SonareTrimSilenceResult>(sonare_mastering_repair_trim_silence,
                                           sonare_free_trim_silence_result,
                                           trim_config(param, value));
}

SonareError probe_repair_trim_silence_stereo(const std::string& param, double value) {
  return run_stereo<SonareTrimSilenceStereoResult>(sonare_mastering_repair_trim_silence_stereo,
                                                   sonare_free_trim_silence_stereo_result,
                                                   trim_config(param, value));
}

SonareError probe_repair_detect_trim_range(const std::string& param, double value) {
  return run_detection<SonareTrimRange>(sonare_mastering_repair_detect_trim_range,
                                        trim_config(param, value));
}

SonareError probe_repair_detect_trim_range_stereo(const std::string& param, double value) {
  return run_detection_stereo<SonareTrimRange>(sonare_mastering_repair_detect_trim_range_stereo,
                                               trim_config(param, value));
}

SonareError probe_repair_analyze(const std::string& param, double value) {
  const size_t num_channels = is_param(param, "num_channels") ? as_size(value) : 1;
  const float* channels[1] = {analysis_samples().data()};
  char* json = nullptr;
  const SonareError err = sonare_mastering_repair_analyze(
      channels, num_channels, analysis_samples().size(), kAnalysisRate, nullptr, &json);
  if (err == SONARE_OK) sonare_free_string(json);
  return err;
}

SonareError probe_repair_apply(const std::string& param, double value) {
  const size_t num_channels = is_param(param, "num_channels") ? as_size(value) : 1;
  const float* channels[1] = {analysis_samples().data()};
  std::vector<float> plane(analysis_samples().size());
  float* planes[1] = {plane.data()};
  char* json = nullptr;
  const SonareError err = sonare_mastering_repair_apply(
      channels, num_channels, analysis_samples().size(), kAnalysisRate, nullptr, nullptr, nullptr,
      planes, &json, nullptr, nullptr);
  if (err == SONARE_OK) sonare_free_string(json);
  return err;
}

SonareError probe_repair_noise_band_bins(const std::string& param, double value) {
  int n_fft = 2048;
  int sample_rate = kAnalysisRate;
  if (is_param(param, "n_fft")) n_fft = as_int(value);
  if (is_param(param, "sample_rate")) sample_rate = as_int(value);
  int bins[SONARE_REPAIR_NOISE_BAND_EDGE_COUNT] = {};
  return sonare_mastering_repair_noise_band_bins(n_fft, sample_rate, bins);
}

// ---------------------------------------------------------------------------
// Mastering processors, profile, equalizer and streaming chain
// ---------------------------------------------------------------------------

SonareError probe_analyze_stereo(const std::string&, double value) {
  const SonareMasteringParam parameter{"bandsPerOctave", value};
  char* json = nullptr;
  const SonareError err = sonare_mastering_analyze_stereo(
      "stereo.monoCompatCheckLogBands", analysis_samples().data(), analysis_samples().data(),
      analysis_samples().size(), kAnalysisRate, &parameter, 1, &json);
  if (err == SONARE_OK) sonare_free_string(json);
  return err;
}

SonareMasteringParam processor_param(const std::string& param, double value) {
  if (is_param(param, "params.delay_samples")) return {"delaySamples", value};
  return {"bitDepth", value};
}

SonareError probe_apply_processor(const std::string& param, double value) {
  const SonareMasteringParam parameter = processor_param(param, value);
  SonareMasteringResult out{};
  const SonareError err = sonare_mastering_apply_processor(
      "saturation.bitcrusher", analysis_samples().data(), analysis_samples().size(), kAnalysisRate,
      &parameter, 1, &out);
  if (err == SONARE_OK) sonare_free_mastering_result(&out);
  return err;
}

SonareError probe_apply_processor_stereo(const std::string& param, double value) {
  const SonareMasteringParam parameter = processor_param(param, value);
  const char* processor =
      is_param(param, "params.delay_samples") ? "stereo.phaseAlign" : "saturation.bitcrusher";
  SonareMasteringStereoResult out{};
  const SonareError err = sonare_mastering_apply_processor_stereo(
      processor, analysis_samples().data(), analysis_samples().data(), analysis_samples().size(),
      kAnalysisRate, &parameter, 1, &out);
  if (err == SONARE_OK) sonare_free_mastering_stereo_result(&out);
  return err;
}

SonareMasteringParam profile_param(const std::string& param, double value) {
  if (is_param(param, "params.hopLength")) return {"hopLength", value};
  return {"nFft", value};
}

SonareError probe_audio_profile(const std::string& param, double value) {
  const SonareMasteringParam parameter = profile_param(param, value);
  char* json = nullptr;
  const SonareError err = sonare_mastering_audio_profile(
      analysis_samples().data(), analysis_samples().size(), kAnalysisRate, &parameter, 1, &json);
  if (err == SONARE_OK) sonare_free_string(json);
  return err;
}

SonareError probe_audio_profile_stereo(const std::string& param, double value) {
  const SonareMasteringParam parameter = profile_param(param, value);
  char* json = nullptr;
  const SonareError err = sonare_mastering_audio_profile_stereo(
      analysis_samples().data(), analysis_samples().data(), analysis_samples().size(),
      kAnalysisRate, &parameter, 1, &json);
  if (err == SONARE_OK) sonare_free_string(json);
  return err;
}

SonareError probe_eq_set_band(const std::string& param, double value) {
  SonareEq* eq = nullptr;
  REQUIRE(sonare_eq_create(48000, 0, &eq) == SONARE_OK);
  const int index = is_param(param, "index") ? as_int(value) : 0;
  const SonareError err = sonare_eq_set_band(eq, index, "{}");
  sonare_eq_destroy(eq);
  return err;
}

SonareError probe_eq_match(const std::string& param, double value) {
  SonareEq* eq = nullptr;
  REQUIRE(sonare_eq_create(48000, 0, &eq) == SONARE_OK);
  const int max_bands = is_param(param, "max_bands") ? as_int(value) : 8;
  const SonareError err = sonare_eq_match(eq, analysis_samples().data(), analysis_samples().size(),
                                          kAnalysisRate, analysis_samples().data(),
                                          analysis_samples().size(), kAnalysisRate, max_bands);
  sonare_eq_destroy(eq);
  return err;
}

SonareError probe_streaming_chain_prepare(const std::string& param, double value) {
  SonareStreamingMasteringChain* chain = nullptr;
  REQUIRE(sonare_streaming_mastering_chain_create(nullptr, nullptr, &chain) == SONARE_OK);
  const int num_channels = is_param(param, "num_channels") ? as_int(value) : 2;
  const SonareError err = sonare_streaming_mastering_chain_prepare(chain, 48000, 512, num_channels);
  sonare_streaming_mastering_chain_destroy(chain);
  return err;
}

#endif  // SONARE_WITH_MASTERING

const std::map<std::string, Probe>& probes() {
  static const std::map<std::string, Probe> table = {
      {"audio_silence_ratio", probe_audio_silence_ratio},
      {"audio_dynamic_range", probe_audio_dynamic_range},
      {"audio_detect_clipping", probe_audio_detect_clipping},
      {"audio_spectrum", probe_audio_spectrum},
      {"audio_spectrum_frame", probe_audio_spectrum_frame},
      {"cqt_to_audio", probe_cqt_to_audio},
      {"frame_signal", probe_frame_signal},
      {"griffin_lim", probe_griffin_lim},
      {"lufs_interleaved", probe_lufs_interleaved},
      {"lufs_series_interleaved", probe_lufs_series_interleaved},
      {"mel_to_audio", probe_mel_to_audio},
      {"mel_to_stft", probe_mel_to_stft},
      {"metering_silence_ratio", probe_metering_silence_ratio},
      {"metering_dynamic_range", probe_metering_dynamic_range},
      {"metering_detect_clipping", probe_metering_detect_clipping},
      {"metering_spectrum", probe_metering_spectrum},
      {"metering_spectrum_frame", probe_metering_spectrum_frame},
      {"mfcc_to_audio", probe_mfcc_to_audio},
      {"mfcc_to_mel", probe_mfcc_to_mel},
      {"pcen", probe_pcen},
      {"segment_agglomerative", probe_segment_agglomerative},
      {"segment_cross_similarity", probe_segment_cross_similarity},
      {"segment_lag_to_recurrence", probe_segment_lag_to_recurrence},
      {"segment_path_enhance", probe_segment_path_enhance},
      {"segment_recurrence_matrix", probe_segment_recurrence_matrix},
      {"segment_recurrence_to_lag", probe_segment_recurrence_to_lag},
      {"segment_subsegment", probe_segment_subsegment},
      {"stream_analyzer_create", probe_stream_analyzer_create},
      {"tonnetz", probe_tonnetz},
      {"vqt_to_audio", probe_vqt_to_audio},
      {"waveform_peak_pyramid", probe_waveform_peak_pyramid},
      {"waveform_peaks", probe_waveform_peaks},
#if defined(SONARE_WITH_ACOUSTIC_SIM)
      {"estimate_room", probe_estimate_room},
      {"room_morph", probe_room_morph},
      {"room_morph_stereo", probe_room_morph_stereo},
      {"synthesize_rir", probe_synthesize_rir},
#endif
#if defined(SONARE_WITH_MASTERING)
      {"eq_match", probe_eq_match},
      {"eq_set_band", probe_eq_set_band},
      {"mastering_analyze_stereo", probe_analyze_stereo},
      {"mastering_apply_processor", probe_apply_processor},
      {"mastering_apply_processor_stereo", probe_apply_processor_stereo},
      {"mastering_audio_profile", probe_audio_profile},
      {"mastering_audio_profile_stereo", probe_audio_profile_stereo},
      {"mastering_repair_analyze", probe_repair_analyze},
      {"mastering_repair_apply", probe_repair_apply},
      {"mastering_repair_declick", probe_repair_declick},
      {"mastering_repair_declick_stereo", probe_repair_declick_stereo},
      {"mastering_repair_declip", probe_repair_declip},
      {"mastering_repair_declip_stereo", probe_repair_declip_stereo},
      {"mastering_repair_decrackle", probe_repair_decrackle},
      {"mastering_repair_decrackle_stereo", probe_repair_decrackle_stereo},
      {"mastering_repair_dehum", probe_repair_dehum},
      {"mastering_repair_dehum_stereo", probe_repair_dehum_stereo},
      {"mastering_repair_denoise_classical", probe_repair_denoise},
      {"mastering_repair_denoise_classical_linked", probe_repair_denoise_linked},
      {"mastering_repair_denoise_classical_stereo", probe_repair_denoise_stereo},
      {"mastering_repair_dereverb_classical", probe_repair_dereverb},
      {"mastering_repair_dereverb_classical_linked", probe_repair_dereverb_linked},
      {"mastering_repair_dereverb_classical_stereo", probe_repair_dereverb_stereo},
      {"mastering_repair_detect_clicks", probe_repair_detect_clicks},
      {"mastering_repair_detect_clipping", probe_repair_detect_clipping},
      {"mastering_repair_detect_crackle", probe_repair_detect_crackle},
      {"mastering_repair_detect_hum", probe_repair_detect_hum},
      {"mastering_repair_detect_noise_floor", probe_repair_detect_noise_floor},
      {"mastering_repair_detect_reverb", probe_repair_detect_reverb},
      {"mastering_repair_detect_trim_range", probe_repair_detect_trim_range},
      {"mastering_repair_detect_trim_range_stereo", probe_repair_detect_trim_range_stereo},
      {"mastering_repair_noise_band_bins", probe_repair_noise_band_bins},
      {"mastering_repair_trim_silence", probe_repair_trim_silence},
      {"mastering_repair_trim_silence_stereo", probe_repair_trim_silence_stereo},
      {"streaming_mastering_chain_prepare", probe_streaming_chain_prepare},
#endif
  };
  return table;
}

/// Whether this build leaves the entry out: the mastering, equalizer and streaming-chain entries
/// need mastering, the room entries need the acoustic simulation.
bool compiled_out([[maybe_unused]] const std::string& entry) {
#if !defined(SONARE_WITH_MASTERING)
  if (starts_with(entry, "mastering_") || starts_with(entry, "eq_") ||
      starts_with(entry, "streaming_mastering_")) {
    return true;
  }
#endif
#if !defined(SONARE_WITH_ACOUSTIC_SIM)
  if (entry == "estimate_room" || entry == "synthesize_rir" || entry == "room_morph" ||
      entry == "room_morph_stereo") {
    return true;
  }
#endif
  return false;
}

/// The rows whose entry this build compiles.
std::vector<NumericDomainRow> built_rows() {
  std::vector<NumericDomainRow> rows;
  for (const NumericDomainRow& row : kMasteringDomainRows) {
    if (!compiled_out(row.entry)) rows.push_back(row);
  }
  return rows;
}

NumericDomainTable built_table() {
  static const std::vector<NumericDomainRow> rows = built_rows();
  return {"mastering", rows.data(), rows.size()};
}

/// A working value inside the domain where the lowest one is degenerate for the entry or breaks a
/// relation to another argument (a spectrogram's bin count to its FFT size, a matrix extent to its
/// partner); the lowest accepted value elsewhere.
double mastering_inside_value(const NumericDomainRow& row) {
  const std::string entry = row.entry;
  const std::string param = row.param;
  if ((entry == "audio_dynamic_range" || entry == "metering_dynamic_range") &&
      param == "high_percentile") {
    return 0.95;
  }
  const bool integer_from_one = row.domain.kind == NumberKind::Integer && *row.domain.lo == 1.0;
  if (starts_with(entry, "segment_")) return integer_from_one ? 4.0 : lowest_inside_value(row);
  if (starts_with(entry, "mel_to_") || starts_with(entry, "mfcc_to_")) {
    if (param == "n_mels") return 8.0;
    if (param == "n_frames" || param == "n_mfcc") return 4.0;
    if (param == "n_fft") return 64.0;
    if (param == "sample_rate") return kAnalysisRate;
    if (param == "n_iter") return 1.0;
  }
  if (starts_with(entry, "cqt_to_") || starts_with(entry, "vqt_to_") || entry == "griffin_lim") {
    if (param == "n_frames") return 8.0;
    if (param == "n_bins") return 33.0;
    if (param == "n_fft") return 64.0;
    if (param == "n_iter") return 1.0;
  }
  if (entry == "tonnetz" && param == "n_chroma") return 12.0;
  if (entry == "pcen" || entry == "tonnetz") return 4.0;
  // A room that holds the default placement, and a receiver pair that fits inside it.
  if (param == "config.length_m" || param == "config.width_m" || param == "config.height_m") {
    return 6.0;
  }
  if (param == "config.receiver_spacing_m") return 0.2;
  if (entry == "stream_analyzer_create" && param == "config.n_fft") return 2048.0;
  if (entry == "mastering_repair_noise_band_bins") return param == "n_fft" ? 2048.0 : kAnalysisRate;
  if (param == "config.n_fft" && starts_with(entry, "mastering_repair_")) return 2048.0;
  if (starts_with(entry, "mastering_audio_profile")) return param == "params.nFft" ? 2048.0 : 512.0;
  return lowest_inside_value(row);
}

}  // namespace

TEST_CASE("the mastering numeric-domain rows are sorted", "[numeric_domains][c_api]") {
  require_sorted(numeric_domain_table("mastering", kMasteringDomainRows));
}

TEST_CASE("a mastering value outside its row's domain is refused under the row's subject",
          "[numeric_domains][c_api]") {
  require_outside_refused(built_table(), probes());
}

TEST_CASE("a mastering value inside its row's domain is not refused under the row's subject",
          "[numeric_domains][c_api]") {
  require_inside_accepted(built_table(), probes(), mastering_inside_value);
}

#if defined(SONARE_WITH_ACOUSTIC_SIM)
TEST_CASE("room morph stereo accepts zero absorption and refuses negative absorption",
          "[numeric_domains][c_api]") {
  CHECK(probe_room_morph_stereo("config.absorption", 0.0) == SONARE_OK);
  CHECK(probe_room_morph_stereo("config.absorption", -0.001) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_subject() == "config.absorption");
}
#endif

TEST_CASE("dynamic-range percentile rows keep the sentinel and refuse outside [0, 1]",
          "[numeric_domains][c_api][dynamic-range-domain]") {
  std::vector<NumericDomainRow> rows;
  for (const NumericDomainRow& row : kMasteringDomainRows) {
    const std::string entry = row.entry;
    if ((entry == "audio_dynamic_range" || entry == "metering_dynamic_range") &&
        (std::string(row.param) == "low_percentile" ||
         std::string(row.param) == "high_percentile")) {
      rows.push_back(row);
    }
  }
  const NumericDomainTable table{"mastering", rows.data(), rows.size()};
  require_outside_refused(table, probes());
  require_inside_accepted(table, probes(), mastering_inside_value);
}

TEST_CASE("a spectrum FFT size is accepted up to the meter's resource limit and refused past it",
          "[numeric_domains][c_api]") {
  constexpr double kLimit = 4194304.0;
  const std::map<std::string, Probe> entries = probes();
  for (const char* entry :
       {"audio_spectrum", "audio_spectrum_frame", "metering_spectrum", "metering_spectrum_frame"}) {
    INFO(entry);
    const Probe& probe = entries.at(entry);
    CHECK(probe("n_fft", kLimit) == SONARE_OK);
    REQUIRE(probe("n_fft", kLimit * 2.0) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_subject() == "n_fft");
    CHECK(std::string(sonare_last_error_message())
              .find(sonare::util::refusal::power_of_two_predicate(2, 4194304)) !=
          std::string::npos);
  }
}
