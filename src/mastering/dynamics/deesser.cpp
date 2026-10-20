#include "mastering/dynamics/deesser.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/detector_level.h"
#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "rt/tail_budget.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/non_finite_state.h"

namespace sonare::mastering::dynamics {

namespace {

using sonare::discard_group_if_non_finite;
using sonare::constants::kPiD;

// Field domains shared by validate_config and live automation.
bool valid_positive(float v) noexcept { return std::isfinite(v) && v > 0.0f; }
bool valid_ratio(float v) noexcept { return std::isfinite(v) && v >= 1.0f; }
bool valid_non_negative(float v) noexcept { return std::isfinite(v) && v >= 0.0f; }

}  // namespace

// The configuration lifecycle (validate + seed active_ + publish the initial
// snapshot) is handled by RtConfigLifecycle's constructor.
DeEsser::DeEsser(DeEsserConfig config) : ConfigBase(std::move(config)) {}

void DeEsser::prepare(double sample_rate, int max_block_size) {
  validate_prepare_args(sample_rate, max_block_size);

  sample_rate_ = sample_rate;
  prepared_ = true;
  active_ = config_;
  if (bandpass_.size() < kRealtimePreparedChannels) {
    bandpass_.resize(kRealtimePreparedChannels, filter_coeffs_);
  }
  if (bandpass2_.size() < kRealtimePreparedChannels) {
    bandpass2_.resize(kRealtimePreparedChannels, filter_coeffs_);
  }
  if (followers_.size() < kRealtimePreparedChannels) {
    followers_.resize(kRealtimePreparedChannels);
  }
  update_coefficients(active_);
  reset();
  // Re-publish so the audio thread observes the same snapshot that prepare()
  // already applied; adopt_snapshot_for_block() skips the redundant
  // recomputation when current() == applied_snapshot_.
  republish_after_prepare();
}

void DeEsser::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "DeEsser");
  if (!validate_process_buffers(channels, num_channels, num_samples)) {
    return;
  }

  ensure_state(num_channels);

  // Adopt the latest published configuration once per block. The returned
  // pointer is stable for the entire per-sample loop — RtPublisher only
  // changes its current() value inside acquire(), and we already called it.
  const DeEsserConfig& cfg = *adopt_snapshot_for_block();

  float max_reduction = 0.0f;
  bool discarded = false;
  for (int ch = 0; ch < num_channels; ++ch) {
    auto& filter = bandpass_[static_cast<size_t>(ch)];
    auto& filter2 = bandpass2_[static_cast<size_t>(ch)];
    auto& follower = followers_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      const float input = channels[ch][i];
      // The cascaded bandpass isolates the sibilant band for both detection AND
      // the actual split: the reduction is applied only to that band, then the
      // attenuated band is recombined with the untouched full-band signal. This
      // avoids the pumping / broadband (incl. low-frequency) attenuation that
      // results from multiplying the full input by a band-derived gain. The
      // detection BP is reused as the split filter; this is an acceptable
      // approximation for a split-band de-esser of this design.
      const float sibilant = filter2.process(filter.process(input));
      const float envelope = follower.process(sibilant);
      const float reduction_db = gain_reduction_db(detector_level_db(envelope), cfg);
      const float linear_reduction = db_to_linear(reduction_db);
      // out = dry full-band minus the attenuated portion of the detected band.
      channels[ch][i] = input + sibilant * (linear_reduction - 1.0f);
      max_reduction = std::min(max_reduction, reduction_db);
    }
    // Five floats per channel, once per block. The two bandpass sections feed
    // the follower, so one non-finite section makes the whole detector unusable.
    discarded |= discard_group_if_non_finite(filter.z1, filter.z2, filter2.z1, filter2.z2);
    discarded |= follower.discard_if_non_finite();
  }
  if (discarded) note_non_finite_discard();

  last_gain_reduction_db_ = max_reduction;
  minimum_gain_reduction_db_ = std::min(minimum_gain_reduction_db_, last_gain_reduction_db_);
}

int DeEsser::tail_samples() const noexcept {
  // The attenuated band leaves the two cascaded band-pass sections after the input stops.
  rt::TailBudget tail;
  return tail.section(filter_coeffs_.c).section(filter_coeffs_.c).samples();
}

void DeEsser::reset() {
  for (auto& filter : bandpass_) filter.reset();
  for (auto& filter : bandpass2_) filter.reset();
  for (auto& follower : followers_) {
    follower.reset();
  }
  last_gain_reduction_db_ = 0.0f;
  minimum_gain_reduction_db_ = 0.0f;
}

bool DeEsser::apply_parameter(DeEsserConfig& config, unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      // update_coefficients clamps the effective cutoff and preserves filter state.
      if (!valid_positive(value)) return false;
      config.frequency_hz = value;
      break;
    case 1:
      config.threshold_db = value;
      break;
    case 2:
      if (!valid_ratio(value)) return false;
      config.ratio = value;
      break;
    case 3:
      if (!valid_non_negative(value)) return false;
      config.attack_ms = value;
      break;
    case 4:
      if (!valid_non_negative(value)) return false;
      config.release_ms = value;
      break;
    case 5:
      if (!valid_non_negative(value)) return false;
      config.range_db = value;
      break;
    case 6:
      if (!valid_positive(value)) return false;
      config.bandpass_q = value;
      break;
    default:
      return false;
  }
  return true;
}

std::vector<rt::ParamDescriptor> DeEsser::parameter_descriptors() const {
  return {{"frequencyHz", 0}, {"thresholdDb", 1}, {"ratio", 2},    {"attackMs", 3},
          {"releaseMs", 4},   {"rangeDb", 5},     {"bandpassQ", 6}};
}

void DeEsser::validate_config(const DeEsserConfig& config) {
  if (!std::isfinite(config.threshold_db) || !valid_positive(config.frequency_hz) ||
      !valid_ratio(config.ratio) || !valid_non_negative(config.attack_ms) ||
      !valid_non_negative(config.release_ms) || !valid_non_negative(config.range_db) ||
      !valid_positive(config.bandpass_q)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid de-esser configuration");
  }
}

float DeEsser::gain_reduction_db(float input_db, const DeEsserConfig& config) {
  if (input_db <= config.threshold_db || config.ratio <= 1.0f) {
    return 0.0f;
  }

  const float over_db = input_db - config.threshold_db;
  const float reduction = over_db * (1.0f - 1.0f / config.ratio);
  return -std::min(config.range_db, reduction);
}

void DeEsser::ensure_state(int num_channels) {
  require_prepared_channels(
      num_channels, std::min({followers_.size(), bandpass_.size(), bandpass2_.size()}), "DeEsser");
}

void DeEsser::update_coefficients(const DeEsserConfig& config) {
  const float nyquist = static_cast<float>(sample_rate_ * 0.5);
  const float cutoff = std::clamp(config.frequency_hz, 10.0f, nyquist * 0.98f);
  const float q = std::max(1.0e-3f, config.bandpass_q);
  const float w0 = static_cast<float>(2.0 * kPiD * cutoff / sample_rate_);
  filter_coeffs_.c = rt::rbj_bandpass(w0, q);
  for (auto& filter : bandpass_) {
    filter.c = filter_coeffs_.c;
  }
  for (auto& filter : bandpass2_) {
    filter.c = filter_coeffs_.c;
  }
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config.attack_ms, config.release_ms);
  }
}

}  // namespace sonare::mastering::dynamics
