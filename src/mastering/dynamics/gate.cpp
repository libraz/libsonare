#include "mastering/dynamics/gate.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare::mastering::dynamics {

// The configuration lifecycle (validate + seed active_ + publish the initial
// snapshot) is handled by RtConfigLifecycle's constructor.
Gate::Gate(GateConfig config) : ConfigBase(std::move(config)) {}

void Gate::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  // Seed the audio thread's live working config from the prepared baseline and
  // derive its coefficients; the per-sample loop reads active_, not config_.
  active_ = config_;
  update_coefficients(active_);
  prepared_ = true;
  hpf_x1_.assign(kRealtimePreparedChannels, 0.0f);
  hpf_y1_.assign(kRealtimePreparedChannels, 0.0f);
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  republish_after_prepare();
}

void Gate::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Gate");
  if (num_channels < 0 || num_samples < 0)
    throw SonareException(ErrorCode::InvalidParameter, "invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  if (static_cast<size_t>(num_channels) > hpf_x1_.size() ||
      static_cast<size_t>(num_channels) > hpf_y1_.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "num_channels exceeds prepared Gate state");
  }

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const GateConfig& cfg = *adopt_snapshot_for_block();

  const float attack = time_to_coefficient(sample_rate_, cfg.attack_ms);
  const float release = time_to_coefficient(sample_rate_, cfg.release_ms);
  const int hold_samples =
      static_cast<int>(sample_rate_ * static_cast<double>(cfg.hold_ms) * 0.001);
  last_gain_reduction_db_ = 0.0f;
  for (int i = 0; i < num_samples; ++i) {
    float detector = 0.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr)
        throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
      float s = channels[ch][i];
      if (cfg.key_hpf_hz > 0.0f) {
        const auto idx = static_cast<size_t>(ch);
        const float y = hpf_b0_ * (s - hpf_x1_[idx]) + hpf_a1_ * hpf_y1_[idx];
        hpf_x1_[idx] = s;
        hpf_y1_[idx] = y;
        s = y;
      }
      detector = std::max(detector, std::abs(s));
    }
    const float level_db = linear_to_db(detector);
    if (level_db >= cfg.threshold_db) {
      hold_samples_remaining_ = hold_samples;
    } else if (hold_samples_remaining_ > 0) {
      --hold_samples_remaining_;
    }
    if (level_db >= cfg.threshold_db) {
      gate_open_ = true;
    } else if (hold_samples_remaining_ == 0 && level_db < cfg.close_threshold_db) {
      gate_open_ = false;
    }
    const bool open = gate_open_ || hold_samples_remaining_ > 0;
    // Smooth the gain in the linear (0..1) domain. dB-domain smoothing toward an
    // open target of 0 dB starting from range_db never converges cleanly (and
    // db_to_linear(-inf) underflows for a fully-closed range), producing an
    // unnatural opening. range_db == -inf maps to a linear floor of 0.
    const float target_gain = open ? 1.0f : db_to_linear(cfg.range_db);
    const float c = target_gain > gain_ ? attack : release;
    gain_ = c * gain_ + (1.0f - c) * target_gain;
    const float gain = gain_;
    for (int ch = 0; ch < num_channels; ++ch) channels[ch][i] *= gain;
    last_gain_reduction_db_ = std::min(last_gain_reduction_db_, linear_to_db(gain_));
  }
}

void Gate::reset() {
  // Linear gain; 1.0 == unity == fully open (matches the previous 0 dB seed).
  gain_ = 1.0f;
  last_gain_reduction_db_ = 0.0f;
  hold_samples_remaining_ = 0;
  gate_open_ = false;
  std::fill(hpf_x1_.begin(), hpf_x1_.end(), 0.0f);
  std::fill(hpf_y1_.begin(), hpf_y1_.end(), 0.0f);
}

bool Gate::set_parameter(unsigned int param_id, float value) {
  // RT-safe in-place automation: mutate the audio thread's live working config
  // and re-derive its coefficients. No shared_ptr publish, no allocation; the
  // control-thread mirror (config_) and the published snapshot stay untouched.
  switch (param_id) {
    case 0:
      active_.threshold_db = value;
      // Keep the hysteresis invariant close_threshold_db <= threshold_db.
      active_.close_threshold_db = std::min(active_.close_threshold_db, active_.threshold_db);
      break;
    case 1:
      active_.attack_ms = std::max(0.0f, value);
      break;
    case 2:
      active_.release_ms = std::max(0.0f, value);
      break;
    case 3:
      active_.range_db = std::min(0.0f, value);
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

std::vector<rt::ParamDescriptor> Gate::parameter_descriptors() const {
  return {{"thresholdDb", 0}, {"attackMs", 1}, {"releaseMs", 2}, {"rangeDb", 3}};
}

void Gate::validate_config(const GateConfig& config) {
  if (config.attack_ms < 0.0f || config.release_ms < 0.0f || config.range_db > 0.0f ||
      config.hold_ms < 0.0f || config.key_hpf_hz < 0.0f ||
      config.close_threshold_db > config.threshold_db) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid gate configuration");
  }
}

void Gate::update_coefficients(const GateConfig& config) {
  // Bilinear-transformed 1st-order highpass with frequency prewarping. Only
  // recompute when the key HPF is enabled; the bypass path keeps b0=1, a1=0
  // which is a passthrough so leaving them at the default is safe.
  if (config.key_hpf_hz > 0.0f) {
    const auto hpf =
        sonare::rt::onepole_highpass_coeffs(static_cast<double>(config.key_hpf_hz), sample_rate_);
    hpf_b0_ = hpf.b0;
    hpf_a1_ = hpf.a1;
  } else {
    hpf_b0_ = 1.0f;
    hpf_a1_ = 0.0f;
  }
}

}  // namespace sonare::mastering::dynamics
