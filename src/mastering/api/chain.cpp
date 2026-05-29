/// @file chain.cpp
/// @brief Implementation of the high-level mastering chain composition.

#include "mastering/api/chain.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "mastering/api/audio_utils.h"
#include "mastering/common/processor_base.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/eq/tilt.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/tape.h"
#include "mastering/spectral/air_band.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"
// TODO(layer-violation): CLAUDE.md restricts `mastering/` (non-assistant) to
// `core/ + util/ + rt/`. The `MasteringChain` reports `input_lufs`,
// `output_lufs`, `output_true_peak_dbtp`, and `output_lra` on every result
// (consumed by the C API and WASM bindings), and the loudness target stage
// internally needs `metering::lufs()` to compute the per-pass gain. Possible
// future fixes:
//   1. Move the loudness-target stage of the chain into `mastering/assistant/`
//      (or into a dedicated `editing/loudness_target` module) and have the
//      core chain only return raw audio + per-stage gain reductions.
//   2. Drop the loudness reporting fields from `MonoChainResult` /
//      `StereoChainResult` and have callers re-measure with `metering/`.
//   3. Accept pre-measured input loudness as a parameter and have the C/WASM
//      bridge perform the post-chain LUFS / dBTP / LRA measurement.
#include "metering/lufs.h"
#include "metering/true_peak.h"

