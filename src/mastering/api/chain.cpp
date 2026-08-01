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
#include "mastering/api/internal_processor_runner.h"
#include "mastering/common/loudness_measure.h"
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
#include "rt/processor_base.h"

namespace sonare::mastering::api {
namespace {

using internal::run_processor_mono;
using internal::run_processor_stereo;

bool valid_true_peak_oversample(int factor) noexcept {
  return factor == 1 || factor == 2 || factor == 4 || factor == 8 || factor == 16;
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
  return common::measure_lufs(samples.data(), samples.size(), sample_rate);
}

// Oversample factor at which the chain's reported output true peak is measured.
// It follows the peak-limiting stage the chain actually applied — the loudness
// limiter when loudness is enabled, otherwise the maximizer true-peak limiter
// when that ran — decoupled from a disabled loudness stage's configured factor,
// after construction has rejected unsupported factors.
int reported_true_peak_oversample(const MasteringChainConfig& config) {
  int factor = config.loudness.true_peak_oversample;
  if (!config.loudness.enabled && config.maximizer.true_peak_limiter.enabled) {
    factor = config.maximizer.true_peak_limiter.config.oversample_factor;
  }
  return factor;
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

void validate_mastering_chain_config(const MasteringChainConfig& config) {
  SONARE_CHECK_MSG(valid_true_peak_oversample(config.loudness.true_peak_oversample),
                   ErrorCode::InvalidParameter,
                   "loudness.truePeakOversample must be one of 1, 2, 4, 8, or 16");
  if (config.maximizer.true_peak_limiter.enabled) {
    SONARE_CHECK_MSG(
        valid_true_peak_oversample(config.maximizer.true_peak_limiter.config.oversample_factor),
        ErrorCode::InvalidParameter,
        "maximizer.truePeakLimiter.oversampleFactor must be one of 1, 2, 4, 8, or 16");
  }
}

MasteringChain::MasteringChain(MasteringChainConfig config) : config_(std::move(config)) {
  validate_mastering_chain_config(config_);
}

void MasteringChain::set_progress_callback(ProgressCallback callback) {
  progress_callback_ = std::move(callback);
}

void MasteringChain::set_cancel_callback(CancelCallback should_cancel) {
  cancel_callback_ = std::move(should_cancel);
}

template <bool CheckCancel>
std::optional<MonoChainResult> MasteringChain::process_mono_impl(const float* samples,
                                                                 std::size_t length,
                                                                 int sample_rate) {
  // Centralized offline-input validation so every surface (C ABI, Node, WASM,
  // Python) rejects empty / out-of-range-rate / non-finite input identically.
  // The realtime block path (process_block) intentionally does not funnel here.
  validate_offline_audio_input(samples, length, sample_rate);

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
    if constexpr (CheckCancel) {
      return !cancel_callback_ || !cancel_callback_();
    }
    return true;
  };

  // 1. repair.declick
  if (config_.repair.declick.enabled) {
    detail::apply_repair_in_place(data, sample_rate, [this](const Audio& in) {
      return mastering::repair::declick(in, config_.repair.declick.config);
    });
    if (!report("repair.declick")) return std::nullopt;
  }

  // 2. repair.declip
  if (config_.repair.declip.enabled) {
    detail::apply_repair_in_place(data, sample_rate, [this](const Audio& in) {
      return mastering::repair::declip(in, config_.repair.declip.config);
    });
    if (!report("repair.declip")) return std::nullopt;
  }

  // 3. repair.decrackle
  if (config_.repair.decrackle.enabled) {
    detail::apply_repair_in_place(data, sample_rate, [this](const Audio& in) {
      return mastering::repair::decrackle(in, config_.repair.decrackle.config);
    });
    if (!report("repair.decrackle")) return std::nullopt;
  }

  // 4. repair.dehum
  if (config_.repair.dehum.enabled) {
    detail::apply_repair_in_place(data, sample_rate, [this](const Audio& in) {
      return mastering::repair::dehum(in, config_.repair.dehum.config);
    });
    if (!report("repair.dehum")) return std::nullopt;
  }

  // 5. repair.dereverb
  if (config_.repair.dereverb.enabled) {
    detail::apply_repair_in_place(data, sample_rate, [this](const Audio& in) {
      return mastering::repair::dereverb_classical(in, config_.repair.dereverb.config);
    });
    if (!report("repair.dereverb")) return std::nullopt;
  }

  // 6. repair.denoise
  if (config_.repair.denoise.enabled) {
    detail::apply_repair_in_place(data, sample_rate, [this](const Audio& in) {
      return mastering::repair::denoise_classical(in, config_.repair.denoise.config);
    });
    if (!report("repair.denoise")) return std::nullopt;
  }

  // 7. eq.tilt
  if (config_.eq.tilt.enabled) {
    mastering::eq::TiltEq tilt;
    tilt.set_tilt_db(config_.eq.tilt.tilt_db);
    tilt.set_pivot_hz(config_.eq.tilt.pivot_hz);
    run_processor_mono(tilt, data, sample_rate);
    if (!report("eq.tilt")) return std::nullopt;
  }

  // 8. dynamics.deesser
  if (config_.dynamics.deesser.enabled) {
    mastering::dynamics::DeEsser processor(config_.dynamics.deesser.config);
    run_processor_mono(processor, data, sample_rate);
    result.stage_gain_reductions.push_back(
        {"dynamics.deesser", processor.last_gain_reduction_db()});
    if (!report("dynamics.deesser")) return std::nullopt;
  }

  // 9. dynamics.transientShaper
  if (config_.dynamics.transient_shaper.enabled) {
    mastering::dynamics::TransientShaper processor(config_.dynamics.transient_shaper.config);
    run_processor_mono(processor, data, sample_rate);
    if (!report("dynamics.transientShaper")) return std::nullopt;
  }

  // 10. dynamics.compressor
  if (config_.dynamics.compressor.enabled) {
    mastering::dynamics::Compressor processor(config_.dynamics.compressor.config);
    run_processor_mono(processor, data, sample_rate);
    result.stage_gain_reductions.push_back(
        {"dynamics.compressor", processor.last_gain_reduction_db()});
    if (!report("dynamics.compressor")) return std::nullopt;
  }

  // 11. dynamics.multibandComp
  if (config_.dynamics.multiband_comp.enabled) {
    mastering::multiband::MultibandCompressor processor(config_.dynamics.multiband_comp.config);
    run_processor_mono(processor, data, sample_rate);
    result.stage_gain_reductions.push_back(
        {"dynamics.multibandComp", max_abs_gain_reduction(processor.last_gain_reductions_db())});
    if (!report("dynamics.multibandComp")) return std::nullopt;
  }

  // 12. saturation.tape
  if (config_.saturation.tape.enabled) {
    mastering::saturation::Tape processor(config_.saturation.tape.config);
    run_processor_mono(processor, data, sample_rate);
    if (!report("saturation.tape")) return std::nullopt;
  }

  // 13. saturation.exciter
  if (config_.saturation.exciter.enabled) {
    mastering::saturation::Exciter processor(config_.saturation.exciter.config);
    run_processor_mono(processor, data, sample_rate);
    if (!report("saturation.exciter")) return std::nullopt;
  }

  // 14. spectral.airBand
  if (config_.spectral.air_band.enabled) {
    mastering::spectral::AirBand processor(config_.spectral.air_band.config);
    run_processor_mono(processor, data, sample_rate);
    if (!report("spectral.airBand")) return std::nullopt;
  }

  // 15. maximizer.truePeakLimiter
  if (config_.maximizer.true_peak_limiter.enabled) {
    mastering::maximizer::TruePeakLimiter processor(config_.maximizer.true_peak_limiter.config);
    run_processor_mono(processor, data, sample_rate);
    result.stage_gain_reductions.push_back(
        {"maximizer.truePeakLimiter", processor.last_gain_reduction_db()});
    if (!report("maximizer.truePeakLimiter")) return std::nullopt;
  }

  // 16. loudness (mono path: manual gain + TruePeakLimiter pass, mirrors stereo)
  if (config_.loudness.enabled) {
    // Clamp the static normalization gain to the ceiling headroom (mirrors the
    // mono loudness_optimize() helper) so the limiter is not overdriven.
    const float gain_db = detail::loudness_gain_db_with_ceiling(
        data, sample_rate, config_.loudness.target_lufs, config_.loudness.ceiling_db,
        config_.loudness.true_peak_oversample);
    const float requested_gain_db =
        config_.loudness.target_lufs - integrated_lufs(data, sample_rate);
    result.loudness_target_limited =
        std::isfinite(requested_gain_db) && gain_db < requested_gain_db - 1e-4f;
    if (gain_db != 0.0f) {
      detail::apply_gain_db(data, gain_db);
      applied_gain_db += gain_db;
    }
    const mastering::maximizer::TruePeakLimiterConfig limiter_config =
        mastering::maximizer::loudness_limiter_config(
            config_.loudness.ceiling_db, config_.loudness.true_peak_oversample,
            config_.loudness.release_ms, config_.loudness.apply_gain_at_input_rate);
    mastering::maximizer::TruePeakLimiter processor(limiter_config);
    run_processor_mono(processor, data, sample_rate);
    result.stage_gain_reductions.push_back(
        {"loudness.optimize", processor.last_gain_reduction_db()});
    if (!report("loudness.optimize")) return std::nullopt;
  }

  result.output_lufs = integrated_lufs(data, sample_rate);
  result.applied_gain_db = applied_gain_db;
  {
    Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
    result.output_true_peak_dbtp =
        common::measure_true_peak_dbtp(audio, reported_true_peak_oversample(config_));
    result.output_lra = common::measure_lra(audio);
  }
  result.samples = std::move(data);
  return result;
}

template <bool CheckCancel>
std::optional<StereoChainResult> MasteringChain::process_stereo_impl(const float* left_in,
                                                                     const float* right_in,
                                                                     std::size_t length,
                                                                     int sample_rate) {
  // Centralized offline-input validation for both channels (see process_mono).
  validate_offline_audio_input(left_in, length, sample_rate);
  validate_offline_audio_input(right_in, length, sample_rate);

  StereoChainResult result;
  result.sample_rate = sample_rate;

  std::vector<float> left(left_in, left_in + length);
  std::vector<float> right(right_in, right_in + length);

  result.input_lufs = detail::stereo_integrated_lufs(left, right, sample_rate);
  float applied_gain_db = 0.0f;

  const int total = count_enabled_stereo_stages(config_);
  int done = 0;
  auto report = [&](const char* stage_name) {
    result.stages.emplace_back(stage_name);
    ++done;
    if (progress_callback_ && total > 0) {
      progress_callback_(static_cast<float>(done) / static_cast<float>(total), stage_name);
    }
    if constexpr (CheckCancel) {
      return !cancel_callback_ || !cancel_callback_();
    }
    return true;
  };

  // 1. repair.declick (per-channel)
  if (config_.repair.declick.enabled) {
    detail::apply_independent_repair(left, right, sample_rate, [this](const Audio& in) {
      return mastering::repair::declick(in, config_.repair.declick.config);
    });
    if (!report("repair.declick")) return std::nullopt;
  }

  // 2. repair.declip (per-channel)
  if (config_.repair.declip.enabled) {
    detail::apply_independent_repair(left, right, sample_rate, [this](const Audio& in) {
      return mastering::repair::declip(in, config_.repair.declip.config);
    });
    if (!report("repair.declip")) return std::nullopt;
  }

  // 3. repair.decrackle (per-channel)
  if (config_.repair.decrackle.enabled) {
    detail::apply_independent_repair(left, right, sample_rate, [this](const Audio& in) {
      return mastering::repair::decrackle(in, config_.repair.decrackle.config);
    });
    if (!report("repair.decrackle")) return std::nullopt;
  }

  // 4. repair.dehum (per-channel)
  if (config_.repair.dehum.enabled) {
    detail::apply_independent_repair(left, right, sample_rate, [this](const Audio& in) {
      return mastering::repair::dehum(in, config_.repair.dehum.config);
    });
    if (!report("repair.dehum")) return std::nullopt;
  }

  // 5. repair.dereverb
  if (config_.repair.dereverb.enabled) {
    detail::apply_shared_mono_transfer_repair(left, right, sample_rate, [this](const Audio& audio) {
      return mastering::repair::dereverb_classical(audio, config_.repair.dereverb.config);
    });
    if (!report("repair.dereverb")) return std::nullopt;
  }

  // 6. repair.denoise
  if (config_.repair.denoise.enabled) {
    detail::apply_shared_mono_transfer_repair(left, right, sample_rate, [this](const Audio& audio) {
      return mastering::repair::denoise_classical(audio, config_.repair.denoise.config);
    });
    if (!report("repair.denoise")) return std::nullopt;
  }

  // 7. eq.tilt
  if (config_.eq.tilt.enabled) {
    mastering::eq::TiltEq tilt;
    tilt.set_tilt_db(config_.eq.tilt.tilt_db);
    tilt.set_pivot_hz(config_.eq.tilt.pivot_hz);
    run_processor_stereo(tilt, left, right, sample_rate);
    if (!report("eq.tilt")) return std::nullopt;
  }

  // 8. dynamics.deesser
  if (config_.dynamics.deesser.enabled) {
    mastering::dynamics::DeEsser processor(config_.dynamics.deesser.config);
    run_processor_stereo(processor, left, right, sample_rate);
    result.stage_gain_reductions.push_back(
        {"dynamics.deesser", processor.last_gain_reduction_db()});
    if (!report("dynamics.deesser")) return std::nullopt;
  }

  // 9. dynamics.transientShaper
  if (config_.dynamics.transient_shaper.enabled) {
    mastering::dynamics::TransientShaper processor(config_.dynamics.transient_shaper.config);
    run_processor_stereo(processor, left, right, sample_rate);
    if (!report("dynamics.transientShaper")) return std::nullopt;
  }

  // 10. dynamics.compressor
  if (config_.dynamics.compressor.enabled) {
    mastering::dynamics::Compressor processor(config_.dynamics.compressor.config);
    run_processor_stereo(processor, left, right, sample_rate);
    result.stage_gain_reductions.push_back(
        {"dynamics.compressor", processor.last_gain_reduction_db()});
    if (!report("dynamics.compressor")) return std::nullopt;
  }

  // 11. dynamics.multibandComp
  if (config_.dynamics.multiband_comp.enabled) {
    mastering::multiband::MultibandCompressor processor(config_.dynamics.multiband_comp.config);
    run_processor_stereo(processor, left, right, sample_rate);
    result.stage_gain_reductions.push_back(
        {"dynamics.multibandComp", max_abs_gain_reduction(processor.last_gain_reductions_db())});
    if (!report("dynamics.multibandComp")) return std::nullopt;
  }

  // 12. saturation.tape
  if (config_.saturation.tape.enabled) {
    mastering::saturation::Tape processor(config_.saturation.tape.config);
    run_processor_stereo(processor, left, right, sample_rate);
    if (!report("saturation.tape")) return std::nullopt;
  }

  // 13. saturation.exciter
  if (config_.saturation.exciter.enabled) {
    mastering::saturation::Exciter processor(config_.saturation.exciter.config);
    run_processor_stereo(processor, left, right, sample_rate);
    if (!report("saturation.exciter")) return std::nullopt;
  }

  // 14. spectral.airBand
  if (config_.spectral.air_band.enabled) {
    mastering::spectral::AirBand processor(config_.spectral.air_band.config);
    run_processor_stereo(processor, left, right, sample_rate);
    if (!report("spectral.airBand")) return std::nullopt;
  }

  // 15. stereo.imager
  if (config_.stereo.imager.enabled) {
    mastering::stereo::Imager processor(config_.stereo.imager.config);
    run_processor_stereo(processor, left, right, sample_rate);
    if (!report("stereo.imager")) return std::nullopt;
  }

  // 16. stereo.monoMaker
  if (config_.stereo.mono_maker.enabled) {
    mastering::stereo::MonoMaker processor(config_.stereo.mono_maker.config);
    run_processor_stereo(processor, left, right, sample_rate);
    if (!report("stereo.monoMaker")) return std::nullopt;
  }

  // 17. maximizer.truePeakLimiter
  if (config_.maximizer.true_peak_limiter.enabled) {
    mastering::maximizer::TruePeakLimiter processor(config_.maximizer.true_peak_limiter.config);
    run_processor_stereo(processor, left, right, sample_rate);
    result.stage_gain_reductions.push_back(
        {"maximizer.truePeakLimiter", processor.last_gain_reduction_db()});
    if (!report("maximizer.truePeakLimiter")) return std::nullopt;
  }

  // 18. loudness (stereo path: manual gain + TruePeakLimiter pass)
  if (config_.loudness.enabled) {
    // Clamp the static normalization gain to the ceiling headroom (mirrors the
    // mono loudness_optimize() helper) so the limiter is not overdriven.
    const float gain_db = detail::loudness_gain_db_with_ceiling(
        left, right, sample_rate, config_.loudness.target_lufs, config_.loudness.ceiling_db,
        config_.loudness.true_peak_oversample);
    const float requested_gain_db =
        config_.loudness.target_lufs - detail::stereo_integrated_lufs(left, right, sample_rate);
    result.loudness_target_limited =
        std::isfinite(requested_gain_db) && gain_db < requested_gain_db - 1e-4f;
    if (gain_db != 0.0f) {
      detail::apply_gain_db(left, right, gain_db);
      applied_gain_db += gain_db;
    }
    const mastering::maximizer::TruePeakLimiterConfig limiter_config =
        mastering::maximizer::loudness_limiter_config(
            config_.loudness.ceiling_db, config_.loudness.true_peak_oversample,
            config_.loudness.release_ms, config_.loudness.apply_gain_at_input_rate);
    mastering::maximizer::TruePeakLimiter processor(limiter_config);
    run_processor_stereo(processor, left, right, sample_rate);
    result.stage_gain_reductions.push_back(
        {"loudness.optimize", processor.last_gain_reduction_db()});
    if (!report("loudness.optimize")) return std::nullopt;
  }

  result.output_lufs = detail::stereo_integrated_lufs(left, right, sample_rate);
  result.applied_gain_db = applied_gain_db;
  {
    Audio left_audio = Audio::from_buffer(left.data(), left.size(), sample_rate);
    Audio right_audio = Audio::from_buffer(right.data(), right.size(), sample_rate);
    const int tp_oversample = reported_true_peak_oversample(config_);
    result.output_true_peak_dbtp =
        std::max(common::measure_true_peak_dbtp(left_audio, tp_oversample),
                 common::measure_true_peak_dbtp(right_audio, tp_oversample));
    // LRA is measured with BS.1770 channel summing (matching output_lufs), not a
    // 0.5*(L+R) mono downmix, which would phase-cancel wide / out-of-phase stereo.
    const std::vector<float> interleaved = detail::interleave_stereo(left, right);
    result.output_lra =
        common::measure_lra_interleaved(interleaved.data(), left.size(), 2, sample_rate);
  }
  result.left = std::move(left);
  result.right = std::move(right);
  return result;
}

MonoChainResult MasteringChain::process_mono(const float* samples, std::size_t length,
                                             int sample_rate) {
  return *process_mono_impl<false>(samples, length, sample_rate);
}

StereoChainResult MasteringChain::process_stereo(const float* left, const float* right,
                                                 std::size_t length, int sample_rate) {
  return *process_stereo_impl<false>(left, right, length, sample_rate);
}

std::optional<MonoChainResult> MasteringChain::process_mono_cancellable(const float* samples,
                                                                        std::size_t length,
                                                                        int sample_rate) {
  return process_mono_impl<true>(samples, length, sample_rate);
}

std::optional<StereoChainResult> MasteringChain::process_stereo_cancellable(const float* left,
                                                                            const float* right,
                                                                            std::size_t length,
                                                                            int sample_rate) {
  return process_stereo_impl<true>(left, right, length, sample_rate);
}
}  // namespace sonare::mastering::api
