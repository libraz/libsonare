#include "mastering/dynamics/compressor.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare::mastering::dynamics {

namespace {

using sonare::constants::kFloorDb;

constexpr float kRmsWindowMs = 10.0f;
constexpr float kLogRmsWindowMs = 50.0f;

// Program-dependent-release normalization: the smoothed gain-reduction state
// (in dB) is mapped to a [0, 1] "how hard are we compressing" amount by
// dividing by this reference depth. 24 dB of sustained reduction saturates the
// program-dependent release at its maximum scaling.
constexpr float kPdrNormalizationDb = 24.0f;

// Fraction of the theoretical full makeup gain applied by the auto-makeup
// heuristic. The full static makeup that exactly restores the pre-compression
// level of a signal sitting at the threshold is
// (-threshold_db) * (1 - 1/ratio); applying all of it tends to overshoot on
// real program material because the average level is well below threshold, so
// we apply half of it as a conservative perceptual compromise.
constexpr float kAutoMakeupFraction = 0.5f;

DetectorMode detector_mode_from_param(float value) {
  switch (static_cast<int>(std::lround(value))) {
    case 0:
      return DetectorMode::Peak;
    case 2:
      return DetectorMode::LogRms;
    default:
      return DetectorMode::Rms;
  }
}

// Computes the total makeup gain in dB. Auto-makeup and an explicit
// makeup_gain_db are mutually exclusive to avoid double-compensation: if the
// user has dialed in any manual makeup, it overrides the auto heuristic.
float compute_makeup_db(const CompressorConfig& config) {
  if (config.makeup_gain_db != 0.0f || !config.auto_makeup) {
    return config.makeup_gain_db;
  }
  return std::max(0.0f, -config.threshold_db) * (1.0f - 1.0f / config.ratio) * kAutoMakeupFraction;
}

}  // namespace

// The configuration lifecycle (validate + seed active_ + publish the initial
// snapshot) is handled by RtConfigLifecycle's constructor.
Compressor::Compressor(CompressorConfig config) : ConfigBase(std::move(config)) {}

void Compressor::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  sample_rate_ = sample_rate;
  prepared_ = true;
  // Seed the audio thread's live working config from the prepared baseline and
  // derive its coefficients; the per-sample loop reads active_, not config_.
  active_ = config_;
  update_coefficients(active_);
  hpf_x1_.assign(kRealtimePreparedChannels, 0.0f);
  hpf_y1_.assign(kRealtimePreparedChannels, 0.0f);
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  republish_after_prepare();
}

