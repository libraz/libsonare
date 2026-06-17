#include "mastering/multiband/multiband_limiter.h"

#include <algorithm>
#include <string>

#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::multiband {

MultibandLimiter::MultibandLimiter(MultibandLimiterConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
  rebuild_processors();
}

void MultibandLimiter::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  crossover_.prepare(sample_rate_, max_block_size_);
  // Pre-size split scratch for stereo so the steady-state audio path is
  // allocation-free; it is grown on demand only if a wider block arrives.
  crossover_.prepare_scratch(scratch_, 2, max_block_size_);
  for (auto& limiter : limiters_) {
    limiter.prepare(sample_rate_, max_block_size_);
  }
  reset();
}

void MultibandLimiter::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "MultibandLimiter");
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

  crossover_.ensure_scratch(scratch_, num_channels, num_samples);
  crossover_.split_into(channels, num_channels, num_samples, scratch_);
  const int num_bands = scratch_.num_bands();
  for (int band = 0; band < num_bands; ++band) {
    limiters_[static_cast<size_t>(band)].process(
        scratch_.band_channels[static_cast<size_t>(band)].data(), num_channels, num_samples);
    last_gain_reductions_db_[static_cast<size_t>(band)] =
        limiters_[static_cast<size_t>(band)].last_gain_reduction_db();
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
    for (int band = 0; band < num_bands; ++band) {
      const auto& band_samples = scratch_.bands[static_cast<size_t>(band)][static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] += band_samples[static_cast<size_t>(i)];
      }
    }
  }
}

void MultibandLimiter::reset() {
  crossover_.reset();
  for (auto& limiter : limiters_) {
    limiter.reset();
  }
  std::fill(last_gain_reductions_db_.begin(), last_gain_reductions_db_.end(), 0.0f);
}

int MultibandLimiter::latency_samples() const noexcept {
  // All bands share the same lookahead configuration, so the per-band limiter
  // latency is uniform; report band 0's latency. Guard against an empty band
  // list (e.g. before prepare()).
  // The linear-phase FIR crossover delays every band; add it to the per-band
  // limiter lookahead so host plugin-delay-compensation stays correct.
  const int crossover_latency = crossover_.latency_samples();
  if (limiters_.empty()) {
    return crossover_latency;
  }
  return crossover_latency + limiters_[0].latency_samples();
}

void MultibandLimiter::set_config(const MultibandLimiterConfig& config) {
  validate_config(config);
  // Only reconfigure/re-prepare the crossover when its parameters actually
  // change; rebuilding it zeroes the crossover filter state and would click on
  // band-parameter-only updates. Sub-processors are always rebuilt and prepared.
  const bool crossover_changed = config.crossover != config_.crossover;
  config_ = config;
  rebuild_processors();
  if (prepared_) {
    if (crossover_changed) {
      crossover_.set_config(config_.crossover);
      crossover_.prepare_scratch(scratch_, 2, max_block_size_);
    }
    for (auto& limiter : limiters_) {
      limiter.prepare(sample_rate_, max_block_size_);
    }
  }
}

bool MultibandLimiter::set_parameter(unsigned int param_id, float value) {
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
  std::vector<rt::ParamDescriptor> descriptors;
  descriptors.reserve(limiters_.size() * kBandStride);
  for (unsigned int band = 0; band < limiters_.size(); ++band) {
    const std::string prefix = "band" + std::to_string(band) + ".";
    descriptors.push_back({prefix + "thresholdDb", band * kBandStride + 0});
    descriptors.push_back({prefix + "releaseMs", band * kBandStride + 1});
  }
  return descriptors;
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
