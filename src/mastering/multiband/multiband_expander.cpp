#include "mastering/multiband/multiband_expander.h"

#include <algorithm>
#include <string>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::multiband {

MultibandExpander::MultibandExpander(MultibandExpanderConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
  rebuild_processors();
}

void MultibandExpander::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void MultibandExpander::prepare(double sample_rate, int max_block_size, int max_channels) {
  validate_prepare_args(sample_rate, max_block_size, max_channels, "MultibandExpander");

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  max_working_channels_ = max_channels;
  prepared_ = true;
  crossover_.prepare(sample_rate_, max_block_size_, max_working_channels_);
  crossover_.prepare_scratch(scratch_, max_working_channels_, max_block_size_);
  for (auto& expander : expanders_) {
    expander.prepare(sample_rate_, max_block_size_);
  }
  reset();
}

void MultibandExpander::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "MultibandExpander");
  if (!validate_block_size(num_channels, num_samples)) {
    return;
  }
  if (num_channels > max_working_channels_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels exceeds prepared MultibandExpander capacity");
  }
  if (num_samples > max_block_size_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared MultibandExpander block size");
  }
  validate_channel_buffers(channels, num_channels);

  crossover_.ensure_scratch(scratch_, num_channels, num_samples);
  crossover_.split_into(channels, num_channels, num_samples, scratch_);
  const int num_bands = scratch_.num_bands();
  for (int band = 0; band < num_bands; ++band) {
    expanders_[static_cast<size_t>(band)].run(
        detector_excluded_channel(num_channels),
        scratch_.band_channels[static_cast<size_t>(band)].data(), num_channels, num_samples);
    last_gain_reductions_db_[static_cast<size_t>(band)] =
        expanders_[static_cast<size_t>(band)].last_gain_reduction_db();
  }

  scratch_.sum_into(channels, num_channels, num_samples);
}

void MultibandExpander::reset() {
  crossover_.reset();
  for (auto& expander : expanders_) {
    expander.reset();
  }
  std::fill(last_gain_reductions_db_.begin(), last_gain_reductions_db_.end(), 0.0f);
}

void MultibandExpander::set_config(const MultibandExpanderConfig& config) {
  validate_config(config);
  if (prepared_) Crossover::validate_config(config.crossover, sample_rate_);
  rt::apply_config_diff(
      config_, config, [this](const rt::ConfigDiff<MultibandExpanderConfig>& diff) {
        // The crossover rebuilds (and zeroes) itself only for a changed split.
        if (diff.changed(&MultibandExpanderConfig::crossover)) {
          crossover_.set_config(config_.crossover);
          if (prepared_)
            crossover_.prepare_scratch(scratch_, max_working_channels_, max_block_size_);
        }
        if (expanders_.size() == config_.bands.size()) {
          // Same band count: each expander takes its own config as a snapshot, which
          // keeps its envelope and detector history.
          for (size_t band = 0; band < expanders_.size(); ++band) {
            expanders_[band].set_config(config_.bands[band]);
          }
          return;
        }
        rebuild_processors();
        if (prepared_) {
          for (auto& expander : expanders_) expander.prepare(sample_rate_, max_block_size_);
        }
      });
}

bool MultibandExpander::set_parameter_impl(unsigned int param_id, float value) {
  const unsigned int band = param_id / kBandStride;
  if (band >= expanders_.size()) {
    return false;
  }
  const unsigned int band_param = param_id % kBandStride;
  if (expanders_[band].set_parameter(band_param, value)) {
    config_.bands[band] = expanders_[band].config();
    return true;
  }
  return false;
}

std::vector<rt::ParamDescriptor> MultibandExpander::parameter_descriptors() const {
  // Mirror the per-band block layout of set_parameter: each band forwards its
  // local param ids to dynamics::Expander, so reuse the Expander descriptors and
  // offset both the id and the construction-time JSON key by the band index.
  std::vector<rt::ParamDescriptor> descriptors;
  for (size_t band = 0; band < expanders_.size(); ++band) {
    const std::string prefix = "band" + std::to_string(band) + ".";
    const unsigned int base = static_cast<unsigned int>(band) * kBandStride;
    for (const auto& band_descriptor : expanders_[band].parameter_descriptors()) {
      descriptors.push_back({prefix + band_descriptor.key, base + band_descriptor.id});
    }
  }
  return descriptors;
}

void MultibandExpander::validate_config(const MultibandExpanderConfig& config) {
  const size_t expected_bands = config.crossover.cutoffs_hz.size() + 1;
  if (config.bands.size() != expected_bands) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "multiband expander band count must match crossover");
  }
  // The crossover and every band are checked here, so set_config() refuses
  // before it commits anything.
  Crossover::validate_config(config.crossover);
  for (const auto& band : config.bands) dynamics::Expander::validate_config(band);
}

void MultibandExpander::rebuild_processors() {
  expanders_.clear();
  expanders_.reserve(config_.bands.size());
  for (const auto& band_config : config_.bands) {
    expanders_.emplace_back(band_config);
  }
  last_gain_reductions_db_.assign(config_.bands.size(), 0.0f);
}

}  // namespace sonare::mastering::multiband