void Compressor::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Compressor");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const CompressorConfig& cfg = *adopt_snapshot_for_block();
  const float makeup_db = compute_makeup_db(cfg);

  if (static_cast<size_t>(num_channels) > hpf_x1_.size() ||
      static_cast<size_t>(num_channels) > hpf_y1_.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels exceeds prepared Compressor state");
  }

  // Detect a detector-mode change since the previous block. The Rms and LogRms
  // detectors share rms_state_ with different time constants, so a mid-stream
  // switch would otherwise carry wrong-window state and spike the gain. Reseed
  // rms_state_ to the current instantaneous power on the first sample below.
  const bool detector_mode_changed =
      detector_mode_initialized_ && cfg.detector != last_detector_mode_;
  bool reseed_rms_state = detector_mode_changed;
  last_detector_mode_ = cfg.detector;
  detector_mode_initialized_ = true;

  const float inv_channels = 1.0f / static_cast<float>(num_channels);
  float max_reduction = 0.0f;
  for (int i = 0; i < num_samples; ++i) {
    // Linked detection: derive a single detector level from all channels each
    // sample so every channel receives the same gain (preserves stereo image).
    // Peak uses the loudest channel; RMS uses the mean power across channels so
    // the two detectors are consistent and anti-correlated content does not
    // collapse the detected level (which max(L^2, R^2) avoided but max-peak did
    // not, leaving the two paths inconsistent).
    float peak_lin = 0.0f;
    float power_sum = 0.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      float s = channels[ch][i];
      if (cfg.sidechain_hpf_enabled) {
        const auto idx = static_cast<size_t>(ch);
        const float y = hpf_b0_ * (s - hpf_x1_[idx]) + hpf_a1_ * hpf_y1_[idx];
        hpf_x1_[idx] = s;
        hpf_y1_[idx] = y;
        s = y;
      }
      peak_lin = std::max(peak_lin, std::abs(s));
      power_sum += s * s;
    }
    const float power_lin = power_sum * inv_channels;

    float level_db = kFloorDb;
    switch (cfg.detector) {
      case DetectorMode::Peak:
        level_db = linear_to_db(peak_lin);
        break;
      case DetectorMode::Rms:
        // Linear-domain RMS smoothing (10 ms window). Fast response, follows
        // transients more aggressively than LogRms.
        if (reseed_rms_state) {
          rms_state_ = power_lin;
          reseed_rms_state = false;
        }
        rms_state_ = rms_coeff_ * rms_state_ + (1.0f - rms_coeff_) * power_lin;
        level_db = linear_to_db(std::sqrt(std::max(rms_state_, 0.0f)));
        break;
      case DetectorMode::LogRms:
        // Slower RMS smoothing (50 ms window) for sustained-level estimation.
        // Used for "musical" compression that ignores brief transients.
        if (reseed_rms_state) {
          rms_state_ = power_lin;
          reseed_rms_state = false;
        }
        rms_state_ = log_rms_coeff_ * rms_state_ + (1.0f - log_rms_coeff_) * power_lin;
        level_db = linear_to_db(std::sqrt(std::max(rms_state_, 0.0f)));
        break;
    }

    const float target_db = gain_reduction_db(level_db, cfg);
    pdr_state_db_ = pdr_coeff_ * pdr_state_db_ + (1.0f - pdr_coeff_) * target_db;
    const float pdr_amount = cfg.pdr_time_ms > 0.0f
                                 ? std::clamp(-pdr_state_db_ / kPdrNormalizationDb, 0.0f, 1.0f)
                                 : 0.0f;
    const float release_position = pdr_amount * static_cast<float>(kReleaseTableSteps);
    const size_t release_index =
        std::min(static_cast<size_t>(release_position), kReleaseTableSteps - 1);
    const float release_fraction = release_position - static_cast<float>(release_index);
    const float release_coeff = release_coeff_table_[release_index] +
                                release_fraction * (release_coeff_table_[release_index + 1] -
                                                    release_coeff_table_[release_index]);
    const float reduction_state_db =
        reduction_smoother_.smooth_bidirectional(target_db, release_coeff, true);

    const float gain = db_to_linear(reduction_state_db + makeup_db);
    for (int ch = 0; ch < num_channels; ++ch) {
      channels[ch][i] *= gain;
    }
    max_reduction = std::min(max_reduction, reduction_state_db);
  }

  last_gain_reduction_db_ = max_reduction;
}

void Compressor::reset() {
  rms_state_ = 0.0f;
  detector_mode_initialized_ = false;
  std::fill(hpf_x1_.begin(), hpf_x1_.end(), 0.0f);
  std::fill(hpf_y1_.begin(), hpf_y1_.end(), 0.0f);
  pdr_state_db_ = 0.0f;
  reduction_smoother_.reset(0.0f);
  last_gain_reduction_db_ = 0.0f;
}

bool Compressor::set_parameter(unsigned int param_id, float value) {
  // RT-safe in-place automation: mutate the audio thread's live working config
  // and re-derive its coefficients. No shared_ptr publish, no allocation; the
  // control-thread mirror (config_) and the published snapshot stay untouched.
  switch (param_id) {
    case 0:
      active_.threshold_db = value;
      break;
    case 1:
      active_.ratio = std::max(1.0f, value);
      break;
    case 2:
      active_.attack_ms = std::max(0.0f, value);
      break;
    case 3:
      active_.release_ms = std::max(0.0f, value);
      break;
    case 4:
      active_.makeup_gain_db = value;
      break;
    case 5:
      active_.knee_db = std::max(0.0f, value);
      break;
    case 6:
      active_.auto_makeup = value != 0.0f;
      break;
    case 7:
      active_.detector = detector_mode_from_param(value);
      break;
    case 8:
      active_.sidechain_hpf_enabled = value != 0.0f;
      break;
    case 9:
      active_.sidechain_hpf_hz = std::max(1.0f, value);
      break;
    case 10:
      active_.pdr_time_ms = std::max(0.0f, value);
      break;
    case 11:
      active_.pdr_release_scale = std::max(1.0f, value);
      break;
    default:
      return false;
  }
  // Mirror the live value into the control-thread config so config() reads back
  // the automated state (matching the historical contract); this writes config_
  // only, never the published snapshot, so no allocation occurs. set_parameter
  // and set_config still must not run concurrently (single-producer contract).
  config_ = active_;
  update_coefficients(active_);
  return true;
}

