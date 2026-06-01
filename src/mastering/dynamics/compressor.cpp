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

Compressor::Compressor(CompressorConfig config)
    : config_(config), config_publisher_(std::make_unique<rt::RtPublisher<CompressorConfig>>()) {
  validate_config(config_);
  // Seed the publisher so a downstream audio thread that starts before
  // prepare() sees a defined snapshot. prepare() will publish again with
  // post-prepare derived state already applied so the first audio block does
  // not redundantly recompute coefficients.
  config_publisher_->publish(std::make_shared<const CompressorConfig>(config_));
}

void Compressor::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  sample_rate_ = sample_rate;
  prepared_ = true;
  update_coefficients(config_);
  hpf_x1_.assign(kRealtimePreparedChannels, 0.0f);
  hpf_y1_.assign(kRealtimePreparedChannels, 0.0f);
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  auto fresh = std::make_shared<const CompressorConfig>(config_);
  applied_snapshot_ = fresh.get();
  config_publisher_->publish(std::move(fresh));
  config_publisher_->acquire();
}

const CompressorConfig* Compressor::adopt_snapshot_for_block() noexcept {
  // Audio-thread entry. acquire() drains the publish ring to the newest
  // snapshot and retires superseded ones via the wait-free retire ring (no
  // alloc, no free, no lock on this thread). If a new snapshot was adopted,
  // re-derive the scalar coefficients — those writes target members the per-
  // sample loop reads, but the loop has not started yet for this block, so no
  // race.
  config_publisher_->acquire();
  const CompressorConfig* current = config_publisher_->current();
  if (current && current != applied_snapshot_) {
    update_coefficients(*current);
    applied_snapshot_ = current;
  }
  // Fallback path: only reachable if the constructor's initial publish was
  // dropped (ring full, which cannot happen on a fresh publisher) AND prepare
  // was never called. In that case use the control-thread mirror; the per-
  // sample loop is itself guarded by prepared_ so this path stays defined.
  return current ? current : &config_;
}

void Compressor::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Compressor");
  if (num_channels < 0 || num_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels and num_samples must be non-negative");
  }
  if (num_channels == 0 || num_samples == 0) {
    return;
  }
  if (channels == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
    }
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

    const float target_db = gain_reduction_db(level_db, cfg);
    pdr_state_db_ = pdr_coeff_ * pdr_state_db_ + (1.0f - pdr_coeff_) * target_db;
    const float pdr_amount =
        cfg.pdr_time_ms > 0.0f ? std::clamp(-pdr_state_db_ / 24.0f, 0.0f, 1.0f) : 0.0f;
    const float release_coeff = time_to_coefficient(
        sample_rate_,
        cfg.release_ms * (1.0f + pdr_amount * std::max(cfg.pdr_release_scale - 1.0f, 0.0f)));
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
  // Control-thread side: validate before publishing so any throw leaves both
  // the control-thread mirror (config_) and the audio-thread snapshot
  // unchanged. The audio thread sees the new snapshot only after publish()
  // succeeds; validation never runs partway through a config_ write that the
  // audio thread could observe.
  validate_config(config);
  config_ = config;
  config_publisher_->publish(std::make_shared<const CompressorConfig>(config_));
}

bool Compressor::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.threshold_db = value;
      break;
    case 1:
      config_.ratio = std::max(1.0f, value);
      break;
    case 2:
      config_.attack_ms = std::max(0.0f, value);
      break;
    case 3:
      config_.release_ms = std::max(0.0f, value);
      break;
    case 4:
      config_.makeup_gain_db = value;
      break;
    default:
      return false;
  }
  config_publisher_->publish(std::make_shared<const CompressorConfig>(config_));
  return true;
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
  // Bilinear-transformed 1st-order highpass with frequency prewarping. Same
  // 6 dB/oct slope as a 1-pole RC, but the cutoff is frequency-accurate.
  const auto hpf = sonare::rt::onepole_highpass_coeffs(static_cast<double>(config.sidechain_hpf_hz),
                                                       sample_rate_);
  hpf_b0_ = hpf.b0;
  hpf_a1_ = hpf.a1;
}

}  // namespace sonare::mastering::dynamics
