#include "mastering/multiband/multiband_compressor.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "mastering/eq/parametric.h"
#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mastering::multiband {

MultibandCompressor::MultibandCompressor(MultibandCompressorConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
  rebuild_processors();
}

void MultibandCompressor::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void MultibandCompressor::prepare(double sample_rate, int max_block_size, int max_channels) {
  validate_prepare_args(sample_rate, max_block_size, max_channels, "MultibandCompressor");

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  max_working_channels_ = max_channels;
  prepared_ = true;
  crossover_.prepare(sample_rate_, max_block_size_, max_working_channels_);
  crossover_.prepare_scratch(scratch_, max_working_channels_, max_block_size_);
  for (auto& compressor : compressors_) {
    compressor.prepare(sample_rate_, max_block_size_);
  }
  reset();
}

void MultibandCompressor::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "MultibandCompressor");
  if (!validate_block_size(num_channels, num_samples)) {
    return;
  }
  if (num_channels > max_working_channels_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels exceeds prepared MultibandCompressor capacity");
  }
  if (num_samples > max_block_size_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared MultibandCompressor block size");
  }
  validate_channel_buffers(channels, num_channels);

  // Summed over the crossover and every band, read once on either side of the
  // whole block: the sum detects that some member moved without standing in for
  // this processor's own count, which is one per process() call.
  const auto member_discards = [this]() noexcept -> uint64_t {
    uint64_t total = crossover_.non_finite_discard_count();
    for (const auto& compressor : compressors_) {
      total += compressor.non_finite_discard_count();
    }
    return total;
  };
  const uint64_t member_discards_before = member_discards();

  crossover_.split_into(channels, num_channels, num_samples, scratch_);
  const int num_bands = scratch_.num_bands();
  for (int band = 0; band < num_bands; ++band) {
    compressors_[static_cast<size_t>(band)].run(
        detector_excluded_channel(num_channels),
        scratch_.band_channels[static_cast<size_t>(band)].data(), num_channels, num_samples);
    last_gain_reductions_db_[static_cast<size_t>(band)] =
        compressors_[static_cast<size_t>(band)].last_gain_reduction_db();
  }

  scratch_.sum_into(channels, num_channels, num_samples);

  if (member_discards() != member_discards_before) note_non_finite_discard();
}

std::vector<float> MultibandCompressor::minimum_gain_reductions_db() const {
  std::vector<float> out;
  out.reserve(compressors_.size());
  for (const auto& compressor : compressors_) {
    out.push_back(compressor.minimum_gain_reduction_db());
  }
  return out;
}

void MultibandCompressor::reset() {
  crossover_.reset();
  for (auto& compressor : compressors_) {
    compressor.reset();
  }
  std::fill(last_gain_reductions_db_.begin(), last_gain_reductions_db_.end(), 0.0f);
}

void MultibandCompressor::set_config(const MultibandCompressorConfig& config) {
  validate_config(config);
  if (prepared_) Crossover::validate_config(config.crossover, sample_rate_);
  rt::apply_config_diff(
      config_, config, [this](const rt::ConfigDiff<MultibandCompressorConfig>& diff) {
        // The crossover rebuilds (and zeroes) itself only for a changed split.
        if (diff.changed(&MultibandCompressorConfig::crossover)) {
          crossover_.set_config(config_.crossover);
          if (prepared_)
            crossover_.prepare_scratch(scratch_, max_working_channels_, max_block_size_);
        }
        if (compressors_.size() == config_.bands.size()) {
          // Same band count: each compressor takes its own config as a snapshot,
          // which keeps its envelope and detector history.
          for (size_t band = 0; band < compressors_.size(); ++band) {
            compressors_[band].set_config(config_.bands[band]);
          }
          return;
        }
        rebuild_processors();
        if (prepared_) {
          for (auto& compressor : compressors_) compressor.prepare(sample_rate_, max_block_size_);
        }
      });
}

bool MultibandCompressor::set_parameter_impl(unsigned int param_id, float value) {
  const unsigned int band = param_id / kBandStride;
  if (band >= compressors_.size()) {
    return false;
  }
  const unsigned int band_param = param_id % kBandStride;
  // Keep config_ in sync so config() and subsequent set_config() observe the
  // automated value; the sub-processor recomputes coefficients in place.
  if (compressors_[band].set_parameter(band_param, value)) {
    config_.bands[band] = compressors_[band].config();
    return true;
  }
  return false;
}

std::vector<rt::ParamDescriptor> MultibandCompressor::parameter_descriptors() const {
  // Mirrors set_parameter exactly: id = band * kBandStride + band_param, valid
  // for every band that exists (band < compressors_.size()) and band_param in
  // [0, kBandStride). Keys use the construction-time band{i}.<field> convention.
  static constexpr const char* kBandParamKeys[kBandStride] = {"thresholdDb", "ratio", "attackMs",
                                                              "releaseMs", "makeupGainDb"};
  return eq::banded_parameter_descriptors(compressors_.size(), kBandParamKeys, kBandStride);
}

void MultibandCompressor::validate_config(const MultibandCompressorConfig& config) {
  const size_t expected_bands = config.crossover.cutoffs_hz.size() + 1;
  if (config.bands.size() != expected_bands) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "multiband compressor band count must match crossover");
  }
  // The crossover and every band are checked here, so set_config() refuses
  // before it commits anything.
  Crossover::validate_config(config.crossover);
  for (const auto& band : config.bands) dynamics::Compressor::validate_config(band);
}

void MultibandCompressor::rebuild_processors() {
  compressors_.clear();
  compressors_.reserve(config_.bands.size());
  for (const auto& band_config : config_.bands) {
    compressors_.emplace_back(band_config);
  }
  last_gain_reductions_db_.assign(config_.bands.size(), 0.0f);
}

}  // namespace sonare::mastering::multiband
