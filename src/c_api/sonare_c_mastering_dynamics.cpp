#include <sonare/sonare_c.h>

#include <array>
#include <cstring>
#include <vector>

#include "mastering/api/internal_processor_runner.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/transient_shaper.h"
#include "sonare_c_internal.h"
#include "util/exception.h"

using namespace sonare;
using namespace sonare_c_detail;

namespace {

constexpr std::array<CName, 7> kGateNames{{
    {"threshold_db", "config.threshold_db"},
    {"attack_ms", "config.attack_ms"},
    {"release_ms", "config.release_ms"},
    {"hold_ms", "config.hold_ms"},
    {"close_threshold_db", "config.close_threshold_db"},
    {"key_high_pass_hz", "config.key_high_pass_hz"},
    {"range_db", "config.range_db"},
}};

sonare::mastering::dynamics::DetectorMode to_cpp_detector_mode(int mode) {
  switch (mode) {
    case SONARE_COMPRESSOR_DETECTOR_PEAK:
      return sonare::mastering::dynamics::DetectorMode::Peak;
    case SONARE_COMPRESSOR_DETECTOR_RMS:
      return sonare::mastering::dynamics::DetectorMode::Rms;
    case SONARE_COMPRESSOR_DETECTOR_LOG_RMS:
      return sonare::mastering::dynamics::DetectorMode::LogRms;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown compressor detector mode");
}

int to_c_detector_mode(sonare::mastering::dynamics::DetectorMode mode) {
  switch (mode) {
    case sonare::mastering::dynamics::DetectorMode::Peak:
      return SONARE_COMPRESSOR_DETECTOR_PEAK;
    case sonare::mastering::dynamics::DetectorMode::Rms:
      return SONARE_COMPRESSOR_DETECTOR_RMS;
    case sonare::mastering::dynamics::DetectorMode::LogRms:
      return SONARE_COMPRESSOR_DETECTOR_LOG_RMS;
  }
  return SONARE_COMPRESSOR_DETECTOR_RMS;
}

sonare::mastering::dynamics::CompressorConfig to_cpp_compressor_config(
    const SonareCompressorConfig* config) {
  sonare::mastering::dynamics::CompressorConfig cpp;
  if (!config) return cpp;
  require_struct_size(config->struct_size, sizeof(SonareCompressorConfig), "config",
                      "sonare_compressor_config_init");
  cpp.threshold_db = config->threshold_db;
  cpp.ratio = config->ratio;
  cpp.attack_ms = config->attack_ms;
  cpp.release_ms = config->release_ms;
  cpp.knee_db = config->knee_db;
  cpp.makeup_gain_db = config->makeup_gain_db;
  cpp.auto_makeup = config->auto_makeup != 0;
  cpp.detector = to_cpp_detector_mode(config->detector);
  cpp.sidechain_high_pass_hz = config->sidechain_high_pass_hz;
  cpp.pdr_time_ms = config->pdr_time_ms;
  cpp.pdr_release_scale = config->pdr_release_scale;
  return cpp;
}

sonare::mastering::dynamics::GateConfig to_cpp_gate_config(const SonareGateConfig* config) {
  sonare::mastering::dynamics::GateConfig cpp;
  if (!config) return cpp;
  require_struct_size(config->struct_size, sizeof(SonareGateConfig), "config",
                      "sonare_gate_config_init");
  cpp.threshold_db = config->threshold_db;
  cpp.attack_ms = config->attack_ms;
  cpp.release_ms = config->release_ms;
  cpp.range_db = config->range_db;
  cpp.hold_ms = config->hold_ms;
  cpp.close_threshold_db = config->close_threshold_db;
  cpp.key_high_pass_hz = config->key_high_pass_hz;
  return cpp;
}

sonare::mastering::dynamics::TransientShaperConfig to_cpp_transient_shaper_config(
    const SonareTransientShaperConfig* config) {
  sonare::mastering::dynamics::TransientShaperConfig cpp;
  if (!config) return cpp;
  require_struct_size(config->struct_size, sizeof(SonareTransientShaperConfig), "config",
                      "sonare_transient_shaper_config_init");
  cpp.attack_gain_db = config->attack_gain_db;
  cpp.sustain_gain_db = config->sustain_gain_db;
  cpp.fast_attack_ms = config->fast_attack_ms;
  cpp.fast_release_ms = config->fast_release_ms;
  cpp.slow_attack_ms = config->slow_attack_ms;
  cpp.slow_release_ms = config->slow_release_ms;
  cpp.sensitivity = config->sensitivity;
  cpp.max_gain_db = config->max_gain_db;
  cpp.gain_smoothing_ms = config->gain_smoothing_ms;
  cpp.lookahead_ms = config->lookahead_ms;
  return cpp;
}

template <typename Processor>
void run_processor_offline(Processor& processor, std::vector<float>& samples, int sample_rate,
                           int* out_latency_samples) {
  // Latency-compensate via the shared mono runner so this direct C-ABI path
  // returns time-aligned output, matching sonare_mastering_apply_processor()
  // (internal::run_processor_mono). Without this, a non-zero lookahead (e.g.
  // the transient shaper's lookahead_ms) would leave the output time-shifted.
  sonare::mastering::api::internal::run_processor_mono(processor, samples, sample_rate);
  if (out_latency_samples) *out_latency_samples = processor.latency_samples();
}

}  // namespace

void sonare_compressor_config_init(SonareCompressorConfig* config) {
  if (config == nullptr) return;
  const sonare::mastering::dynamics::CompressorConfig cpp;
  *config = {};
  config->struct_size = sizeof(*config);
  config->threshold_db = cpp.threshold_db;
  config->ratio = cpp.ratio;
  config->attack_ms = cpp.attack_ms;
  config->release_ms = cpp.release_ms;
  config->knee_db = cpp.knee_db;
  config->makeup_gain_db = cpp.makeup_gain_db;
  config->auto_makeup = cpp.auto_makeup ? 1 : 0;
  config->detector = to_c_detector_mode(cpp.detector);
  config->sidechain_high_pass_hz = cpp.sidechain_high_pass_hz;
  config->pdr_time_ms = cpp.pdr_time_ms;
  config->pdr_release_scale = cpp.pdr_release_scale;
}

void sonare_gate_config_init(SonareGateConfig* config) {
  if (config == nullptr) return;
  const sonare::mastering::dynamics::GateConfig cpp;
  *config = {};
  config->struct_size = sizeof(*config);
  config->threshold_db = cpp.threshold_db;
  config->attack_ms = cpp.attack_ms;
  config->release_ms = cpp.release_ms;
  config->range_db = cpp.range_db;
  config->hold_ms = cpp.hold_ms;
  config->close_threshold_db = cpp.close_threshold_db;
  config->key_high_pass_hz = cpp.key_high_pass_hz;
}

void sonare_transient_shaper_config_init(SonareTransientShaperConfig* config) {
  if (config == nullptr) return;
  const sonare::mastering::dynamics::TransientShaperConfig cpp;
  *config = {};
  config->struct_size = sizeof(*config);
  config->attack_gain_db = cpp.attack_gain_db;
  config->sustain_gain_db = cpp.sustain_gain_db;
  config->fast_attack_ms = cpp.fast_attack_ms;
  config->fast_release_ms = cpp.fast_release_ms;
  config->slow_attack_ms = cpp.slow_attack_ms;
  config->slow_release_ms = cpp.slow_release_ms;
  config->sensitivity = cpp.sensitivity;
  config->max_gain_db = cpp.max_gain_db;
  config->gain_smoothing_ms = cpp.gain_smoothing_ms;
  config->lookahead_ms = cpp.lookahead_ms;
}

SonareError sonare_mastering_dynamics_compressor(const float* samples, size_t length,
                                                 int sample_rate,
                                                 const SonareCompressorConfig* config, float** out,
                                                 size_t* out_length, int* out_latency_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_latency_samples);
  SonareError err = validate_audio_params("samples", samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  SONARE_C_TRY
  std::vector<float> buffer(samples, samples + length);
  sonare::mastering::dynamics::Compressor processor(to_cpp_compressor_config(config));
  run_processor_offline(processor, buffer, sample_rate, out_latency_samples);
  return copy_vector(buffer, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_mastering_dynamics_gate(const float* samples, size_t length, int sample_rate,
                                           const SonareGateConfig* config, float** out,
                                           size_t* out_length, int* out_latency_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_latency_samples);
  SonareError err = validate_audio_params("samples", samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  SONARE_C_TRY
  std::vector<float> buffer(samples, samples + length);
  with_c_names(kGateNames, [&] {
    sonare::mastering::dynamics::Gate processor(to_cpp_gate_config(config));
    run_processor_offline(processor, buffer, sample_rate, out_latency_samples);
  });
  return copy_vector(buffer, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_mastering_dynamics_transient_shaper(const float* samples, size_t length,
                                                       int sample_rate,
                                                       const SonareTransientShaperConfig* config,
                                                       float** out, size_t* out_length,
                                                       int* out_latency_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_latency_samples);
  SonareError err = validate_audio_params("samples", samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  SONARE_C_TRY
  std::vector<float> buffer(samples, samples + length);
  sonare::mastering::dynamics::TransientShaper processor(to_cpp_transient_shaper_config(config));
  run_processor_offline(processor, buffer, sample_rate, out_latency_samples);
  return copy_vector(buffer, out, out_length);
  SONARE_C_CATCH
}
