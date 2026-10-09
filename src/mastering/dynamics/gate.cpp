#include "mastering/dynamics/gate.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "mastering/common/prepare_args.h"
#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"
#include "util/non_finite_state.h"

namespace sonare::mastering::dynamics {

using sonare::discard_group_if_non_finite;
using sonare::discard_if_non_finite;

// The configuration lifecycle (validate + seed active_ + publish the initial
// snapshot) is handled by RtConfigLifecycle's constructor.
Gate::Gate(GateConfig config) : ConfigBase(std::move(config)) {}

void Gate::prepare(double sample_rate, int max_block_size) {
  validate_prepare_args(sample_rate, max_block_size);

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
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
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
  const int excluded_channel = detector_excluded_channel(num_channels);
  last_gain_reduction_db_ = 0.0f;
  for (int i = 0; i < num_samples; ++i) {
    float detector = 0.0f;
    if (excluded_channel < 0) {
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
    } else {
      for (int ch = 0; ch < num_channels; ++ch) {
        if (ch == excluded_channel) continue;
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

  // Every cell carried between blocks, once per block. The gain rests open, at
  // unity.
  //
  // Never seen to fire: no sample drove this cell non-finite. The detector folds
  // with std::max, which drops a NaN, and an infinity reads as a level at or over
  // the threshold, which opens the gate onto a finite target -- so the smoother
  // only ever sees finite input. A configuration route is open (range_db is not
  // checked for finiteness) but that is a validation question and nothing here
  // covers it. A measured absence on the signals tried, not a proof of one.
  bool discarded = discard_if_non_finite(gain_, 1.0f);
  // The two taps of a one-pole section are meaningful only together. This pair
  // does strand: a non-finite tap makes the detector read silence from then on,
  // so the gate stays shut for the rest of the handle with every output sample
  // finite -- which is why the test reads a value rather than finiteness.
  for (size_t ch = 0; ch < hpf_x1_.size(); ++ch) {
    discarded |= discard_group_if_non_finite(hpf_x1_[ch], hpf_y1_[ch]);
  }
  if (discarded) note_non_finite_discard();
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

bool Gate::apply_parameter(GateConfig& config, unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config.threshold_db = value;
      // Keep the hysteresis invariant close_threshold_db <= threshold_db.
      config.close_threshold_db = std::min(config.close_threshold_db, config.threshold_db);
      break;
    case 1:
      config.attack_ms = std::max(0.0f, value);
      break;
    case 2:
      config.release_ms = std::max(0.0f, value);
      break;
    case 3:
      config.range_db = std::min(0.0f, value);
      break;
    default:
      return false;
  }
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