namespace sonare::mastering::api {
namespace {

// ---------------------------------------------------------------------------
// Shared per-processor helpers (mirror src/wasm/bindings.cpp lines 137-182).
// ---------------------------------------------------------------------------

void run_processor_mono(common::ProcessorBase& processor, std::vector<float>& samples,
                        int sample_rate) {
  if (samples.empty()) {
    return;
  }
  const int n = static_cast<int>(samples.size());
  // Query latency: prepare once at N, read latency (valid post-prepare for our
  // processors).
  processor.prepare(sample_rate, n);
  const int latency = processor.latency_samples();
  if (latency <= 0) {
    float* channels[] = {samples.data()};
    processor.process(channels, 1, n);
    return;
  }
  // Re-prepare for the padded length (prepare() reinitializes processor state,
  // so a separate reset() before it would be redundant), then process N signal +
  // `latency` zeros and drop the leading `latency` output samples so the result
  // is time-aligned and the delayed tail is flushed out.
  processor.prepare(sample_rate, n + latency);
  std::vector<float> padded(samples.begin(), samples.end());
  padded.resize(static_cast<std::size_t>(n) + latency, 0.0f);
  float* channels[] = {padded.data()};
  processor.process(channels, 1, n + latency);
  std::copy(padded.begin() + latency, padded.begin() + latency + n, samples.begin());
}

void run_processor_stereo(common::ProcessorBase& processor, std::vector<float>& left,
                          std::vector<float>& right, int sample_rate) {
  if (left.empty()) {
    return;
  }
  if (left.size() != right.size()) {
    throw std::invalid_argument("stereo channel lengths must match");
  }
  const int n = static_cast<int>(left.size());
  // Query latency: prepare once at N, read latency (valid post-prepare for our
  // processors).
  processor.prepare(sample_rate, n);
  const int latency = processor.latency_samples();
  if (latency <= 0) {
    float* channels[] = {left.data(), right.data()};
    processor.process(channels, 2, n);
    return;
  }
  // Re-prepare for the padded length (prepare() reinitializes processor state,
  // so a separate reset() before it would be redundant), then process N signal +
  // `latency` zeros and drop the leading `latency` output samples so the result
  // is time-aligned and the delayed tail is flushed out.
  processor.prepare(sample_rate, n + latency);
  std::vector<float> padded_left(left.begin(), left.end());
  std::vector<float> padded_right(right.begin(), right.end());
  padded_left.resize(static_cast<std::size_t>(n) + latency, 0.0f);
  padded_right.resize(static_cast<std::size_t>(n) + latency, 0.0f);
  float* channels[] = {padded_left.data(), padded_right.data()};
  processor.process(channels, 2, n + latency);
  std::copy(padded_left.begin() + latency, padded_left.begin() + latency + n, left.begin());
  std::copy(padded_right.begin() + latency, padded_right.begin() + latency + n, right.begin());
}

// Returns the per-band gain reduction with the largest magnitude (most-reduced
// band). Returns 0.0f for an empty vector.
float max_abs_gain_reduction(const std::vector<float>& gain_reductions_db) {
  float most_reduced = 0.0f;
  for (float gr : gain_reductions_db) {
    if (std::abs(gr) > std::abs(most_reduced)) {
      most_reduced = gr;
    }
  }
  return most_reduced;
}

float integrated_lufs(const std::vector<float>& samples, int sample_rate) {
  Audio audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
  return metering::lufs(audio).integrated_lufs;
}

// ---------------------------------------------------------------------------
// Count of enabled stages for progress callback denominator.
// ---------------------------------------------------------------------------

int count_enabled_mono_stages(const MasteringChainConfig& cfg) {
  int n = 0;
  if (cfg.repair.declick.enabled) ++n;
  if (cfg.repair.declip.enabled) ++n;
  if (cfg.repair.decrackle.enabled) ++n;
  if (cfg.repair.dehum.enabled) ++n;
  if (cfg.repair.dereverb.enabled) ++n;
  if (cfg.repair.denoise.enabled) ++n;
  if (cfg.eq.tilt.enabled) ++n;
  if (cfg.dynamics.deesser.enabled) ++n;
  if (cfg.dynamics.transient_shaper.enabled) ++n;
  if (cfg.dynamics.compressor.enabled) ++n;
  if (cfg.dynamics.multiband_comp.enabled) ++n;
  if (cfg.saturation.tape.enabled) ++n;
  if (cfg.saturation.exciter.enabled) ++n;
  if (cfg.spectral.air_band.enabled) ++n;
  if (cfg.maximizer.true_peak_limiter.enabled) ++n;
  if (cfg.loudness.enabled) ++n;
  return n;
}

int count_enabled_stereo_stages(const MasteringChainConfig& cfg) {
  int n = count_enabled_mono_stages(cfg);
  if (cfg.stereo.imager.enabled) ++n;
  if (cfg.stereo.mono_maker.enabled) ++n;
  return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// MasteringChain
// ---------------------------------------------------------------------------

MasteringChain::MasteringChain(MasteringChainConfig config) : config_(std::move(config)) {}

void MasteringChain::set_progress_callback(ProgressCallback callback) {
  progress_callback_ = std::move(callback);
}

MonoChainResult MasteringChain::process_mono(const float* samples, std::size_t length,
                                             int sample_rate) {
  MonoChainResult result;
  result.sample_rate = sample_rate;

  std::vector<float> data(samples, samples + length);
  result.input_lufs = integrated_lufs(data, sample_rate);
  float applied_gain_db = 0.0f;

  const int total = count_enabled_mono_stages(config_);
  int done = 0;
  auto report = [&](const char* stage_name) {
    result.stages.emplace_back(stage_name);
    ++done;
    if (progress_callback_ && total > 0) {
      progress_callback_(static_cast<float>(done) / static_cast<float>(total), stage_name);
    }
  };

  // 1. repair.declick
  if (config_.repair.declick.enabled) {
    Audio input = Audio::from_buffer(data.data(), data.size(), sample_rate);
    Audio repaired = mastering::repair::declick(input, config_.repair.declick.config);
    data.assign(repaired.data(), repaired.data() + repaired.size());
    report("repair.declick");
  }

  // 2. repair.declip
  if (config_.repair.declip.enabled) {
    Audio input = Audio::from_buffer(data.data(), data.size(), sample_rate);
    Audio repaired = mastering::repair::declip(input, config_.repair.declip.config);
    data.assign(repaired.data(), repaired.data() + repaired.size());
    report("repair.declip");
  }

  // 3. repair.decrackle
  if (config_.repair.decrackle.enabled) {
    Audio input = Audio::from_buffer(data.data(), data.size(), sample_rate);
    Audio repaired = mastering::repair::decrackle(input, config_.repair.decrackle.config);
    data.assign(repaired.data(), repaired.data() + repaired.size());
    report("repair.decrackle");
  }

  // 4. repair.dehum
  if (config_.repair.dehum.enabled) {
    Audio input = Audio::from_buffer(data.data(), data.size(), sample_rate);
    Audio repaired = mastering::repair::dehum(input, config_.repair.dehum.config);
    data.assign(repaired.data(), repaired.data() + repaired.size());
    report("repair.dehum");
  }

  // 5. repair.dereverb
  if (config_.repair.dereverb.enabled) {
    Audio input = Audio::from_buffer(data.data(), data.size(), sample_rate);
    Audio repaired = mastering::repair::dereverb_classical(input, config_.repair.dereverb.config);
    data.assign(repaired.data(), repaired.data() + repaired.size());
    report("repair.dereverb");
  }

  // 6. repair.denoise
  if (config_.repair.denoise.enabled) {
    Audio input = Audio::from_buffer(data.data(), data.size(), sample_rate);
    Audio repaired = mastering::repair::denoise_classical(input, config_.repair.denoise.config);
    data.assign(repaired.data(), repaired.data() + repaired.size());
    report("repair.denoise");
  }

  // 4. eq.tilt
  if (config_.eq.tilt.enabled) {
    mastering::eq::TiltEq tilt;
    tilt.set_tilt_db(config_.eq.tilt.tilt_db);
    tilt.set_pivot_hz(config_.eq.tilt.pivot_hz);
    run_processor_mono(tilt, data, sample_rate);
    report("eq.tilt");
  }

  // 5. dynamics.deesser
  if (config_.dynamics.deesser.enabled) {
    mastering::dynamics::DeEsser processor(config_.dynamics.deesser.config);
    run_processor_mono(processor, data, sample_rate);
    result.stage_gain_reductions.push_back(
        {"dynamics.deesser", processor.last_gain_reduction_db()});
    report("dynamics.deesser");
  }

  // 6. dynamics.transientShaper
  if (config_.dynamics.transient_shaper.enabled) {
    mastering::dynamics::TransientShaper processor(config_.dynamics.transient_shaper.config);
    run_processor_mono(processor, data, sample_rate);
    report("dynamics.transientShaper");
  }

  // 7. dynamics.compressor
  if (config_.dynamics.compressor.enabled) {
    mastering::dynamics::Compressor processor(config_.dynamics.compressor.config);
    run_processor_mono(processor, data, sample_rate);
    result.stage_gain_reductions.push_back(
        {"dynamics.compressor", processor.last_gain_reduction_db()});
    report("dynamics.compressor");
  }

  // 8. dynamics.multibandComp
  if (config_.dynamics.multiband_comp.enabled) {
    mastering::multiband::MultibandCompressor processor(config_.dynamics.multiband_comp.config);
    run_processor_mono(processor, data, sample_rate);
    result.stage_gain_reductions.push_back(
        {"dynamics.multibandComp", max_abs_gain_reduction(processor.last_gain_reductions_db())});
    report("dynamics.multibandComp");
  }

  // 9. saturation.tape
  if (config_.saturation.tape.enabled) {
    mastering::saturation::Tape processor(config_.saturation.tape.config);
    run_processor_mono(processor, data, sample_rate);
    report("saturation.tape");
  }

  // 10. saturation.exciter
  if (config_.saturation.exciter.enabled) {
    mastering::saturation::Exciter processor(config_.saturation.exciter.config);
    run_processor_mono(processor, data, sample_rate);
    report("saturation.exciter");
  }

  // 11. spectral.airBand
  if (config_.spectral.air_band.enabled) {
    mastering::spectral::AirBand processor(config_.spectral.air_band.config);
    run_processor_mono(processor, data, sample_rate);
    report("spectral.airBand");
  }

  // 12. maximizer.truePeakLimiter
  if (config_.maximizer.true_peak_limiter.enabled) {
    mastering::maximizer::TruePeakLimiter processor(config_.maximizer.true_peak_limiter.config);
    run_processor_mono(processor, data, sample_rate);
    result.stage_gain_reductions.push_back(
        {"maximizer.truePeakLimiter", processor.last_gain_reduction_db()});
    report("maximizer.truePeakLimiter");
  }

  // 13. loudness (mono path: manual gain + TruePeakLimiter pass, mirrors stereo)
  if (config_.loudness.enabled) {
    const float current_lufs = integrated_lufs(data, sample_rate);
    if (std::isfinite(current_lufs)) {
      const float gain_db = config_.loudness.target_lufs - current_lufs;
      detail::apply_gain_db(data, gain_db);
      applied_gain_db += gain_db;
    }
    mastering::maximizer::TruePeakLimiterConfig limiter_config;
    limiter_config.ceiling_db = config_.loudness.ceiling_db;
    limiter_config.oversample_factor = config_.loudness.true_peak_oversample;
    limiter_config.release_ms = config_.loudness.release_ms;
    limiter_config.apply_gain_at_input_rate = config_.loudness.apply_gain_at_input_rate;
    mastering::maximizer::TruePeakLimiter processor(limiter_config);
    run_processor_mono(processor, data, sample_rate);
    result.stage_gain_reductions.push_back(
        {"loudness.optimize", processor.last_gain_reduction_db()});
    report("loudness.optimize");
  }

  result.output_lufs = integrated_lufs(data, sample_rate);
  result.applied_gain_db = applied_gain_db;
  {
    Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
    result.output_true_peak_dbtp = metering::true_peak_db(audio, 4);
    result.output_lra = metering::lufs(audio).loudness_range;
  }
  result.samples = std::move(data);
  return result;
}

StereoChainResult MasteringChain::process_stereo(const float* left_in, const float* right_in,
                                                 std::size_t length, int sample_rate) {
  StereoChainResult result;
  result.sample_rate = sample_rate;

  std::vector<float> left(left_in, left_in + length);
  std::vector<float> right(right_in, right_in + length);

  result.input_lufs = integrated_lufs(detail::mono_mix(left, right), sample_rate);
  float applied_gain_db = 0.0f;

  const int total = count_enabled_stereo_stages(config_);
  int done = 0;
  auto report = [&](const char* stage_name) {
    result.stages.emplace_back(stage_name);
    ++done;
    if (progress_callback_ && total > 0) {
      progress_callback_(static_cast<float>(done) / static_cast<float>(total), stage_name);
    }
  };

  // 1. repair.declick (per-channel)
  if (config_.repair.declick.enabled) {
    Audio left_audio = Audio::from_buffer(left.data(), left.size(), sample_rate);
    Audio right_audio = Audio::from_buffer(right.data(), right.size(), sample_rate);
    Audio left_repaired = mastering::repair::declick(left_audio, config_.repair.declick.config);
    Audio right_repaired = mastering::repair::declick(right_audio, config_.repair.declick.config);
    left.assign(left_repaired.data(), left_repaired.data() + left_repaired.size());
    right.assign(right_repaired.data(), right_repaired.data() + right_repaired.size());
    report("repair.declick");
  }

  // 2. repair.declip (per-channel)
  if (config_.repair.declip.enabled) {
    Audio left_audio = Audio::from_buffer(left.data(), left.size(), sample_rate);
    Audio right_audio = Audio::from_buffer(right.data(), right.size(), sample_rate);
    Audio left_repaired = mastering::repair::declip(left_audio, config_.repair.declip.config);
    Audio right_repaired = mastering::repair::declip(right_audio, config_.repair.declip.config);
    left.assign(left_repaired.data(), left_repaired.data() + left_repaired.size());
    right.assign(right_repaired.data(), right_repaired.data() + right_repaired.size());
    report("repair.declip");
  }

  // 3. repair.decrackle (per-channel)
  if (config_.repair.decrackle.enabled) {
    Audio left_audio = Audio::from_buffer(left.data(), left.size(), sample_rate);
    Audio right_audio = Audio::from_buffer(right.data(), right.size(), sample_rate);
    Audio left_repaired = mastering::repair::decrackle(left_audio, config_.repair.decrackle.config);
    Audio right_repaired =
        mastering::repair::decrackle(right_audio, config_.repair.decrackle.config);
    left.assign(left_repaired.data(), left_repaired.data() + left_repaired.size());
    right.assign(right_repaired.data(), right_repaired.data() + right_repaired.size());
    report("repair.decrackle");
  }

  // 4. repair.dehum (per-channel)
  if (config_.repair.dehum.enabled) {
    Audio left_audio = Audio::from_buffer(left.data(), left.size(), sample_rate);
    Audio right_audio = Audio::from_buffer(right.data(), right.size(), sample_rate);
    Audio left_repaired = mastering::repair::dehum(left_audio, config_.repair.dehum.config);
    Audio right_repaired = mastering::repair::dehum(right_audio, config_.repair.dehum.config);
    left.assign(left_repaired.data(), left_repaired.data() + left_repaired.size());
    right.assign(right_repaired.data(), right_repaired.data() + right_repaired.size());
    report("repair.dehum");
  }

  // 5. repair.dereverb (per-channel)
  if (config_.repair.dereverb.enabled) {
    Audio left_audio = Audio::from_buffer(left.data(), left.size(), sample_rate);
    Audio right_audio = Audio::from_buffer(right.data(), right.size(), sample_rate);
    Audio left_repaired =
        mastering::repair::dereverb_classical(left_audio, config_.repair.dereverb.config);
    Audio right_repaired =
        mastering::repair::dereverb_classical(right_audio, config_.repair.dereverb.config);
    left.assign(left_repaired.data(), left_repaired.data() + left_repaired.size());
    right.assign(right_repaired.data(), right_repaired.data() + right_repaired.size());
    report("repair.dereverb");
  }

  // 6. repair.denoise (per-channel)
  if (config_.repair.denoise.enabled) {
    Audio left_audio = Audio::from_buffer(left.data(), left.size(), sample_rate);
    Audio right_audio = Audio::from_buffer(right.data(), right.size(), sample_rate);
    Audio left_repaired =
        mastering::repair::denoise_classical(left_audio, config_.repair.denoise.config);
    Audio right_repaired =
        mastering::repair::denoise_classical(right_audio, config_.repair.denoise.config);
    left.assign(left_repaired.data(), left_repaired.data() + left_repaired.size());
    right.assign(right_repaired.data(), right_repaired.data() + right_repaired.size());
    report("repair.denoise");
  }

  // 4. eq.tilt
  if (config_.eq.tilt.enabled) {
    mastering::eq::TiltEq tilt;
    tilt.set_tilt_db(config_.eq.tilt.tilt_db);
    tilt.set_pivot_hz(config_.eq.tilt.pivot_hz);
    run_processor_stereo(tilt, left, right, sample_rate);
    report("eq.tilt");
  }

  // 5. dynamics.deesser
  if (config_.dynamics.deesser.enabled) {
    mastering::dynamics::DeEsser processor(config_.dynamics.deesser.config);
    run_processor_stereo(processor, left, right, sample_rate);
    result.stage_gain_reductions.push_back(
        {"dynamics.deesser", processor.last_gain_reduction_db()});
    report("dynamics.deesser");
  }

  // 6. dynamics.transientShaper
  if (config_.dynamics.transient_shaper.enabled) {
    mastering::dynamics::TransientShaper processor(config_.dynamics.transient_shaper.config);
    run_processor_stereo(processor, left, right, sample_rate);
    report("dynamics.transientShaper");
  }

  // 7. dynamics.compressor
  if (config_.dynamics.compressor.enabled) {
    mastering::dynamics::Compressor processor(config_.dynamics.compressor.config);
    run_processor_stereo(processor, left, right, sample_rate);
    result.stage_gain_reductions.push_back(
        {"dynamics.compressor", processor.last_gain_reduction_db()});
    report("dynamics.compressor");
  }

  // 8. dynamics.multibandComp
  if (config_.dynamics.multiband_comp.enabled) {
    mastering::multiband::MultibandCompressor processor(config_.dynamics.multiband_comp.config);
    run_processor_stereo(processor, left, right, sample_rate);
    result.stage_gain_reductions.push_back(
        {"dynamics.multibandComp", max_abs_gain_reduction(processor.last_gain_reductions_db())});
    report("dynamics.multibandComp");
  }

  // 9. saturation.tape
  if (config_.saturation.tape.enabled) {
    mastering::saturation::Tape processor(config_.saturation.tape.config);
    run_processor_stereo(processor, left, right, sample_rate);
    report("saturation.tape");
  }

  // 10. saturation.exciter
  if (config_.saturation.exciter.enabled) {
    mastering::saturation::Exciter processor(config_.saturation.exciter.config);
    run_processor_stereo(processor, left, right, sample_rate);
    report("saturation.exciter");
  }

  // 11. spectral.airBand
  if (config_.spectral.air_band.enabled) {
    mastering::spectral::AirBand processor(config_.spectral.air_band.config);
    run_processor_stereo(processor, left, right, sample_rate);
    report("spectral.airBand");
  }

  // 12. stereo.imager
  if (config_.stereo.imager.enabled) {
    mastering::stereo::Imager processor(config_.stereo.imager.config);
    run_processor_stereo(processor, left, right, sample_rate);
    report("stereo.imager");
  }

  // 13. stereo.monoMaker
  if (config_.stereo.mono_maker.enabled) {
    mastering::stereo::MonoMaker processor(config_.stereo.mono_maker.config);
    run_processor_stereo(processor, left, right, sample_rate);
    report("stereo.monoMaker");
  }

  // 14. maximizer.truePeakLimiter
  if (config_.maximizer.true_peak_limiter.enabled) {
    mastering::maximizer::TruePeakLimiter processor(config_.maximizer.true_peak_limiter.config);
    run_processor_stereo(processor, left, right, sample_rate);
    result.stage_gain_reductions.push_back(
        {"maximizer.truePeakLimiter", processor.last_gain_reduction_db()});
    report("maximizer.truePeakLimiter");
  }

  // 15. loudness (stereo path: manual gain + TruePeakLimiter pass)
  if (config_.loudness.enabled) {
    const float current_lufs = integrated_lufs(detail::mono_mix(left, right), sample_rate);
    if (std::isfinite(current_lufs)) {
      const float gain_db = config_.loudness.target_lufs - current_lufs;
      detail::apply_gain_db(left, right, gain_db);
      applied_gain_db += gain_db;
    }
    mastering::maximizer::TruePeakLimiterConfig limiter_config;
    limiter_config.ceiling_db = config_.loudness.ceiling_db;
    limiter_config.oversample_factor = config_.loudness.true_peak_oversample;
    limiter_config.release_ms = config_.loudness.release_ms;
    limiter_config.apply_gain_at_input_rate = config_.loudness.apply_gain_at_input_rate;
    mastering::maximizer::TruePeakLimiter processor(limiter_config);
    run_processor_stereo(processor, left, right, sample_rate);
    result.stage_gain_reductions.push_back(
        {"loudness.optimize", processor.last_gain_reduction_db()});
    report("loudness.optimize");
  }

  result.output_lufs = integrated_lufs(detail::mono_mix(left, right), sample_rate);
  result.applied_gain_db = applied_gain_db;
  {
    Audio left_audio = Audio::from_buffer(left.data(), left.size(), sample_rate);
    Audio right_audio = Audio::from_buffer(right.data(), right.size(), sample_rate);
    result.output_true_peak_dbtp =
        std::max(metering::true_peak_db(left_audio, 4), metering::true_peak_db(right_audio, 4));
    std::vector<float> mono = detail::mono_mix(left, right);
    Audio mono_audio = Audio::from_buffer(mono.data(), mono.size(), sample_rate);
    result.output_lra = metering::lufs(mono_audio).loudness_range;
  }
  result.left = std::move(left);
  result.right = std::move(right);
  return result;
}
}  // namespace sonare::mastering::api