std::vector<rt::ParamDescriptor> Compressor::parameter_descriptors() const {
  return {{"thresholdDb", 0},    {"ratio", 1},        {"attackMs", 2},
          {"releaseMs", 3},      {"makeupGainDb", 4}, {"kneeDb", 5},
          {"autoMakeup", 6},     {"detector", 7},     {"sidechainHpfEnabled", 8},
          {"sidechainHpfHz", 9}, {"pdrTimeMs", 10},   {"pdrReleaseScale", 11}};
}

void Compressor::validate_config(const CompressorConfig& config) {
  if (!(config.ratio >= 1.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "compressor ratio must be at least 1");
  }
  if (config.attack_ms < 0.0f || config.release_ms < 0.0f || config.knee_db < 0.0f ||
      config.sidechain_hpf_hz <= 0.0f || config.pdr_time_ms < 0.0f ||
      config.pdr_release_scale < 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "compressor timing and knee values must be non-negative");
  }
}

float Compressor::gain_reduction_db(float input_db, const CompressorConfig& config) {
  if (config.ratio <= 1.0f) {
    return 0.0f;
  }

  const float over_db = input_db - config.threshold_db;
  float compressed_over_db = 0.0f;
  if (config.knee_db > 0.0f) {
    const float half_knee = config.knee_db * 0.5f;
    if (over_db <= -half_knee) {
      compressed_over_db = 0.0f;
    } else if (over_db >= half_knee) {
      compressed_over_db = over_db * (1.0f - 1.0f / config.ratio);
    } else {
      const float x = over_db + half_knee;
      compressed_over_db = (1.0f - 1.0f / config.ratio) * x * x / (2.0f * config.knee_db);
    }
  } else if (over_db > 0.0f) {
    compressed_over_db = over_db * (1.0f - 1.0f / config.ratio);
  }

  return -compressed_over_db;
}

void Compressor::update_coefficients(const CompressorConfig& config) {
  reduction_smoother_.prepare(sample_rate_, config.attack_ms, config.release_ms);
  rms_coeff_ = time_to_coefficient(sample_rate_, kRmsWindowMs);
  log_rms_coeff_ = time_to_coefficient(sample_rate_, kLogRmsWindowMs);
  pdr_coeff_ = time_to_coefficient(sample_rate_, config.pdr_time_ms);
  update_release_table(config);
  // Bilinear-transformed 1st-order highpass with frequency prewarping. Same
  // 6 dB/oct slope as a 1-pole RC, but the cutoff is frequency-accurate.
  const auto hpf = sonare::rt::onepole_highpass_coeffs(static_cast<double>(config.sidechain_hpf_hz),
                                                       sample_rate_);
  hpf_b0_ = hpf.b0;
  hpf_a1_ = hpf.a1;
}

void Compressor::update_release_table(const CompressorConfig& config) noexcept {
  const float scale_range = std::max(config.pdr_release_scale - 1.0f, 0.0f);
  for (size_t index = 0; index <= kReleaseTableSteps; ++index) {
    const float amount = static_cast<float>(index) / static_cast<float>(kReleaseTableSteps);
    release_coeff_table_[index] =
        time_to_coefficient(sample_rate_, config.release_ms * (1.0f + amount * scale_range));
  }
}

}  // namespace sonare::mastering::dynamics
