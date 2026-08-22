/// @file chain.cpp
/// @brief Implementation of the high-level mastering chain composition.

#include "mastering/api/chain.h"

#include <algorithm>
#include <array>
#include <cmath>
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
#include "mastering/match/reference_spectrum.h"
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

MasteringLoudnessSummary to_report_summary(const common::LoudnessSummary& summary) {
  return {summary.integrated_lufs, summary.max_momentary_lufs, summary.max_short_term_lufs,
          summary.true_peak_dbtp, summary.loudness_range};
}

// `reference_spectrum()` is the existing long-term spectrum implementation
// used by the reference-match processors. The report only resamples its output
// into a compact fixed shape; it does not estimate any new musical property.
float interpolated_spectrum_db(const mastering::match::ReferenceSpectrum& spectrum,
                               float frequency_hz) {
  const auto upper =
      std::lower_bound(spectrum.frequencies.begin(), spectrum.frequencies.end(), frequency_hz);
  if (upper == spectrum.frequencies.begin()) return spectrum.db.front();
  if (upper == spectrum.frequencies.end()) return spectrum.db.back();
  const size_t high = static_cast<size_t>(upper - spectrum.frequencies.begin());
  const size_t low = high - 1;
  const float low_frequency = spectrum.frequencies[low];
  const float high_frequency = spectrum.frequencies[high];
  const float ratio = (frequency_hz - low_frequency) / (high_frequency - low_frequency);
  return spectrum.db[low] + ratio * (spectrum.db[high] - spectrum.db[low]);
}

std::array<float, kMasteringReportBandCount> spectrum_delta(
    const mastering::match::ReferenceSpectrum& before,
    const mastering::match::ReferenceSpectrum& after, int sample_rate) {
  std::array<float, kMasteringReportBandCount> delta{};
  const float low_hz = std::min(20.0f, static_cast<float>(sample_rate) * 0.5f);
  const float high_hz = static_cast<float>(sample_rate) * 0.5f;
  for (size_t index = 0; index < delta.size(); ++index) {
    const float position =
        (static_cast<float>(index) + 0.5f) / static_cast<float>(kMasteringReportBandCount);
    const float frequency_hz = low_hz * std::pow(high_hz / low_hz, position);
    delta[index] = interpolated_spectrum_db(after, frequency_hz) -
                   interpolated_spectrum_db(before, frequency_hz);
  }
  return delta;
}

float stereo_spectrum_db(const mastering::match::ReferenceSpectrum& left,
                         const mastering::match::ReferenceSpectrum& right, float frequency_hz) {
  const float left_power = std::pow(10.0f, interpolated_spectrum_db(left, frequency_hz) / 10.0f);
  const float right_power = std::pow(10.0f, interpolated_spectrum_db(right, frequency_hz) / 10.0f);
  return 10.0f * std::log10(0.5f * (left_power + right_power));
}

// Long-term spectrum of one channel. Audio::from_buffer deep-copies, so the
// copy is scoped to this call: the caller keeps the 1025-bin spectrum, not
// another track-length buffer.
mastering::match::ReferenceSpectrum channel_spectrum(const std::vector<float>& channel,
                                                     int sample_rate) {
  const Audio audio = Audio::from_buffer(channel.data(), channel.size(), sample_rate);
  return mastering::match::reference_spectrum(audio);
}

// Takes spectra rather than Audio so the caller decides how long each
// track-length copy stays alive; the report only ever needs the bins.
std::array<float, kMasteringReportBandCount> stereo_spectrum_delta(
    const mastering::match::ReferenceSpectrum& before_left,
    const mastering::match::ReferenceSpectrum& before_right,
    const mastering::match::ReferenceSpectrum& after_left,
    const mastering::match::ReferenceSpectrum& after_right, int sample_rate) {
  std::array<float, kMasteringReportBandCount> delta{};
  const float low_hz = std::min(20.0f, static_cast<float>(sample_rate) * 0.5f);
  const float high_hz = static_cast<float>(sample_rate) * 0.5f;
  for (size_t index = 0; index < delta.size(); ++index) {
    const float position =
        (static_cast<float>(index) + 0.5f) / static_cast<float>(kMasteringReportBandCount);
    const float frequency_hz = low_hz * std::pow(high_hz / low_hz, position);
    delta[index] = stereo_spectrum_db(after_left, after_right, frequency_hz) -
                   stereo_spectrum_db(before_left, before_right, frequency_hz);
  }
  return delta;
}

