#include "mastering/saturation/multiband_exciter.h"

#include <algorithm>
#include <utility>

#include "mastering/dynamics/channel_limits.h"
#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {

MultibandExciter::MultibandExciter(MultibandExciterConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
  rebuild_processors();
}

void MultibandExciter::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void MultibandExciter::prepare(double sample_rate, int max_block_size, int max_channels) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  if (max_channels < 1 || max_channels > static_cast<int>(dynamics::kRealtimePreparedChannels))
    throw SonareException(ErrorCode::InvalidParameter,
                          "max_channels exceeds MultibandExciter capacity");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  max_working_channels_ = max_channels;
  prepared_ = true;
  crossover_.prepare(sample_rate_, max_block_size_, max_working_channels_);
  crossover_.prepare_scratch(scratch_, max_working_channels_, max_block_size_);
  for (auto& exciter : exciters_) exciter.prepare(sample_rate_, max_block_size_);
  reset();
}

void MultibandExciter::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "MultibandExciter");
  if (!validate_block_size(num_channels, num_samples)) {
    return;
  }
  if (num_channels > max_working_channels_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels exceeds prepared MultibandExciter capacity");
  }
  if (num_samples > max_block_size_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared MultibandExciter block size");
  }
  validate_channel_buffers(channels, num_channels);

  crossover_.ensure_scratch(scratch_, num_channels, num_samples);
  crossover_.split_into(channels, num_channels, num_samples, scratch_);
  const int num_bands = scratch_.num_bands();
  for (int band = 0; band < num_bands; ++band) {
    exciters_[static_cast<size_t>(band)].process(
        scratch_.band_channels[static_cast<size_t>(band)].data(), num_channels, num_samples);
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
    for (int band = 0; band < num_bands; ++band) {
      const auto& samples = scratch_.bands[static_cast<size_t>(band)][static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) channels[ch][i] += samples[static_cast<size_t>(i)];
    }
  }
}

void MultibandExciter::reset() {
  crossover_.reset();
  for (auto& exciter : exciters_) exciter.reset();
}

void MultibandExciter::set_config(const MultibandExciterConfig& config) {
  validate_config(config);
  config_ = config;
  crossover_.set_config(config_.crossover);
  rebuild_processors();
  if (prepared_) prepare(sample_rate_, max_block_size_, max_working_channels_);
}

bool MultibandExciter::set_parameter(unsigned int param_id, float value) {
  if (param_id > 4 || exciters_.empty()) return false;
  for (size_t band = 0; band < exciters_.size(); ++band) {
    if (!exciters_[band].set_parameter(param_id, value)) return false;
    // Keep the kept config mirror in sync so config() reflects the automation.
    config_.bands[band] = exciters_[band].config();
  }
  return true;
}

std::vector<rt::ParamDescriptor> MultibandExciter::parameter_descriptors() const {
  return {{"frequencyHz", 0}, {"driveDb", 1}, {"amount", 2}, {"q", 3}, {"evenOddMix", 4}};
}

void MultibandExciter::validate_config(const MultibandExciterConfig& config) {
  if (config.bands.size() != config.crossover.cutoffs_hz.size() + 1) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "multiband exciter band count must match crossover");
  }
}

void MultibandExciter::rebuild_processors() {
  exciters_.clear();
  exciters_.reserve(config_.bands.size());
  for (const auto& band : config_.bands) exciters_.emplace_back(band);
}

}  // namespace sonare::mastering::saturation
