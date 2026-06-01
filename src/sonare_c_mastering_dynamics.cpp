#include <cstring>
#include <vector>

#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/transient_shaper.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"
#include "util/exception.h"

using namespace sonare;
using namespace sonare_c_detail;

namespace {

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

sonare::mastering::dynamics::CompressorConfig to_cpp_compressor_config(
    const SonareCompressorConfig* config) {
  sonare::mastering::dynamics::CompressorConfig cpp;
  if (!config) return cpp;
  cpp.threshold_db = config->threshold_db;
  cpp.ratio = config->ratio;
  cpp.attack_ms = config->attack_ms;
  cpp.release_ms = config->release_ms;
  cpp.knee_db = config->knee_db;
  cpp.makeup_gain_db = config->makeup_gain_db;
  cpp.auto_makeup = config->auto_makeup != 0;
  cpp.detector = to_cpp_detector_mode(config->detector);
  cpp.sidechain_hpf_enabled = config->sidechain_hpf_enabled != 0;
  cpp.sidechain_hpf_hz = config->sidechain_hpf_hz;
  cpp.pdr_time_ms = config->pdr_time_ms;
  cpp.pdr_release_scale = config->pdr_release_scale;
  return cpp;
}

sonare::mastering::dynamics::GateConfig to_cpp_gate_config(const SonareGateConfig* config) {
  sonare::mastering::dynamics::GateConfig cpp;
  if (!config) return cpp;
  cpp.threshold_db = config->threshold_db;
  cpp.attack_ms = config->attack_ms;
  cpp.release_ms = config->release_ms;
  cpp.range_db = config->range_db;
  cpp.hold_ms = config->hold_ms;
  cpp.close_threshold_db = config->close_threshold_db;
  cpp.key_hpf_hz = config->key_hpf_hz;
  return cpp;
}

sonare::mastering::dynamics::TransientShaperConfig to_cpp_transient_shaper_config(
    const SonareTransientShaperConfig* config) {
  sonare::mastering::dynamics::TransientShaperConfig cpp;
  if (!config) return cpp;
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

void clear_float_output(float** out, size_t* out_length) {
  *out = nullptr;
  *out_length = 0;
}

template <typename Processor>
void run_offline(Processor& processor, std::vector<float>& samples, int sample_rate,
                 int* out_latency_samples) {
  processor.prepare(sample_rate, static_cast<int>(samples.size()));
  float* channels[] = {samples.data()};
  processor.process(channels, 1, static_cast<int>(samples.size()));
  if (out_latency_samples) *out_latency_samples = processor.latency_samples();
}

}  // namespace

SonareError sonare_mastering_dynamics_compressor(const float* samples, size_t length,
                                                 int sample_rate,
                                                 const SonareCompressorConfig* config, float** out,
                                                 size_t* out_length, int* out_latency_samples) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);
  if (out_latency_samples) *out_latency_samples = 0;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  std::vector<float> buffer(samples, samples + length);
  sonare::mastering::dynamics::Compressor processor(to_cpp_compressor_config(config));
  run_offline(processor, buffer, sample_rate, out_latency_samples);
  *out_length = buffer.size();
  *out = new float[buffer.size()];
  std::memcpy(*out, buffer.data(), buffer.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_dynamics_gate(const float* samples, size_t length, int sample_rate,
                                           const SonareGateConfig* config, float** out,
                                           size_t* out_length, int* out_latency_samples) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);
  if (out_latency_samples) *out_latency_samples = 0;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  std::vector<float> buffer(samples, samples + length);
  sonare::mastering::dynamics::Gate processor(to_cpp_gate_config(config));
  run_offline(processor, buffer, sample_rate, out_latency_samples);
  *out_length = buffer.size();
  *out = new float[buffer.size()];
  std::memcpy(*out, buffer.data(), buffer.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_dynamics_transient_shaper(const float* samples, size_t length,
                                                       int sample_rate,
                                                       const SonareTransientShaperConfig* config,
                                                       float** out, size_t* out_length,
                                                       int* out_latency_samples) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  clear_float_output(out, out_length);
  if (out_latency_samples) *out_latency_samples = 0;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  std::vector<float> buffer(samples, samples + length);
  sonare::mastering::dynamics::TransientShaper processor(to_cpp_transient_shaper_config(config));
  run_offline(processor, buffer, sample_rate, out_latency_samples);
  *out_length = buffer.size();
  *out = new float[buffer.size()];
  std::memcpy(*out, buffer.data(), buffer.size() * sizeof(float));
  return SONARE_OK;
  SONARE_C_CATCH
}