float max_gain_reduction_db(const std::vector<StageGainReduction>& reductions) {
  float max_reduction = 0.0f;
  for (const auto& reduction : reductions) {
    max_reduction = std::min(max_reduction, reduction.gain_reduction_db);
  }
  return max_reduction;
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
  SONARE_CHECK_MSG(std::isfinite(config.loudness.max_limiter_gain_reduction_db) &&
                       config.loudness.max_limiter_gain_reduction_db >= 0.0f,
                   ErrorCode::InvalidParameter,
                   "loudness.maxLimiterGainReductionDb must be finite and >= 0");
  if (config.maximizer.true_peak_limiter.enabled) {
    SONARE_CHECK_MSG(
        valid_true_peak_oversample(config.maximizer.true_peak_limiter.config.oversample_factor),
        ErrorCode::InvalidParameter,
        "maximizer.truePeakLimiter.oversampleFactor must be one of 1, 2, 4, 8, or 16");
  }
  if (config.eq.tilt.enabled) {
    // TiltEq::set_pivot_hz rejects this at stage time, which for a chain with
    // repair stages in front of it is minutes of STFT work into the render.
    // The rate-dependent half of the same constraint (pivot below Nyquist)
    // cannot be checked here because the sample rate is not known until
    // process time; validate_chain_config_for_rate() carries it.
    SONARE_CHECK_MSG(std::isfinite(config.eq.tilt.pivot_hz) && config.eq.tilt.pivot_hz > 0.0f,
                     ErrorCode::InvalidParameter, "eq.tilt.pivotHz must be finite and > 0");
    SONARE_CHECK_MSG(std::isfinite(config.eq.tilt.tilt_db), ErrorCode::InvalidParameter,
                     "eq.tilt.tiltDb must be finite");
  }
}

