#include "mastering/dynamics/deesser.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::dynamics {

namespace {

using sonare::constants::kPiD;

}  // namespace

// The configuration lifecycle (validate + seed active_ + publish the initial
// snapshot) is handled by RtConfigLifecycle's constructor.
DeEsser::DeEsser(DeEsserConfig config) : ConfigBase(std::move(config)) {}

void DeEsser::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }

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
      const float reduction_db = gain_reduction_db(linear_to_db(envelope), cfg);
      const float linear_reduction = db_to_linear(reduction_db);
      // out = dry full-band minus the attenuated portion of the detected band.
      channels[ch][i] = input + sibilant * (linear_reduction - 1.0f);
      max_reduction = std::min(max_reduction, reduction_db);
    }
  }

  last_gain_reduction_db_ = max_reduction;
}

void DeEsser::reset() {
  for (auto& filter : bandpass_) filter.reset();
  for (auto& filter : bandpass2_) filter.reset();
  for (auto& follower : followers_) {
    follower.reset();
  }
  last_gain_reduction_db_ = 0.0f;
}

bool DeEsser::set_parameter(unsigned int param_id, float value) {
  // RT-safe in-place automation: mutate the audio thread's working config and
  // re-derive coefficients. No shared_ptr publish, no allocation; the published
  // snapshot stays untouched and the control-thread mirror (config_) is updated
  // so config() reads back the automated state. set_parameter and set_config
  // must not run concurrently (single-producer contract).
  switch (param_id) {
    case 0:
      // Keep frequency positive (validate_config invariant); update_coefficients
      // clamps the effective cutoff to a valid range and preserves filter state.
      active_.frequency_hz = std::max(value, 1.0f);
      break;
    case 1:
      active_.threshold_db = value;
      break;
    case 2:
      active_.ratio = std::max(1.0f, value);
      break;
    case 3:
      active_.attack_ms = std::max(0.0f, value);
      break;
    case 4:
      active_.release_ms = std::max(0.0f, value);
      break;
    case 5:
      active_.range_db = std::max(0.0f, value);
      break;
    case 6:
      active_.bandpass_q = std::max(1.0e-3f, value);
      break;
    default:
      return false;
  }
  update_coefficients(active_);
  config_ = active_;
  return true;
}

std::vector<rt::ParamDescriptor> DeEsser::parameter_descriptors() const {
  return {{"frequencyHz", 0}, {"thresholdDb", 1}, {"ratio", 2},    {"attackMs", 3},
          {"releaseMs", 4},   {"rangeDb", 5},     {"bandpassQ", 6}};
}

void DeEsser::validate_config(const DeEsserConfig& config) {
  if (!(config.frequency_hz > 0.0f) || !(config.ratio >= 1.0f) || config.attack_ms < 0.0f ||
      config.release_ms < 0.0f || config.range_db < 0.0f || !(config.bandpass_q > 0.0f)) {
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
  const auto target_size = static_cast<size_t>(num_channels);
  if (followers_.size() >= target_size && bandpass_.size() >= target_size &&
      bandpass2_.size() >= target_size) {
    return;
  }

  throw SonareException(ErrorCode::InvalidParameter, "num_channels exceeds prepared DeEsser state");
}

void DeEsser::update_coefficients(const DeEsserConfig& config) {
  const float nyquist = static_cast<float>(sample_rate_ * 0.5);
  const float cutoff = std::clamp(config.frequency_hz, 10.0f, nyquist * 0.98f);
  const float q = std::max(1.0e-3f, config.bandpass_q);
  const float w0 = static_cast<float>(2.0 * kPiD * cutoff / sample_rate_);
  filter_coeffs_.c = rt::rbj_bandpass(w0, q);
  for (auto& filter : bandpass_) {
    const float z1 = filter.z1;
    const float z2 = filter.z2;
    filter = filter_coeffs_;
    filter.z1 = z1;
    filter.z2 = z2;
  }
  for (auto& filter : bandpass2_) {
    const float z1 = filter.z1;
    const float z2 = filter.z2;
    filter = filter_coeffs_;
    filter.z1 = z1;
    filter.z2 = z2;
  }
  for (auto& follower : followers_) {
    follower.prepare(sample_rate_, config.attack_ms, config.release_ms);
  }
}

}  // namespace sonare::mastering::dynamics
