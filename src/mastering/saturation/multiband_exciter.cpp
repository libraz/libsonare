#include "mastering/saturation/multiband_exciter.h"

#include <algorithm>
#include <utility>

#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {

MultibandExciter::MultibandExciter(MultibandExciterConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
  rebuild_processors();
}

void MultibandExciter::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  prepared_ = true;
  crossover_.prepare(sample_rate_, max_block_size_);
  // Pre-size split scratch for stereo so the steady-state audio path is
  // allocation-free; it is grown on demand only if a wider block arrives.
  crossover_.prepare_scratch(scratch_, 2, max_block_size_);
  for (auto& exciter : exciters_) exciter.prepare(sample_rate_, max_block_size_);
  reset();
}

void MultibandExciter::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "MultibandExciter");
  if (num_channels < 0 || num_samples < 0)
    throw SonareException(ErrorCode::InvalidParameter, "invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
  }

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
  if (prepared_) prepare(sample_rate_, max_block_size_);
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