void validate_chain_config_for_rate(const MasteringChainConfig& config, int sample_rate) {
  // Everything here needs the sample rate, so it cannot live in the
  // construction-time check. It still runs before stage 1, which is the part
  // that matters: the alternative is discovering it after the repair stages
  // have already processed the whole track.
  if (config.eq.tilt.enabled && config.eq.tilt.tilt_db != 0.0f) {
    // A zero tilt leaves both shelves disabled, and a disabled band never has
    // its coefficients designed, so mirror that condition exactly rather than
    // rejecting a configuration the stage would have run.
    const float nyquist = 0.5f * static_cast<float>(sample_rate);
    SONARE_CHECK_MSG(config.eq.tilt.pivot_hz < nyquist, ErrorCode::InvalidParameter,
                     "eq.tilt.pivotHz must be below Nyquist for this sample rate");
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
  validate_chain_config_for_rate(config_, sample_rate);

  MonoChainResult result;
  result.sample_rate = sample_rate;

  std::vector<float> data(samples, samples + length);
  const int true_peak_oversample = reported_true_peak_oversample(config_);
  // Everything the report needs from the INPUT is reduced to its measurements
  // before a single stage runs, the same shape as the stereo path below: the
  // track-length Audio copy is scoped here and only the 1025-bin spectrum
  // survives to the band-delta at the end. Holding that copy across the whole
  // chain kept three track-length buffers alive at once.
  mastering::match::ReferenceSpectrum before_spectrum;
  {
    const Audio before_audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
    result.report.before =
        to_report_summary(common::measure_loudness_summary(before_audio, true_peak_oversample));
    before_spectrum = mastering::match::reference_spectrum(before_audio);
  }
  result.input_lufs = result.report.before.integrated_lufs;
  float applied_gain_db = 0.0f;

  const int total = count_enabled_mono_stages(config_);
  int done = 0;
  if (progress_callback_ && total == 0) {
    progress_callback_(1.0f, "complete");
    if constexpr (CheckCancel) {
      if (cancel_callback_ && cancel_callback_()) return std::nullopt;
    }
  }
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
        {"maximizer.truePeakLimiter", processor.minimum_gain_reduction_db()});
    if (!report("maximizer.truePeakLimiter")) return std::nullopt;
  }

  // 16. loudness (mono path: manual gain + TruePeakLimiter pass, mirrors stereo)
  float loudness_requested_gain_db = 0.0f;
  float loudness_applied_gain_db = 0.0f;
  if (config_.loudness.enabled) {
    // Bound the static normalization gain to the ceiling headroom plus the depth
    // the limiter below may be driven to (mirrors the mono loudness_optimize()
    // helper). Measure the post-processor stage input once. `report.before`
    // describes the original chain input and must not drive this stage's
    // normalization.
    const Audio stage_audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
    const common::LufsAndTruePeak stage_measurement =
        common::measure_lufs_and_true_peak(stage_audio, config_.loudness.true_peak_oversample);
    const float gain_db = detail::loudness_gain_db_with_ceiling(
        stage_measurement.integrated_lufs, config_.loudness.target_lufs,
        config_.loudness.ceiling_db, stage_measurement.true_peak_dbtp,
        config_.loudness.max_limiter_gain_reduction_db);
    const float requested_gain_db =
        config_.loudness.target_lufs - stage_measurement.integrated_lufs;
    loudness_requested_gain_db = requested_gain_db;
    loudness_applied_gain_db = gain_db;
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
        {"loudness.optimize", processor.minimum_gain_reduction_db()});
    if (!report("loudness.optimize")) return std::nullopt;
  }

  // Same shape on the output side: measure, then release, one track-length
  // temporary at a time.
  mastering::match::ReferenceSpectrum after_spectrum;
  {
    const Audio after_audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
    result.report.after =
        to_report_summary(common::measure_loudness_summary(after_audio, true_peak_oversample));
    after_spectrum = mastering::match::reference_spectrum(after_audio);
  }
  result.output_lufs = result.report.after.integrated_lufs;
  // Loudness is the last stage, so the chain output measured just above is
  // exactly what it achieved; no second measurement is needed to tell a reached
  // target from a limited one.
  if (config_.loudness.enabled) {
    result.loudness_target_limited =
        detail::loudness_target_was_limited(loudness_requested_gain_db, loudness_applied_gain_db,
                                            config_.loudness.target_lufs, result.output_lufs);
  }
  result.applied_gain_db = applied_gain_db;
  result.output_true_peak_dbtp = result.report.after.true_peak_dbtp;
  result.output_lra = result.report.after.loudness_range;
  result.report.applied_gain_db = applied_gain_db;
  result.report.max_gain_reduction_db = max_gain_reduction_db(result.stage_gain_reductions);
  result.report.loudness_target_limited = result.loudness_target_limited;
  result.report.band_energy_delta_db = spectrum_delta(before_spectrum, after_spectrum, sample_rate);
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
  validate_chain_config_for_rate(config_, sample_rate);

  StereoChainResult result;
  result.sample_rate = sample_rate;

  std::vector<float> left(left_in, left_in + length);
  std::vector<float> right(right_in, right_in + length);

  const int true_peak_oversample = reported_true_peak_oversample(config_);
  // Everything the report needs from the INPUT is measured here and reduced to
  // its measurements before a single stage runs. Each track-length temporary
  // (the interleaved view the loudness meter consumes, and one Audio copy per
  // channel for the spectrum) is scoped so it is released before the next is
  // taken, and only two 1025-bin spectra survive to the end of the function.
  // Holding the four "before" copies until the band-delta report was computed
  // made the peak working set grow with the number of measurements taken
  // instead of with the track.
  mastering::match::ReferenceSpectrum before_left_spectrum;
  mastering::match::ReferenceSpectrum before_right_spectrum;
  {
    result.report.before = to_report_summary(common::measure_loudness_summary_stereo_planar(
        left.data(), right.data(), left.size(), sample_rate, true_peak_oversample));
    before_left_spectrum = channel_spectrum(left, sample_rate);
    before_right_spectrum = channel_spectrum(right, sample_rate);
  }
  result.input_lufs = result.report.before.integrated_lufs;
  float applied_gain_db = 0.0f;

  const int total = count_enabled_stereo_stages(config_);
  int done = 0;
  if (progress_callback_ && total == 0) {
    progress_callback_(1.0f, "complete");
    if constexpr (CheckCancel) {
      if (cancel_callback_ && cancel_callback_()) return std::nullopt;
    }
  }
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
        {"maximizer.truePeakLimiter", processor.minimum_gain_reduction_db()});
    if (!report("maximizer.truePeakLimiter")) return std::nullopt;
  }

  // 18. loudness (stereo path: manual gain + TruePeakLimiter pass)
  float loudness_requested_gain_db = 0.0f;
  float loudness_applied_gain_db = 0.0f;
  if (config_.loudness.enabled) {
    // One BS.1770 measurement drives both the requested gain and the bounded gain.
    // In particular, avoid a second interleaved stereo allocation for the target-limited flag.
    const float current_lufs = detail::stereo_integrated_lufs(left, right, sample_rate);
    const float peak_db = detail::stereo_true_peak_dbtp(left, right, sample_rate,
                                                        config_.loudness.true_peak_oversample);
    const float gain_db = detail::loudness_gain_db_with_ceiling(
        current_lufs, config_.loudness.target_lufs, config_.loudness.ceiling_db, peak_db,
        config_.loudness.max_limiter_gain_reduction_db);
    const float requested_gain_db = config_.loudness.target_lufs - current_lufs;
    loudness_requested_gain_db = requested_gain_db;
    loudness_applied_gain_db = gain_db;
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
        {"loudness.optimize", processor.minimum_gain_reduction_db()});
    if (!report("loudness.optimize")) return std::nullopt;
  }

  // Same shape on the output side: measure, then release, one track-length
  // temporary at a time.
  mastering::match::ReferenceSpectrum after_left_spectrum;
  mastering::match::ReferenceSpectrum after_right_spectrum;
  {
    result.report.after = to_report_summary(common::measure_loudness_summary_stereo_planar(
        left.data(), right.data(), left.size(), sample_rate, true_peak_oversample));
    after_left_spectrum = channel_spectrum(left, sample_rate);
    after_right_spectrum = channel_spectrum(right, sample_rate);
  }
  result.output_lufs = result.report.after.integrated_lufs;
  // Loudness is the last stage; see the mono path for why the chain output is
  // the achieved value the target-limited flag is decided against.
  if (config_.loudness.enabled) {
    result.loudness_target_limited =
        detail::loudness_target_was_limited(loudness_requested_gain_db, loudness_applied_gain_db,
                                            config_.loudness.target_lufs, result.output_lufs);
  }
  result.applied_gain_db = applied_gain_db;
  result.output_true_peak_dbtp = result.report.after.true_peak_dbtp;
  result.output_lra = result.report.after.loudness_range;
  result.report.applied_gain_db = applied_gain_db;
  result.report.max_gain_reduction_db = max_gain_reduction_db(result.stage_gain_reductions);
  result.report.loudness_target_limited = result.loudness_target_limited;
  result.report.band_energy_delta_db =
      stereo_spectrum_delta(before_left_spectrum, before_right_spectrum, after_left_spectrum,
                            after_right_spectrum, sample_rate);
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
