#include "mastering/dynamics/gate.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

#include "mastering/common/biquad_design.h"
#include "mastering/common/scoped_no_denormals.h"
#include "util/db.h"
#include "util/dsp_primitives.h"

namespace sonare::mastering::dynamics {

Gate::Gate(GateConfig config)
    : config_(config), config_publisher_(std::make_unique<rt::RtPublisher<GateConfig>>()) {
  validate_config(config_);
  // Seed the publisher so a downstream audio thread that starts before
  // prepare() sees a defined snapshot. prepare() will publish again with
  // post-prepare derived state already applied so the first audio block does
  // not redundantly recompute coefficients.
  config_publisher_->publish(std::make_shared<const GateConfig>(config_));
}

void Gate::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw std::invalid_argument("sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw std::invalid_argument("max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  update_coefficients(config_);
  prepared_ = true;
  hpf_x1_.assign(kRealtimePreparedChannels, 0.0f);
  hpf_y1_.assign(kRealtimePreparedChannels, 0.0f);
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  auto fresh = std::make_shared<const GateConfig>(config_);
  applied_snapshot_ = fresh.get();
  config_publisher_->publish(std::move(fresh));
  config_publisher_->acquire();
}

const GateConfig* Gate::adopt_snapshot_for_block() noexcept {
  // Audio-thread entry. acquire() drains the publish ring to the newest
  // snapshot and retires superseded ones via the wait-free retire ring (no
  // alloc, no free, no lock on this thread). If a new snapshot was adopted,
  // re-derive the scalar coefficients — those writes target members the per-
  // sample loop reads, but the loop has not started yet for this block, so no
  // race.
  config_publisher_->acquire();
  const GateConfig* current = config_publisher_->current();
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

void Gate::process(float* const* channels, int num_channels, int num_samples) {
  sonare::mastering::common::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Gate");
  if (num_channels < 0 || num_samples < 0) throw std::invalid_argument("invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr) throw std::invalid_argument("channels must not be null");
  if (static_cast<size_t>(num_channels) > hpf_x1_.size() ||
      static_cast<size_t>(num_channels) > hpf_y1_.size()) {
    throw std::invalid_argument("num_channels exceeds prepared Gate state");
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
      if (channels[ch] == nullptr) throw std::invalid_argument("channel buffer must not be null");
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
    const float target_db = open ? 0.0f : cfg.range_db;
    const float c = target_db > gain_db_ ? attack : release;
    gain_db_ = c * gain_db_ + (1.0f - c) * target_db;
    const float gain = db_to_linear(gain_db_);
    for (int ch = 0; ch < num_channels; ++ch) channels[ch][i] *= gain;
    last_gain_reduction_db_ = std::min(last_gain_reduction_db_, gain_db_);
  }
}

void Gate::reset() {
  gain_db_ = 0.0f;
  last_gain_reduction_db_ = 0.0f;
  hold_samples_remaining_ = 0;
  gate_open_ = false;
  std::fill(hpf_x1_.begin(), hpf_x1_.end(), 0.0f);
  std::fill(hpf_y1_.begin(), hpf_y1_.end(), 0.0f);
}

void Gate::set_config(const GateConfig& config) {
  // Control-thread side: validate before publishing so any throw leaves both
  // the control-thread mirror (config_) and the audio-thread snapshot
  // unchanged. The audio thread sees the new snapshot only after publish()
  // succeeds; validation never runs partway through a config_ write that the
  // audio thread could observe.
  validate_config(config);
  config_ = config;
  config_publisher_->publish(std::make_shared<const GateConfig>(config_));
}

bool Gate::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.threshold_db = value;
      // Keep the hysteresis invariant close_threshold_db <= threshold_db.
      config_.close_threshold_db = std::min(config_.close_threshold_db, config_.threshold_db);
      break;
    case 1:
      config_.attack_ms = std::max(0.0f, value);
      break;
    case 2:
      config_.release_ms = std::max(0.0f, value);
      break;
    case 3:
      config_.range_db = std::min(0.0f, value);
      break;
    default:
      return false;
  }
  config_publisher_->publish(std::make_shared<const GateConfig>(config_));
  return true;
}

void Gate::validate_config(const GateConfig& config) {
  if (config.attack_ms < 0.0f || config.release_ms < 0.0f || config.range_db > 0.0f ||
      config.hold_ms < 0.0f || config.key_hpf_hz < 0.0f ||
      config.close_threshold_db > config.threshold_db) {
    throw std::invalid_argument("invalid gate configuration");
  }
}

void Gate::update_coefficients(const GateConfig& config) {
  // Bilinear-transformed 1st-order highpass with frequency prewarping. Only
  // recompute when the key HPF is enabled; the bypass path keeps b0=1, a1=0
  // which is a passthrough so leaving them at the default is safe.
  if (config.key_hpf_hz > 0.0f) {
    const auto hpf =
        common::onepole_highpass_coeffs(static_cast<double>(config.key_hpf_hz), sample_rate_);
    hpf_b0_ = hpf.b0;
    hpf_a1_ = hpf.a1;
  } else {
    hpf_b0_ = 1.0f;
    hpf_a1_ = 0.0f;
  }
}

}  // namespace sonare::mastering::dynamics
