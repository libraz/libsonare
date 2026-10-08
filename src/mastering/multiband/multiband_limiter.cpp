#include "mastering/multiband/multiband_limiter.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "mastering/eq/parametric.h"
#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::multiband {

MultibandLimiter::MultibandLimiter(MultibandLimiterConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
  rebuild_processors();
}

void MultibandLimiter::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void MultibandLimiter::prepare(double sample_rate, int max_block_size, int max_channels) {
  validate_prepare_args(sample_rate, max_block_size, max_channels, "MultibandLimiter");

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  max_working_channels_ = max_channels;
  prepared_ = true;
  crossover_.prepare(sample_rate_, max_block_size_, max_working_channels_);
  crossover_.prepare_scratch(scratch_, max_working_channels_, max_block_size_);
  for (auto& limiter : limiters_) {
    limiter.prepare(sample_rate_, max_block_size_);
  }
  rebuild_band_compensation();
  reset();
}

void MultibandLimiter::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "MultibandLimiter");
  if (!validate_block_size(num_channels, num_samples)) {
    return;
  }
  if (num_channels > max_working_channels_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels exceeds prepared MultibandLimiter capacity");
  }
  if (num_samples > max_block_size_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared MultibandLimiter block size");
  }
  validate_channel_buffers(channels, num_channels);

  crossover_.ensure_scratch(scratch_, num_channels, num_samples);
  crossover_.split_into(channels, num_channels, num_samples, scratch_);
  const int num_bands = scratch_.num_bands();
  for (int band = 0; band < num_bands; ++band) {
    auto& band_channels = scratch_.band_channels[static_cast<size_t>(band)];
    limiters_[static_cast<size_t>(band)].process(band_channels.data(), num_channels, num_samples);
    for (int ch = 0; ch < num_channels; ++ch) {
      band_paths_.align_block(static_cast<size_t>(band), static_cast<size_t>(ch),
                              band_channels[static_cast<size_t>(ch)], num_samples);
    }
    last_gain_reductions_db_[static_cast<size_t>(band)] =
        limiters_[static_cast<size_t>(band)].last_gain_reduction_db();
  }

  scratch_.sum_into(channels, num_channels, num_samples);
}

void MultibandLimiter::reset() {
  crossover_.reset();
  for (auto& limiter : limiters_) {
    limiter.reset();
  }
  std::fill(last_gain_reductions_db_.begin(), last_gain_reductions_db_.end(), 0.0f);
  band_paths_.reset();
}

int MultibandLimiter::latency_samples() const noexcept { return latency_samples_q8() >> 8; }

int MultibandLimiter::latency_samples_q8() const noexcept {
  return (crossover_.latency_samples() << 8) + band_paths_.latency_samples_q8();
}

void MultibandLimiter::rebuild_band_compensation() {
  std::vector<int> latencies(limiters_.size());
  for (size_t band = 0; band < limiters_.size(); ++band) {
    latencies[band] = limiters_[band].latency_samples_q8();
  }
  band_paths_.set_path_latencies_q8(std::move(latencies));
  band_paths_.ensure_channels(static_cast<size_t>(max_working_channels_));
}

void MultibandLimiter::set_config(const MultibandLimiterConfig& config) {
  validate_config(config);
  // Only reconfigure/re-prepare the crossover when its parameters actually
  // change; rebuilding it zeroes the crossover filter state and would click on
  // band-parameter-only updates. Sub-processors are always rebuilt and prepared.
  const bool crossover_changed = config.crossover != config_.crossover;
  // Before the mirror: an unprepared crossover must still adopt the new split,
  // and a rejected one leaves config() untouched.
  if (crossover_changed) crossover_.set_config(config.crossover);
  config_ = config;
  rebuild_processors();
  if (prepared_) {
    if (crossover_changed) {
      crossover_.prepare_scratch(scratch_, max_working_channels_, max_block_size_);
    }
    for (auto& limiter : limiters_) {
      limiter.prepare(sample_rate_, max_block_size_);
    }
  }
  rebuild_band_compensation();
}

bool MultibandLimiter::set_parameter_impl(unsigned int param_id, float value) {
  const unsigned int band = param_id / kBandStride;
  if (band >= limiters_.size()) {
    return false;
  }
  const unsigned int band_param = param_id % kBandStride;
  if (limiters_[band].set_parameter(band_param, value)) {
    config_.bands[band] = limiters_[band].config();
    return true;
  }
  return false;
}

std::vector<rt::ParamDescriptor> MultibandLimiter::parameter_descriptors() const {
  static constexpr const char* kBandParamKeys[kBandStride] = {"thresholdDb", "releaseMs"};
  return eq::banded_parameter_descriptors(limiters_.size(), kBandParamKeys, kBandStride);
}

void MultibandLimiter::validate_config(const MultibandLimiterConfig& config) {
  const size_t expected_bands = config.crossover.cutoffs_hz.size() + 1;
  if (config.bands.size() != expected_bands) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "multiband limiter band count must match crossover");
  }
}

void MultibandLimiter::rebuild_processors() {
  limiters_.clear();
  limiters_.reserve(config_.bands.size());
  for (const auto& band_config : config_.bands) {
    limiters_.emplace_back(band_config);
  }
  last_gain_reductions_db_.assign(config_.bands.size(), 0.0f);
}

}  // namespace sonare::mastering::multiband
