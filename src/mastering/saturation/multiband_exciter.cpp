#include "mastering/saturation/multiband_exciter.h"

#include <algorithm>
#include <cstdint>
#include <string>
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
  // Summed over the crossover and every band, read once on either side of the
  // whole block: the sum detects that some member moved without standing in for
  // this processor's own count, which is one per process() call.
  const auto member_discards = [this]() noexcept -> uint64_t {
    uint64_t total = crossover_.non_finite_discard_count();
    for (const auto& exciter : exciters_) total += exciter.non_finite_discard_count();
    return total;
  };
  const uint64_t member_discards_before = member_discards();

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

  if (member_discards() != member_discards_before) note_non_finite_discard();
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
  const unsigned int band = param_id / kBandStride;
  if (band >= exciters_.size()) {
    return false;
  }
  const unsigned int band_param = param_id % kBandStride;
  if (exciters_[band].set_parameter(band_param, value)) {
    // Keep the kept config mirror in sync so config() reflects the automation.
    config_.bands[band] = exciters_[band].config();
    return true;
  }
  return false;
}

std::vector<rt::ParamDescriptor> MultibandExciter::parameter_descriptors() const {
  // Mirror the per-band block layout of set_parameter: each band forwards its
  // local param ids to Exciter, so reuse the Exciter descriptors and offset both
  // the id and the construction-time JSON key by the band index.
  std::vector<rt::ParamDescriptor> descriptors;
  descriptors.reserve(exciters_.size() * kBandStride);
  for (size_t band = 0; band < exciters_.size(); ++band) {
    const std::string prefix = "band" + std::to_string(band) + ".";
    const unsigned int base = static_cast<unsigned int>(band) * kBandStride;
    for (const auto& band_descriptor : exciters_[band].parameter_descriptors()) {
      descriptors.push_back({prefix + band_descriptor.key, base + band_descriptor.id});
    }
  }
  return descriptors;
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
