#include "mastering/dynamics/gate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/detector_level.h"
#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"
#include "util/non_finite_state.h"

namespace sonare::mastering::dynamics {

using sonare::discard_group_if_non_finite;
using sonare::discard_if_non_finite;

namespace {

// Field domains shared by validate_config and live automation.
bool valid_time_ms(float v) noexcept { return std::isfinite(v) && v >= 0.0f; }
bool valid_range_db(float v) noexcept {
  return (std::isfinite(v) || v == -std::numeric_limits<float>::infinity()) && v <= 0.0f;
}

}  // namespace

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
  hpf_channel_gates_.assign(kRealtimePreparedChannels, rt::StageGate{});
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
  require_prepared_channels(
      num_channels, std::min({hpf_x1_.size(), hpf_y1_.size(), hpf_channel_gates_.size()}), "Gate");

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const GateConfig& cfg = *adopt_snapshot_for_block();
  const bool key_hpf_enabled = key_hpf_gate_.admit(cfg.key_hpf_hz > 0.0f, [this] {
    std::fill(hpf_x1_.begin(), hpf_x1_.end(), 0.0f);
    std::fill(hpf_y1_.begin(), hpf_y1_.end(), 0.0f);
  });

  const double hold_samples_value = sample_rate_ * static_cast<double>(cfg.hold_ms) * 0.001;
  const int hold_samples = hold_samples_value > static_cast<double>(std::numeric_limits<int>::max())
                               ? std::numeric_limits<int>::max()
                               : static_cast<int>(hold_samples_value);
  const int excluded_channel = detector_excluded_channel(num_channels);
  std::array<bool, kRealtimePreparedChannels> hpf_admitted{};
  for (size_t ch = 0; ch < hpf_channel_gates_.size(); ++ch) {
    const bool active = key_hpf_enabled && ch < static_cast<size_t>(num_channels) &&
                        static_cast<int>(ch) != excluded_channel;
    hpf_admitted[ch] = hpf_channel_gates_[ch].admit(active, [this, ch] {
      hpf_x1_[ch] = 0.0f;
      hpf_y1_[ch] = 0.0f;
    });
  }
  const float range_gain = db_to_linear(cfg.range_db);
  // Metering converts once after the loop. linear_to_db is monotone on positive
  // gains but floors at kFloorDb for gain <= 0, so the positive minimum and the
  // floor case are tracked apart to match a per-sample min exactly.
  float min_positive_gain = std::numeric_limits<float>::infinity();
  bool saw_non_positive_gain = false;
  for (int i = 0; i < num_samples; ++i) {
    float detector = 0.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      if (ch == excluded_channel) continue;
      float s = channels[ch][i];
      if (hpf_admitted[static_cast<size_t>(ch)]) {
        const auto idx = static_cast<size_t>(ch);
        s = rt::onepole_highpass_step(hpf_b0_, hpf_a1_, hpf_x1_[idx], hpf_y1_[idx], s);
      }
      detector = std::max(detector, std::abs(s));
    }
    const float level_db = detector_level_db(detector);
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
    const float target_gain = open ? 1.0f : range_gain;
    const float c = target_gain > gain_ ? attack_coeff_ : release_coeff_;
    gain_ = c * gain_ + (1.0f - c) * target_gain;
    const float gain = gain_;
    for (int ch = 0; ch < num_channels; ++ch) channels[ch][i] *= gain;
    if (gain_ > 0.0f) {
      min_positive_gain = std::min(min_positive_gain, gain_);
    } else if (gain_ <= 0.0f) {
      saw_non_positive_gain = true;
    }
  }
  float min_reduction_db = 0.0f;
  if (min_positive_gain != std::numeric_limits<float>::infinity()) {
    min_reduction_db = std::min(min_reduction_db, linear_to_db(min_positive_gain));
  }
  if (saw_non_positive_gain) {
    min_reduction_db = std::min(min_reduction_db, linear_to_db(0.0f));
  }
  last_gain_reduction_db_ = min_reduction_db;

  // Every cell carried between blocks, once per block. The gain rests open, at
  // unity.
  //
  // Never seen to fire: no sample drove this cell non-finite. The detector folds
  // with std::max, which drops a NaN, and an infinity reads as a level at or over
  // the threshold, which opens the gate onto a finite target -- so the smoother
  // only ever sees finite input. Configuration validation requires every scalar
  // to be finite except for the intentional range_db == -inf floor. A measured
  // absence on the signals tried, not a proof of one.
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
  key_hpf_gate_.close();
  for (auto& gate : hpf_channel_gates_) gate.close();
  std::fill(hpf_x1_.begin(), hpf_x1_.end(), 0.0f);
  std::fill(hpf_y1_.begin(), hpf_y1_.end(), 0.0f);
}

bool Gate::apply_parameter(GateConfig& config, unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      if (!std::isfinite(value)) return false;
      config.threshold_db = value;
      // close_threshold_db is not automatable, so it follows the threshold down.
      config.close_threshold_db = std::min(config.close_threshold_db, config.threshold_db);
      break;
    case 1:
      if (!valid_time_ms(value)) return false;
      config.attack_ms = value;
      break;
    case 2:
      if (!valid_time_ms(value)) return false;
      config.release_ms = value;
      break;
    case 3:
      if (!valid_range_db(value)) return false;
      config.range_db = value;
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
  if (!std::isfinite(config.threshold_db) || !std::isfinite(config.attack_ms) ||
      !std::isfinite(config.release_ms) || !std::isfinite(config.hold_ms) ||
      !std::isfinite(config.close_threshold_db) || !std::isfinite(config.key_hpf_hz) ||
      !valid_range_db(config.range_db) || !valid_time_ms(config.attack_ms) ||
      !valid_time_ms(config.release_ms) || config.hold_ms < 0.0f || config.key_hpf_hz < 0.0f ||
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
  attack_coeff_ = time_to_coefficient(sample_rate_, config.attack_ms);
  release_coeff_ = time_to_coefficient(sample_rate_, config.release_ms);
}

}  // namespace sonare::mastering::dynamics
