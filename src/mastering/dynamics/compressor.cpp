#include "mastering/dynamics/compressor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mastering/common/biquad_design.h"
#include "mastering/common/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"

namespace sonare::mastering::dynamics {

namespace {

using sonare::constants::kFloorDb;

constexpr float kRmsWindowMs = 10.0f;
constexpr float kLogRmsWindowMs = 50.0f;

// Fraction of the theoretical full makeup gain applied by the auto-makeup
// heuristic. The full static makeup that exactly restores the pre-compression
// level of a signal sitting at the threshold is
// (-threshold_db) * (1 - 1/ratio); applying all of it tends to overshoot on
// real program material because the average level is well below threshold, so
// we apply half of it as a conservative perceptual compromise.
constexpr float kAutoMakeupFraction = 0.5f;

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

Compressor::Compressor(CompressorConfig config) : config_(config) { validate_config(config_); }

void Compressor::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }
  sample_rate_ = sample_rate;
  prepared_ = true;
  update_coefficients();
  hpf_x1_.assign(2, 0.0f);
  hpf_y1_.assign(2, 0.0f);
  reset();
}

void Compressor::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  if (!prepared_) {
    throw std::logic_error("Compressor must be prepared before processing");
  }
  if (num_channels < 0 || num_samples < 0) {
    throw std::invalid_argument("num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw std::invalid_argument("channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw std::invalid_argument("channel buffer must not be null");
    }
  }

  const float makeup_db = compute_makeup_db(config_);

  if (hpf_x1_.size() != static_cast<size_t>(num_channels)) {
    hpf_x1_.assign(static_cast<size_t>(num_channels), 0.0f);
    hpf_y1_.assign(static_cast<size_t>(num_channels), 0.0f);
  }

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
      if (config_.sidechain_hpf_enabled) {
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
    switch (config_.detector) {
      case DetectorMode::Peak:
        level_db = linear_to_db(peak_lin);
        break;
      case DetectorMode::Rms:
        // Linear-domain RMS smoothing (10 ms window). Fast response, follows
        // transients more aggressively than LogRms.
        rms_state_ = rms_coeff_ * rms_state_ + (1.0f - rms_coeff_) * power_lin;
        level_db = linear_to_db(std::sqrt(std::max(rms_state_, 0.0f)));
        break;
      case DetectorMode::LogRms:
        // Slower RMS smoothing (50 ms window) for sustained-level estimation.
        // Used for "musical" compression that ignores brief transients.
        rms_state_ = log_rms_coeff_ * rms_state_ + (1.0f - log_rms_coeff_) * power_lin;
        level_db = linear_to_db(std::sqrt(std::max(rms_state_, 0.0f)));
        break;
    }

    const float target_db = gain_reduction_db(level_db, config_);
    pdr_state_db_ = pdr_coeff_ * pdr_state_db_ + (1.0f - pdr_coeff_) * target_db;
    const float pdr_amount =
        config_.pdr_time_ms > 0.0f ? std::clamp(-pdr_state_db_ / 24.0f, 0.0f, 1.0f) : 0.0f;
    const float release_coeff = time_to_coefficient(
        sample_rate_, config_.release_ms *
                          (1.0f + pdr_amount * std::max(config_.pdr_release_scale - 1.0f, 0.0f)));
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
  std::fill(hpf_x1_.begin(), hpf_x1_.end(), 0.0f);
  std::fill(hpf_y1_.begin(), hpf_y1_.end(), 0.0f);
  pdr_state_db_ = 0.0f;
  reduction_smoother_.reset(0.0f);
  last_gain_reduction_db_ = 0.0f;
}

void Compressor::set_config(const CompressorConfig& config) {
  validate_config(config);
  config_ = config;
  if (prepared_) {
    update_coefficients();
    reset();
  }
}

bool Compressor::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.threshold_db = value;
      return true;
    case 1:
      config_.ratio = std::max(1.0f, value);
      return true;
    case 2:
      config_.attack_ms = std::max(0.0f, value);
      // Recompute smoother coefficients in place; preserves envelope state.
      if (prepared_) {
        reduction_smoother_.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
      }
      return true;
    case 3:
      config_.release_ms = std::max(0.0f, value);
      if (prepared_) {
        reduction_smoother_.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
      }
      return true;
    case 4:
      config_.makeup_gain_db = value;
      return true;
    default:
      return false;
  }
}

void Compressor::validate_config(const CompressorConfig& config) {
  if (!(config.ratio >= 1.0f)) {
    throw std::invalid_argument("compressor ratio must be at least 1");
  }
  if (config.attack_ms < 0.0f || config.release_ms < 0.0f || config.knee_db < 0.0f ||
      config.sidechain_hpf_hz <= 0.0f || config.pdr_time_ms < 0.0f ||
      config.pdr_release_scale < 1.0f) {
    throw std::invalid_argument("compressor timing and knee values must be non-negative");
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

void Compressor::update_coefficients() {
  reduction_smoother_.prepare(sample_rate_, config_.attack_ms, config_.release_ms);
  rms_coeff_ = time_to_coefficient(sample_rate_, kRmsWindowMs);
  log_rms_coeff_ = time_to_coefficient(sample_rate_, kLogRmsWindowMs);
  pdr_coeff_ = time_to_coefficient(sample_rate_, config_.pdr_time_ms);
  // Bilinear-transformed 1st-order highpass with frequency prewarping. Same
  // 6 dB/oct slope as a 1-pole RC, but the cutoff is frequency-accurate.
  const auto hpf =
      common::onepole_highpass_coeffs(static_cast<double>(config_.sidechain_hpf_hz), sample_rate_);
  hpf_b0_ = hpf.b0;
  hpf_a1_ = hpf.a1;
}

}  // namespace sonare::mastering::dynamics
