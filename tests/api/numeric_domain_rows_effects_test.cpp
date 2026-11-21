/// @file numeric_domain_rows_effects_test.cpp
/// @brief The effects, engine and DAW partition of the numeric-domain table against its C entries.

#include "c_api/numeric_domain_rows_effects.h"

#include <sonare/sonare_c.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "numeric_domain_rows_support.h"

namespace {

using namespace sonare::test::numeric_domains;
using sonare_c_detail::kEffectsDomainRows;
using sonare_c_detail::numeric_domain_table;

std::string field_of(const std::string& param) { return param.substr(param.rfind('.') + 1); }

bool ends_with(const std::string& text, const char* suffix) {
  const std::string tail = suffix;
  return text.size() >= tail.size() &&
         text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

// A probe value reaches an unsigned argument through a signed integer, so a value below zero wraps
// to the largest unsigned instead of being an undefined conversion.
std::int64_t as_i64(double value) { return static_cast<std::int64_t>(value); }
std::uint32_t as_u32(double value) { return static_cast<std::uint32_t>(as_i64(value)); }
std::uint8_t as_u8(double value) { return static_cast<std::uint8_t>(as_i64(value)); }
std::size_t as_size(double value) {
  return value < 0.0 ? std::numeric_limits<std::size_t>::max() : static_cast<std::size_t>(value);
}

/// An engine prepared at 48 kHz, destroyed with the scope.
class PreparedEngine {
 public:
  PreparedEngine() {
    REQUIRE(sonare_engine_create(&engine_) == SONARE_OK);
    REQUIRE(sonare_engine_prepare(engine_, 48000.0, 64, 32, 16) == SONARE_OK);
  }
  ~PreparedEngine() { sonare_engine_destroy(engine_); }
  PreparedEngine(const PreparedEngine&) = delete;
  PreparedEngine& operator=(const PreparedEngine&) = delete;
  SonareEngine* get() const { return engine_; }

 private:
  SonareEngine* engine_ = nullptr;
};

// ---------------------------------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------------------------------

SonareError run_decompose(const std::string& param, double value, bool with_init) {
  int n_features = 2;
  int n_frames = 2;
  int n_components = 1;
  int n_iter = 1;
  if (is_param(param, "n_features")) n_features = static_cast<int>(value);
  if (is_param(param, "n_frames")) n_frames = static_cast<int>(value);
  if (is_param(param, "n_components")) n_components = static_cast<int>(value);
  if (is_param(param, "n_iter")) n_iter = static_cast<int>(value);
  const std::vector<float> s(4, 1.0f);
  float* w = nullptr;
  float* h = nullptr;
  size_t w_length = 0;
  size_t h_length = 0;
  const SonareError err =
      with_init ? sonare_decompose_with_init(s.data(), n_features, n_frames, n_components, n_iter,
                                             2.0f, "random", &w, &w_length, &h, &h_length)
                : sonare_decompose(s.data(), n_features, n_frames, n_components, n_iter, 2.0f, &w,
                                   &w_length, &h, &h_length);
  if (err == SONARE_OK) {
    sonare_free_floats(w);
    sonare_free_floats(h);
  }
  return err;
}

SonareError probe_decompose(const std::string& param, double value) {
  return run_decompose(param, value, false);
}

SonareError probe_decompose_with_init(const std::string& param, double value) {
  return run_decompose(param, value, true);
}

SonareError probe_nn_filter(const std::string& param, double value) {
  int n_features = 2;
  int n_frames = 2;
  if (is_param(param, "n_features")) n_features = static_cast<int>(value);
  if (is_param(param, "n_frames")) n_frames = static_cast<int>(value);
  const std::vector<float> s(4, 1.0f);
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_nn_filter(s.data(), n_features, n_frames, "mean", -1, -1, &out, &out_length);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

void set_stems_config(SonareDecomposeStemsConfig* config, const std::string& param, double value) {
  if (is_param(param, "config.n_components")) config->n_components = static_cast<int>(value);
  if (is_param(param, "config.n_fft")) config->n_fft = static_cast<int>(value);
  if (is_param(param, "config.n_iter")) config->n_iter = static_cast<int>(value);
}

SonareError probe_decompose_stems(const std::string& param, double value) {
  SonareDecomposeStemsConfig config{};
  sonare_decompose_stems_config_init(&config);
  config.n_iter = 1;
  set_stems_config(&config, param, value);
  float* out = nullptr;
  size_t count = 0;
  size_t length = 0;
  const SonareError err =
      sonare_decompose_stems(analysis_samples().data(), analysis_samples().size(), kAnalysisRate,
                             &config, &out, &count, &length, nullptr, nullptr, nullptr, nullptr);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

SonareError probe_decompose_stems_linked(const std::string& param, double value) {
  SonareDecomposeStemsConfig config{};
  sonare_decompose_stems_config_init(&config);
  config.n_iter = 1;
  set_stems_config(&config, param, value);
  const float* channels[2] = {analysis_samples().data(), analysis_samples().data()};
  const size_t num_channels = is_param(param, "num_channels") ? as_size(value) : 1;
  float* out = nullptr;
  size_t count = 0;
  size_t out_channels = 0;
  size_t length = 0;
  const SonareError err = sonare_decompose_stems_linked(
      channels, num_channels, analysis_samples().size(), kAnalysisRate, &config, &out, &count,
      &out_channels, &length, nullptr, nullptr, nullptr, nullptr);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

SonareError probe_hpss_ex(const std::string& param, double value) {
  const int n_fft = is_param(param, "n_fft") ? static_cast<int>(value) : 2048;
  SonareHpssResult out{};
  const SonareError err =
      sonare_hpss_ex(analysis_samples().data(), analysis_samples().size(), kAnalysisRate, 31, 31,
                     n_fft, SONARE_STFT_HOP_AUTO, 0, 0, &out, nullptr);
  if (err == SONARE_OK) sonare_free_hpss_result(&out);
  return err;
}

SonareError probe_phase_vocoder(const std::string& param, double value) {
  const int n_fft = is_param(param, "n_fft") ? static_cast<int>(value) : 2048;
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_phase_vocoder(analysis_samples().data(), analysis_samples().size(), kAnalysisRate,
                           1.5f, n_fft, SONARE_STFT_HOP_AUTO, &out, &out_length);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

SonareError probe_time_stretch_ex(const std::string& param, double value) {
  const int n_fft = is_param(param, "n_fft") ? static_cast<int>(value) : 2048;
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_time_stretch_ex(analysis_samples().data(), analysis_samples().size(), kAnalysisRate,
                             1.5f, n_fft, SONARE_STFT_HOP_AUTO, &out, &out_length);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

SonareError probe_pitch_shift_ex(const std::string& param, double value) {
  const int n_fft = is_param(param, "n_fft") ? static_cast<int>(value) : 2048;
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_pitch_shift_ex(analysis_samples().data(), analysis_samples().size(), kAnalysisRate,
                            2.0f, n_fft, SONARE_STFT_HOP_AUTO, &out, &out_length);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

SonareError probe_biquad_filter(const std::string& param, double value) {
  SonareBiquadFilterConfig config{};
  sonare_biquad_filter_config_init(&config);
  config.cutoff_hz = 1000.0f;
  if (is_param(param, "config.type")) config.type = static_cast<int32_t>(value);
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err = sonare_biquad_filter(analysis_samples().data(), analysis_samples().size(),
                                               kAnalysisRate, &config, &out, &out_length);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

struct SilenceFraming {
  int frame_length = 2048;
  int hop_length = 512;
};

SilenceFraming silence_framing(const std::string& param, double value) {
  SilenceFraming framing;
  if (is_param(param, "frame_length")) framing.frame_length = static_cast<int>(value);
  if (is_param(param, "hop_length")) framing.hop_length = static_cast<int>(value);
  return framing;
}

SonareError probe_trim_ex(const std::string& param, double value) {
  const SilenceFraming framing = silence_framing(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_trim_ex(analysis_samples().data(), analysis_samples().size(), kAnalysisRate, -60.0f,
                     framing.frame_length, framing.hop_length, &out, &out_length);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

SonareError probe_trim_silence(const std::string& param, double value) {
  const SilenceFraming framing = silence_framing(param, value);
  float* out = nullptr;
  size_t out_length = 0;
  int start = 0;
  int end = 0;
  const SonareError err = sonare_trim_silence(analysis_samples().data(), analysis_samples().size(),
                                              60.0f, framing.frame_length, framing.hop_length, &out,
                                              &out_length, &start, &end);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

SonareError probe_split_silence(const std::string& param, double value) {
  const SilenceFraming framing = silence_framing(param, value);
  int* intervals = nullptr;
  size_t count = 0;
  const SonareError err =
      sonare_split_silence(analysis_samples().data(), analysis_samples().size(), 60.0f,
                           framing.frame_length, framing.hop_length, &intervals, &count);
  if (err == SONARE_OK) sonare_free_ints(intervals);
  return err;
}

SonareError probe_split_silence_common(const std::string& param, double value) {
  const SilenceFraming framing = silence_framing(param, value);
  const float* signals[1] = {analysis_samples().data()};
  const size_t lengths[1] = {analysis_samples().size()};
  int* intervals = nullptr;
  size_t count = 0;
  const SonareError err = sonare_split_silence_common(
      signals, 1, lengths, 60.0f, framing.frame_length, framing.hop_length, &intervals, &count);
  if (err == SONARE_OK) sonare_free_ints(intervals);
  return err;
}

SonareError probe_split_silence_common_ex(const std::string& param, double value) {
  const SilenceFraming framing = silence_framing(param, value);
  const float* signals[1] = {analysis_samples().data()};
  const size_t lengths[1] = {analysis_samples().size()};
  int* intervals = nullptr;
  size_t count = 0;
  SonareSilenceCommonReport report{};
  const SonareError err =
      sonare_split_silence_common_ex(signals, 1, lengths, 60.0f, framing.frame_length,
                                     framing.hop_length, &intervals, &count, &report);
  if (err == SONARE_OK) sonare_free_ints(intervals);
  return err;
}

SonareError probe_spectral_edit(const std::string& param, double value) {
  SonareSpectralEditConfig config{};
  sonare_spectral_edit_config_init(&config);
  SonareSpectralRegionOp op{};
  sonare_spectral_region_op_init(&op);
  if (is_param(param, "config.n_fft")) config.n_fft = static_cast<int>(value);
  if (is_param(param, "config.heal_radius_frames")) {
    config.heal_radius_frames = static_cast<int>(value);
  }
  if (is_param(param, "config.window")) config.window = static_cast<int>(value);
  const bool with_op = is_param(param, "ops[].mode");
  if (with_op) op.mode = static_cast<int>(value);
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_spectral_edit(analysis_samples().data(), analysis_samples().size(), kAnalysisRate,
                           &config, with_op ? &op : nullptr, with_op ? 1 : 0, &out, &out_length);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

// ---------------------------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------------------------

SonareError probe_clip_page_provider_create(const std::string& param, double value) {
  int num_channels = 1;
  std::int64_t num_samples = 16;
  std::int64_t page_frames = 16;
  if (is_param(param, "num_channels")) num_channels = static_cast<int>(value);
  if (is_param(param, "num_samples")) num_samples = as_i64(value);
  if (is_param(param, "page_frames")) page_frames = as_i64(value);
  SonareClipPageProvider* provider = nullptr;
  const SonareError err =
      sonare_clip_page_provider_create(num_channels, num_samples, page_frames, &provider);
  if (err == SONARE_OK) sonare_clip_page_provider_destroy(provider);
  return err;
}

SonareError probe_engine_set_capture_buffer(const std::string& param, double value) {
  PreparedEngine engine;
  std::array<float, 4> plane{};
  float* channels[1] = {plane.data()};
  SonareEngineCaptureBuffer buffer{channels, 1, 4};
  if (is_param(param, "buffer.num_channels")) buffer.num_channels = static_cast<int>(value);
  if (is_param(param, "buffer.capacity_frames")) buffer.capacity_frames = as_i64(value);
  return sonare_engine_set_capture_buffer(engine.get(), &buffer);
}

SonareError probe_engine_set_capture_punch(const std::string& param, double value) {
  PreparedEngine engine;
  const std::int64_t start = is_param(param, "start_sample") ? as_i64(value) : 0;
  return sonare_engine_set_capture_punch(engine.get(), start, 1000, 0);
}

template <typename C>
void set_clip_field(C* clip, const std::string& field, double value) {
  if (field == "num_channels") clip->num_channels = static_cast<int>(value);
  if (field == "num_samples") clip->num_samples = as_i64(value);
  if (field == "length_samples") clip->length_samples = as_i64(value);
  if (field == "fade_in_samples") clip->fade_in_samples = as_i64(value);
  if (field == "fade_out_samples") clip->fade_out_samples = as_i64(value);
}

SonareEngineClip make_clip(const std::string& param, double value) {
  static const std::array<float, 4> kSource{};
  static const float* const kPlanes[1] = {kSource.data()};
  SonareEngineClip clip{};
  sonare_engine_clip_init(&clip);
  clip.id = 1;
  clip.track_id = 1;
  clip.channels = kPlanes;
  clip.num_channels = 1;
  clip.num_samples = 4;
  set_clip_field(&clip, field_of(param), value);
  return clip;
}

SonareError probe_engine_set_clips(const std::string& param, double value) {
  PreparedEngine engine;
  const SonareEngineClip clip = make_clip(param, value);
  return sonare_engine_set_clips(engine.get(), &clip, 1);
}

SonareError probe_engine_upsert_clip(const std::string& param, double value) {
  PreparedEngine engine;
  const SonareEngineClip clip = make_clip(param, value);
  return sonare_engine_upsert_clip(engine.get(), &clip);
}

SonareError probe_engine_set_warp_voice_capacity(const std::string&, double value) {
  PreparedEngine engine;
  return sonare_engine_set_warp_voice_capacity(engine.get(), as_u32(value));
}

SonareError probe_engine_set_clip_page_prefetch_frames(const std::string&, double value) {
  PreparedEngine engine;
  return sonare_engine_set_clip_page_prefetch_frames(engine.get(), as_i64(value));
}

SonareError probe_engine_load_soundfont(const std::string&, double value) {
  PreparedEngine engine;
  const std::array<std::uint8_t, 4> data{};
  return sonare_engine_load_soundfont(engine.get(), data.data(), as_size(value));
}

SonareError probe_engine_push_midi_sysex(const std::string&, double value) {
  PreparedEngine engine;
  const std::array<std::uint8_t, 1> data{0x7E};
  return sonare_engine_push_midi_sysex(engine.get(), 1, data.data(), as_size(value), 0);
}

SonareError probe_engine_set_articulation(const std::string&, double value) {
  PreparedEngine engine;
  return sonare_engine_set_articulation(engine.get(), 1, as_u8(value), 0);
}

SonareError probe_engine_articulation(const std::string&, double value) {
  PreparedEngine engine;
  int articulation = 0;
  return sonare_engine_articulation(engine.get(), 1, as_u8(value), &articulation);
}

SonareError probe_engine_bind_midi_cc(const std::string& param, double value) {
  PreparedEngine engine;
  std::uint8_t channel = 0;
  std::uint8_t controller = 1;
  std::uint32_t param_id = 1;
  if (is_param(param, "channel")) channel = as_u8(value);
  if (is_param(param, "controller")) controller = as_u8(value);
  if (is_param(param, "param_id")) param_id = as_u32(value);
  return sonare_engine_bind_midi_cc(engine.get(), channel, controller, param_id, 0.0f, 1.0f);
}

SonareError probe_engine_bind_midi_cc_binding(const std::string& param, double value) {
  PreparedEngine engine;
  SonareMidiCcBinding binding{};
  binding.cc_number = 1;
  binding.channel = 0;
  binding.kind = SONARE_MIDI_CC_CONTROL_CHANGE_7;
  binding.param_id = 1;
  binding.min_value = 0.0f;
  binding.max_value = 1.0f;
  if (is_param(param, "binding.cc_number")) binding.cc_number = as_u8(value);
  if (is_param(param, "binding.param_id")) binding.param_id = as_u32(value);
  return sonare_engine_bind_midi_cc_binding(engine.get(), &binding);
}

SonareError probe_engine_render_offline_ex(const std::string& param, double value) {
  PreparedEngine engine;
  int num_channels = 1;
  std::int64_t total_frames = 16;
  int block_size = 0;
  if (is_param(param, "num_channels")) num_channels = static_cast<int>(value);
  if (is_param(param, "total_frames")) total_frames = as_i64(value);
  if (is_param(param, "block_size")) block_size = static_cast<int>(value);
  std::array<float, 64> plane{};
  float* out[1] = {plane.data()};
  return sonare_engine_render_offline_ex(engine.get(), out, num_channels, total_frames, block_size,
                                         1);
}

SonareError probe_engine_render_offline(const std::string& param, double value) {
  PreparedEngine engine;
  int num_channels = 1;
  std::int64_t total_frames = 16;
  int block_size = 0;
  if (is_param(param, "num_channels")) num_channels = static_cast<int>(value);
  if (is_param(param, "total_frames")) total_frames = as_i64(value);
  if (is_param(param, "block_size")) block_size = static_cast<int>(value);
  std::array<float, 64> plane{};
  float* out[1] = {plane.data()};
  return sonare_engine_render_offline(engine.get(), out, num_channels, total_frames, block_size);
}

SonareError probe_engine_bounce_offline(const std::string& param, double value) {
  PreparedEngine engine;
  SonareEngineBounceOptions options{};
  sonare_engine_bounce_options_init(&options);
  options.total_frames = 1;
  options.num_channels = 1;
  if (is_param(param, "options.total_frames")) options.total_frames = as_i64(value);
  if (is_param(param, "options.block_size")) options.block_size = static_cast<int>(value);
  if (is_param(param, "options.num_channels")) options.num_channels = static_cast<int>(value);
  if (is_param(param, "options.target_sample_rate")) {
    options.target_sample_rate = static_cast<int>(value);
  }
  if (is_param(param, "options.source_sample_rate")) {
    options.source_sample_rate = static_cast<int>(value);
  }
  if (is_param(param, "options.dither_bits")) {
    options.dither = 1;
    options.dither_bits = static_cast<int>(value);
  }
  SonareEngineBounceResult out{};
  const SonareError err = sonare_engine_bounce_offline(engine.get(), &options, &out);
  if (err == SONARE_OK) sonare_free_engine_bounce_result(&out);
  return err;
}

SonareError probe_engine_freeze_offline(const std::string& param, double value) {
  PreparedEngine engine;
  SonareEngineFreezeOptions options{};
  sonare_engine_freeze_options_init(&options);
  options.total_frames = 16;
  options.num_channels = 1;
  options.clip_id = 1;
  if (is_param(param, "options.total_frames")) options.total_frames = as_i64(value);
  if (is_param(param, "options.block_size")) options.block_size = static_cast<int>(value);
  if (is_param(param, "options.num_channels")) options.num_channels = static_cast<int>(value);
  SonareEngineFreezeResult out{};
  return sonare_engine_freeze_offline(engine.get(), &options, &out);
}

SonareError probe_engine_set_track_strip_channel_delay_samples(const std::string&, double value) {
  PreparedEngine engine;
  return sonare_engine_set_track_strip_channel_delay_samples(engine.get(), 1,
                                                             static_cast<int>(value));
}

SonareError probe_engine_prepare(const std::string& param, double value) {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  int max_block_size = 64;
  std::size_t command_capacity = 32;
  std::size_t telemetry_capacity = 16;
  if (is_param(param, "max_block_size")) max_block_size = static_cast<int>(value);
  if (is_param(param, "command_capacity")) command_capacity = as_size(value);
  if (is_param(param, "telemetry_capacity")) telemetry_capacity = as_size(value);
  const SonareError err =
      sonare_engine_prepare(engine, 48000.0, max_block_size, command_capacity, telemetry_capacity);
  sonare_engine_destroy(engine);
  return err;
}

SonareError probe_engine_prepare_with_channels(const std::string& param, double value) {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  int max_block_size = 64;
  std::size_t command_capacity = 32;
  std::size_t telemetry_capacity = 16;
  int max_channels = 2;
  if (is_param(param, "max_block_size")) max_block_size = static_cast<int>(value);
  if (is_param(param, "command_capacity")) command_capacity = as_size(value);
  if (is_param(param, "telemetry_capacity")) telemetry_capacity = as_size(value);
  if (is_param(param, "max_channels")) max_channels = static_cast<int>(value);
  const SonareError err = sonare_engine_prepare_with_channels(
      engine, 48000.0, max_block_size, command_capacity, telemetry_capacity, max_channels);
  sonare_engine_destroy(engine);
  return err;
}

SonareError probe_engine_seek_sample(const std::string&, double value) {
  PreparedEngine engine;
  return sonare_engine_seek_sample(engine.get(), as_i64(value), 0);
}

SonareError probe_engine_count_in_end_sample(const std::string&, double value) {
  PreparedEngine engine;
  std::int64_t end = 0;
  return sonare_engine_count_in_end_sample(engine.get(), as_i64(value), 1, &end);
}

SonareError probe_engine_prime_offline_parameters(const std::string& param, double value) {
  PreparedEngine engine;
  int num_channels = 1;
  int block_size = 0;
  if (is_param(param, "num_channels")) num_channels = static_cast<int>(value);
  if (is_param(param, "block_size")) block_size = static_cast<int>(value);
  return sonare_engine_prime_offline_parameters(engine.get(), num_channels, block_size);
}

SonareError probe_engine_set_time_signature(const std::string& param, double value) {
  PreparedEngine engine;
  int numerator = 4;
  int denominator = 4;
  if (is_param(param, "numerator")) numerator = static_cast<int>(value);
  if (is_param(param, "denominator")) denominator = static_cast<int>(value);
  return sonare_engine_set_time_signature(engine.get(), numerator, denominator);
}

SonareError probe_engine_set_tempo_segments(const std::string& param, double value) {
  PreparedEngine engine;
  const SonareProjectTempoSegment segment{0.0, 120.0, 0.0, 0.0};
  const std::size_t count = is_param(param, "segment_count") ? as_size(value) : 1;
  return sonare_engine_set_tempo_segments(engine.get(), &segment, count);
}

SonareError probe_engine_set_time_signature_segments(const std::string& param, double value) {
  PreparedEngine engine;
  SonareProjectTimeSignatureSegment segment{0.0, 4, 4};
  std::size_t count = 1;
  if (is_param(param, "segment_count")) count = as_size(value);
  if (is_param(param, "segments[].numerator")) segment.numerator = static_cast<int>(value);
  if (is_param(param, "segments[].denominator")) segment.denominator = static_cast<int>(value);
  return sonare_engine_set_time_signature_segments(engine.get(), &segment, count);
}

SonareError probe_engine_add_parameter(const std::string&, double value) {
  PreparedEngine engine;
  SonareParameterInfo info{};
  info.id = as_u32(value);
  std::strcpy(info.name, "p");
  info.min_value = 0.0f;
  info.max_value = 1.0f;
  info.default_value = 0.0f;
  info.rt_safe = 1;
  return sonare_engine_add_parameter(engine.get(), &info);
}

SonareError probe_engine_set_markers(const std::string&, double value) {
  PreparedEngine engine;
  SonareEngineMarker marker{};
  marker.id = as_u32(value);
  std::strcpy(marker.name, "m");
  return sonare_engine_set_markers(engine.get(), &marker, 1);
}

#if defined(SONARE_WITH_GRAPH)

SonareError probe_engine_set_graph(const std::string& param, double value) {
  PreparedEngine engine;
  SonareEngineGraphNode node{};
  sonare_engine_graph_node_init(&node);
  std::strcpy(node.id, "a");
  SonareEngineGraphParameterBinding binding{};
  binding.param_id = 1;
  std::strcpy(binding.node_id, "a");
  SonareEngineGraphSpec spec{};
  spec.nodes = &node;
  spec.node_count = 1;
  spec.num_channels = 1;
  std::strcpy(spec.input_node, "a");
  std::strcpy(spec.output_node, "a");
  if (is_param(param, "spec.node_count")) spec.node_count = as_size(value);
  if (is_param(param, "spec.num_channels")) spec.num_channels = static_cast<int>(value);
  if (is_param(param, "spec.nodes[].num_ports")) node.num_ports = static_cast<int>(value);
  if (is_param(param, "spec.parameter_bindings[].param_id")) {
    binding.param_id = as_u32(value);
    spec.parameter_bindings = &binding;
    spec.parameter_binding_count = 1;
  }
  return sonare_engine_set_graph(engine.get(), &spec);
}

#endif  // SONARE_WITH_GRAPH

#if defined(SONARE_WITH_ARRANGEMENT)

SonareError probe_engine_bind_controller(const std::string&, double value) {
  PreparedEngine engine;
  SonareControllerBinding binding{};
  binding.input = SONARE_CONTROLLER_INPUT_CONTROL_CHANGE;
  binding.index = as_u8(value);
  binding.axis = SONARE_CONTROLLER_AXIS_EXCITATION;
  binding.lo = 0.0f;
  binding.hi = 1.0f;
  binding.curve = 1.0f;
  return sonare_engine_bind_controller(engine.get(), 5, &binding);
}

SonareError probe_engine_set_midi_clips(const std::string& param, double value) {
  PreparedEngine engine;
  SonareEngineMidiEvent event{};
  event.word0 = 0x20903C40u;
  event.word_count = 1;
  SonareEngineMidiClipSchedule clip{};
  sonare_engine_midi_clip_schedule_init(&clip);
  clip.id = 1;
  clip.events = &event;
  clip.event_count = 1;
  const std::string field = field_of(param);
  if (field == "group") event.group = as_u8(value);
  if (field == "length_samples") clip.length_samples = as_i64(value);
  if (field == "loop") clip.loop = static_cast<int>(value);
  if (field == "loop_length_samples") {
    clip.loop = 1;
    clip.loop_length_samples = as_i64(value);
  }
  if (field == "fade_in_samples") clip.fade_in_samples = as_i64(value);
  if (field == "fade_out_samples") clip.fade_out_samples = as_i64(value);
  return sonare_engine_set_midi_clips(engine.get(), &clip, 1);
}

#endif  // SONARE_WITH_ARRANGEMENT

#if defined(SONARE_WITH_MIXING)

SonareError probe_engine_set_track_lanes(const std::string& param, double value) {
  PreparedEngine engine;
  SonareEngineTrackLane lane{};
  lane.source_channel_layout = SONARE_CHANNEL_LAYOUT_STEREO;
  if (is_param(param, "lanes[].track_id")) {
    lane.track_id = as_u32(value);
    return sonare_engine_set_track_lanes(engine.get(), &lane, 1);
  }
  return sonare_engine_set_track_lanes(engine.get(), &lane, as_size(value));
}

SonareError probe_engine_set_track_buses(const std::string&, double value) {
  PreparedEngine engine;
  SonareEngineBus bus{};
  bus.bus_id = as_u32(value);
  return sonare_engine_set_track_buses(engine.get(), &bus, 1);
}

SonareError probe_engine_set_track_strip_eq_band_json(const std::string&, double value) {
  PreparedEngine engine;
  return sonare_engine_set_track_strip_eq_band_json(engine.get(), 1, static_cast<int>(value), "{}");
}

SonareError probe_engine_set_master_strip_eq_band_json(const std::string&, double value) {
  PreparedEngine engine;
  return sonare_engine_set_master_strip_eq_band_json(engine.get(), static_cast<int>(value), "{}");
}

SonareError probe_engine_set_bus_strip_eq_band_json(const std::string&, double value) {
  PreparedEngine engine;
  return sonare_engine_set_bus_strip_eq_band_json(engine.get(), 1, static_cast<int>(value), "{}");
}

SonareError probe_strip_set_channel_delay_samples(const std::string&, double value) {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 512, &mixer) == SONARE_OK);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "s");
  REQUIRE(strip != nullptr);
  const SonareError err = sonare_strip_set_channel_delay_samples(strip, static_cast<int>(value));
  sonare_mixer_destroy(mixer);
  return err;
}

#endif  // SONARE_WITH_MIXING

// ---------------------------------------------------------------------------------------------
// DAW
// ---------------------------------------------------------------------------------------------

void set_percussive_framing(const std::string& param, double value, int32_t* n_fft,
                            int32_t* kernel_harmonic, int32_t* kernel_percussive) {
  if (is_param(param, "config.n_fft")) *n_fft = static_cast<int32_t>(value);
  if (is_param(param, "config.hpss_kernel_harmonic")) {
    *kernel_harmonic = static_cast<int32_t>(value);
  }
  if (is_param(param, "config.hpss_kernel_percussive")) {
    *kernel_percussive = static_cast<int32_t>(value);
  }
}

SonareError probe_extract_percussive_events(const std::string& param, double value) {
  SonarePercussiveEventConfig config{};
  sonare_percussive_event_config_init(&config);
  set_percussive_framing(param, value, &config.n_fft, &config.hpss_kernel_harmonic,
                         &config.hpss_kernel_percussive);
  SonarePercussiveEventsResult out{};
  const SonareError err = sonare_extract_percussive_events(
      analysis_samples().data(), analysis_samples().size(), kAnalysisRate, &config, &out);
  if (err == SONARE_OK) sonare_free_percussive_events_result(&out);
  return err;
}

SonareError probe_render_percussive_events(const std::string& param, double value) {
  SonarePercussiveRenderConfig config{};
  sonare_percussive_render_config_init(&config);
  set_percussive_framing(param, value, &config.n_fft, &config.hpss_kernel_harmonic,
                         &config.hpss_kernel_percussive);
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_render_percussive_events(analysis_samples().data(), analysis_samples().size(),
                                      kAnalysisRate, nullptr, 0, &config, &out, &out_length);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

#if defined(SONARE_WITH_ARRANGEMENT)

SonareError probe_sample_bank_add_sample(const std::string& param, double value) {
  SonareSampleBank* bank = nullptr;
  REQUIRE(sonare_sample_bank_create(&bank) == SONARE_OK);
  const std::array<float, 8> data{};
  SonareSampleDesc desc{};
  size_t n_frames = 8;
  if (is_param(param, "n_frames")) n_frames = as_size(value);
  if (is_param(param, "desc.loop_mode")) desc.loop_mode = static_cast<int>(value);
  uint32_t index = 0;
  const SonareError err = sonare_sample_bank_add_sample(bank, data.data(), n_frames, &desc, &index);
  sonare_sample_bank_destroy(bank);
  return err;
}

SonareError probe_sample_bank_add_zone(const std::string&, double value) {
  SonareSampleBank* bank = nullptr;
  REQUIRE(sonare_sample_bank_create(&bank) == SONARE_OK);
  const SonareSampleZoneDesc zone{};
  const SonareError err = sonare_sample_bank_add_zone(bank, as_u32(value), &zone);
  sonare_sample_bank_destroy(bank);
  return err;
}

#endif  // SONARE_WITH_ARRANGEMENT

#if defined(SONARE_WITH_PITCH_EDITOR)

constexpr size_t kTrackFrames = 8;

const std::vector<float>& track_f0() {
  static const std::vector<float> f0(kTrackFrames, 220.0f);
  return f0;
}

const std::vector<int32_t>& track_voiced() {
  static const std::vector<int32_t> voiced(kTrackFrames, 1);
  return voiced;
}

SonareError probe_pitch_correct_to_midi_timevarying(const std::string& param, double value) {
  const bool hop = is_param(param, "hop_length");
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err = sonare_pitch_correct_to_midi_timevarying(
      analysis_samples().data(), analysis_samples().size(), kAnalysisRate, track_f0().data(),
      nullptr, track_voiced().data(), hop ? track_f0().size() : as_size(value),
      hop ? static_cast<int>(value) : 512, 60.0f, &out, &out_length);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

SonareError probe_pitch_correct_timevarying(const std::string& param, double value) {
  SonarePitchCorrectionConfig config{};
  sonare_pitch_correction_config_init(&config);
  size_t n_frames = track_f0().size();
  int hop_length = 512;
  if (is_param(param, "n_frames")) n_frames = as_size(value);
  if (is_param(param, "hop_length")) hop_length = static_cast<int>(value);
  if (is_param(param, "config.scale_root")) config.scale_root = static_cast<int>(value);
  if (is_param(param, "config.scale_mode_mask")) config.scale_mode_mask = as_u32(value);
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err = sonare_pitch_correct_timevarying(
      analysis_samples().data(), analysis_samples().size(), kAnalysisRate, track_f0().data(),
      nullptr, track_voiced().data(), n_frames, hop_length, &config, &out, &out_length);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

SonareError probe_extract_notes(const std::string&, double value) {
  SonareNoteObjectsResult out{};
  const SonareError err = sonare_extract_notes(
      analysis_samples().data(), analysis_samples().size(), kAnalysisRate, track_f0().data(),
      nullptr, track_voiced().data(), as_size(value), 100.0f, nullptr, &out);
  if (err == SONARE_OK) sonare_free_note_objects_result(&out);
  return err;
}

SonareNoteObject make_note(int32_t frame_start, int32_t frame_end) {
  SonareNoteObject note{};
  sonare_note_edit_init(&note.edit);
  note.onset_sample = 0;
  note.offset_sample = 100;
  note.frame_start = frame_start;
  note.frame_end = frame_end;
  note.median_hz = 220.0f;
  return note;
}

SonareError probe_render_notes(const std::string& param, double value) {
  static const std::array<float, 4> kEnvelope{1.0f, 1.0f, 1.0f, 1.0f};
  SonareNoteObject note = make_note(0, 2);
  const float* f0 = nullptr;
  size_t n_frames = 0;
  size_t note_count = 0;
  if (is_param(param, "n_frames")) {
    f0 = track_f0().data();
    n_frames = as_size(value);
  } else if (is_param(param, "notes[].frame_start")) {
    f0 = track_f0().data();
    n_frames = kTrackFrames;
    note_count = 1;
    note.frame_start = static_cast<int32_t>(value);
  } else if (is_param(param, "notes[].edit.envelope_offset")) {
    note_count = 1;
    note.edit.envelope_count = 1;
    note.edit.envelope_offset = as_i64(value);
  }
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_render_notes(analysis_samples().data(), analysis_samples().size(), kAnalysisRate,
                          &note, note_count, kEnvelope.data(), kEnvelope.size(), f0, nullptr,
                          n_frames, 100.0f, nullptr, &out, &out_length);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

SonareError probe_decompose_note_pitch(const std::string&, double value) {
  SonarePitchDecompositionResult out{};
  const SonareError err = sonare_decompose_note_pitch(track_f0().data(), track_voiced().data(),
                                                      as_size(value), 100.0f, 220.0f, 0.0f, &out);
  if (err == SONARE_OK) sonare_free_pitch_decomposition_result(&out);
  return err;
}

SonareError probe_split_note(const std::string& param, double value) {
  static const std::array<float, 4> kEnvelope{1.0f, 1.0f, 1.0f, 1.0f};
  SonareNoteObject note = make_note(0, 4);
  size_t n_frames = kTrackFrames;
  if (is_param(param, "n_frames")) n_frames = as_size(value);
  if (is_param(param, "notes[].edit.envelope_offset")) {
    note.edit.envelope_count = 1;
    note.edit.envelope_offset = as_i64(value);
  }
  SonareNoteObjectsResult out{};
  const SonareError err =
      sonare_split_note(analysis_samples().data(), analysis_samples().size(), kAnalysisRate,
                        track_f0().data(), nullptr, track_voiced().data(), n_frames, 100.0f,
                        nullptr, &note, 1, kEnvelope.data(), kEnvelope.size(), 0, 2, &out);
  if (err == SONARE_OK) sonare_free_note_objects_result(&out);
  return err;
}

SonareError probe_merge_notes(const std::string& param, double value) {
  static const std::array<float, 4> kEnvelope{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<SonareNoteObject, 2> notes{make_note(0, 2), make_note(2, 4)};
  size_t n_frames = kTrackFrames;
  if (is_param(param, "n_frames")) n_frames = as_size(value);
  if (is_param(param, "notes[].edit.envelope_offset")) {
    notes[0].edit.envelope_count = 1;
    notes[0].edit.envelope_offset = as_i64(value);
  }
  SonareNoteObjectsResult out{};
  const SonareError err = sonare_merge_notes(
      analysis_samples().data(), analysis_samples().size(), kAnalysisRate, track_f0().data(),
      nullptr, track_voiced().data(), n_frames, 100.0f, nullptr, notes.data(), notes.size(),
      kEnvelope.data(), kEnvelope.size(), 0, 1, &out);
  if (err == SONARE_OK) sonare_free_note_objects_result(&out);
  return err;
}

SonareError probe_assign_note_targets(const std::string&, double value) {
  size_t assigned = 0;
  return sonare_assign_note_targets(nullptr, 0, static_cast<int>(value), nullptr, 0, nullptr,
                                    &assigned);
}

#endif  // SONARE_WITH_PITCH_EDITOR

#if defined(SONARE_WITH_VOICE_CHANGER)

SonareError probe_voice_change_realtime(const std::string&, double value) {
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err = sonare_voice_change_realtime(
      analysis_samples().data(), analysis_samples().size(), kAnalysisRate, nullptr,
      static_cast<int>(value), &out, &out_length);
  if (err == SONARE_OK) sonare_free_floats(out);
  return err;
}

SonareError probe_realtime_voice_changer_preset_config(const std::string&, double value) {
  SonareRealtimeVoiceChangerConfig out{};
  sonare_realtime_voice_changer_config_init(&out);
  return sonare_realtime_voice_changer_preset_config(
      static_cast<SonareVoiceCharacterPreset>(static_cast<int>(value)), &out);
}

#endif  // SONARE_WITH_VOICE_CHANGER

const std::map<std::string, Probe>& probes() {
  static const std::map<std::string, Probe> table = {
      {"biquad_filter", probe_biquad_filter},
      {"clip_page_provider_create", probe_clip_page_provider_create},
      {"decompose", probe_decompose},
      {"decompose_stems", probe_decompose_stems},
      {"decompose_stems_linked", probe_decompose_stems_linked},
      {"decompose_with_init", probe_decompose_with_init},
      {"engine_add_parameter", probe_engine_add_parameter},
      {"engine_articulation", probe_engine_articulation},
      {"engine_bind_midi_cc", probe_engine_bind_midi_cc},
      {"engine_bind_midi_cc_binding", probe_engine_bind_midi_cc_binding},
      {"engine_bounce_offline", probe_engine_bounce_offline},
      {"engine_count_in_end_sample", probe_engine_count_in_end_sample},
      {"engine_freeze_offline", probe_engine_freeze_offline},
      {"engine_load_soundfont", probe_engine_load_soundfont},
      {"engine_prepare", probe_engine_prepare},
      {"engine_prepare_with_channels", probe_engine_prepare_with_channels},
      {"engine_prime_offline_parameters", probe_engine_prime_offline_parameters},
      {"engine_push_midi_sysex", probe_engine_push_midi_sysex},
      {"engine_render_offline", probe_engine_render_offline},
      {"engine_render_offline_ex", probe_engine_render_offline_ex},
      {"engine_seek_sample", probe_engine_seek_sample},
      {"engine_set_articulation", probe_engine_set_articulation},
      {"engine_set_capture_buffer", probe_engine_set_capture_buffer},
      {"engine_set_capture_punch", probe_engine_set_capture_punch},
      {"engine_set_clip_page_prefetch_frames", probe_engine_set_clip_page_prefetch_frames},
      {"engine_set_clips", probe_engine_set_clips},
      {"engine_set_markers", probe_engine_set_markers},
      {"engine_set_tempo_segments", probe_engine_set_tempo_segments},
      {"engine_set_time_signature", probe_engine_set_time_signature},
      {"engine_set_time_signature_segments", probe_engine_set_time_signature_segments},
      {"engine_set_track_strip_channel_delay_samples",
       probe_engine_set_track_strip_channel_delay_samples},
      {"engine_set_warp_voice_capacity", probe_engine_set_warp_voice_capacity},
      {"engine_upsert_clip", probe_engine_upsert_clip},
      {"extract_percussive_events", probe_extract_percussive_events},
      {"hpss_ex", probe_hpss_ex},
      {"nn_filter", probe_nn_filter},
      {"phase_vocoder", probe_phase_vocoder},
      {"pitch_shift_ex", probe_pitch_shift_ex},
      {"render_percussive_events", probe_render_percussive_events},
      {"spectral_edit", probe_spectral_edit},
      {"split_silence", probe_split_silence},
      {"split_silence_common", probe_split_silence_common},
      {"split_silence_common_ex", probe_split_silence_common_ex},
      {"time_stretch_ex", probe_time_stretch_ex},
      {"trim_ex", probe_trim_ex},
      {"trim_silence", probe_trim_silence},
#if defined(SONARE_WITH_GRAPH)
      {"engine_set_graph", probe_engine_set_graph},
#endif
#if defined(SONARE_WITH_ARRANGEMENT)
      {"engine_bind_controller", probe_engine_bind_controller},
      {"engine_set_midi_clips", probe_engine_set_midi_clips},
      {"sample_bank_add_sample", probe_sample_bank_add_sample},
      {"sample_bank_add_zone", probe_sample_bank_add_zone},
#endif
#if defined(SONARE_WITH_MIXING)
      {"engine_set_bus_strip_eq_band_json", probe_engine_set_bus_strip_eq_band_json},
      {"engine_set_master_strip_eq_band_json", probe_engine_set_master_strip_eq_band_json},
      {"engine_set_track_buses", probe_engine_set_track_buses},
      {"engine_set_track_lanes", probe_engine_set_track_lanes},
      {"engine_set_track_strip_eq_band_json", probe_engine_set_track_strip_eq_band_json},
      {"strip_set_channel_delay_samples", probe_strip_set_channel_delay_samples},
#endif
#if defined(SONARE_WITH_PITCH_EDITOR)
      {"assign_note_targets", probe_assign_note_targets},
      {"decompose_note_pitch", probe_decompose_note_pitch},
      {"extract_notes", probe_extract_notes},
      {"merge_notes", probe_merge_notes},
      {"pitch_correct_timevarying", probe_pitch_correct_timevarying},
      {"pitch_correct_to_midi_timevarying", probe_pitch_correct_to_midi_timevarying},
      {"render_notes", probe_render_notes},
      {"split_note", probe_split_note},
#endif
#if defined(SONARE_WITH_VOICE_CHANGER)
      {"realtime_voice_changer_preset_config", probe_realtime_voice_changer_preset_config},
      {"voice_change_realtime", probe_voice_change_realtime},
#endif
  };
  return table;
}

/// True when this build compiles the row's refusal: its entry has a probe, and the dither word
/// length is read only where mastering is.
bool row_is_built(const NumericDomainRow& row) {
  if (probes().count(row.entry) == 0) return false;
#if !defined(SONARE_WITH_MASTERING)
  if (std::string(row.param) == "options.dither_bits") return false;
#endif
  return true;
}

std::vector<NumericDomainRow> built_rows() {
  std::vector<NumericDomainRow> rows;
  for (const NumericDomainRow& row : kEffectsDomainRows) {
    if (row_is_built(row)) rows.push_back(row);
  }
  return rows;
}

NumericDomainTable built_table() {
  static const std::vector<NumericDomainRow> rows = built_rows();
  return {"effects", rows.data(), rows.size()};
}

/// The domain's lowest value, except where the entry refuses that value for another reason under
/// the same subject: an FFT size must be even, and a source rate must be the prepared one.
double inside_value(const NumericDomainRow& row) {
  const std::string param = row.param;
  if (ends_with(param, "n_fft")) return 2048.0;
  if (ends_with(param, "sample_rate")) return 48000.0;
  return lowest_inside_value(row);
}

}  // namespace

TEST_CASE("the effects numeric-domain rows are sorted", "[numeric_domains][c_api]") {
  require_sorted(numeric_domain_table("effects", kEffectsDomainRows));
}

TEST_CASE("an effects value outside its row's domain is refused under the row's subject",
          "[numeric_domains][c_api]") {
  require_outside_refused(built_table(), probes());
}

TEST_CASE("an effects value inside its row's domain is not refused under the row's subject",
          "[numeric_domains][c_api]") {
  require_inside_accepted(built_table(), probes(), inside_value);
}
