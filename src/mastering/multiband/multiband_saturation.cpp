#include "mastering/multiband/multiband_saturation.h"

#include <algorithm>
#include <string>
#include <utility>

#include "mastering/saturation/exciter.h"
#include "mastering/saturation/soft_clipper.h"
#include "mastering/saturation/tape.h"
#include "mastering/saturation/tube.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::multiband {

namespace {

std::unique_ptr<rt::ProcessorBase> make_processor(const SaturationBandConfig& band) {
  // Map the shared band config onto each algorithm's native parameters. drive_db
  // and mix carry over where the algorithm supports them; output_gain_db is
  // applied uniformly by the multiband loop so it works for every type.
  switch (band.type) {
    case SaturationType::Tape: {
      saturation::TapeConfig cfg;
      cfg.drive_db = band.drive_db;
      cfg.output_gain_db = 0.0f;
      return std::make_unique<saturation::Tape>(cfg);
    }
    case SaturationType::Tube: {
      saturation::TubeConfig cfg;
      cfg.drive_db = band.drive_db;
      cfg.mix = std::clamp(band.mix, 0.0f, 1.0f);
      return std::make_unique<saturation::Tube>(cfg);
    }
    case SaturationType::Exciter: {
      saturation::ExciterConfig cfg;
      cfg.drive_db = band.drive_db;
      cfg.amount = std::clamp(band.mix, 0.0f, 1.0f);
      return std::make_unique<saturation::Exciter>(cfg);
    }
    case SaturationType::SoftClip:
      break;
  }
  saturation::SoftClipperConfig cfg;
  cfg.drive_db = band.drive_db;
  cfg.mix = std::clamp(band.mix, 0.0f, 1.0f);
  return std::make_unique<saturation::SoftClipper>(cfg);
}

}  // namespace

MultibandSaturation::MultibandSaturation(MultibandSaturationConfig config)
    : config_(std::move(config)), crossover_(config_.crossover) {
  validate_config(config_);
  rebuild_processors();
}

MultibandSaturation::~MultibandSaturation() = default;

void MultibandSaturation::prepare(double sample_rate, int max_block_size) {
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
  for (auto& processor : processors_) {
    processor->prepare(sample_rate_, max_block_size_);
  }
  reset();
}

void MultibandSaturation::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "MultibandSaturation");
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
    const auto& band_config = config_.bands[static_cast<size_t>(band)];
    if (!band_config.enabled) {
      continue;
    }
    processors_[static_cast<size_t>(band)]->process(
        scratch_.band_channels[static_cast<size_t>(band)].data(), num_channels, num_samples);
    const float output_gain = db_to_linear(band_config.output_gain_db);
    if (output_gain != 1.0f) {
      for (int ch = 0; ch < num_channels; ++ch) {
        auto& band_samples = scratch_.bands[static_cast<size_t>(band)][static_cast<size_t>(ch)];
        for (int i = 0; i < num_samples; ++i) {
          band_samples[static_cast<size_t>(i)] *= output_gain;
        }
      }
    }
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

void MultibandSaturation::reset() {
  crossover_.reset();
  for (auto& processor : processors_) {
    processor->reset();
  }
}

void MultibandSaturation::set_config(const MultibandSaturationConfig& config) {
  validate_config(config);
  // Only reconfigure/re-prepare the crossover when its parameters actually
  // change. crossover_.set_config() (and prepare()) rebuild and zero the
  // crossover filter state, which would click if invoked on every set_config
  // call that only touches band parameters. Sub-processors are always rebuilt
  // and prepared; that path does not disturb crossover state.
  const bool crossover_changed = config.crossover != config_.crossover;
  config_ = config;
  rebuild_processors();
  if (prepared_) {
    if (crossover_changed) {
      crossover_.set_config(config_.crossover);
      crossover_.prepare_scratch(scratch_, 2, max_block_size_);
    }
    for (auto& processor : processors_) {
      processor->prepare(sample_rate_, max_block_size_);
    }
  }
}

bool MultibandSaturation::set_parameter(unsigned int param_id, float value) {
  const size_t band = param_id / kBandStride;
  if (band >= config_.bands.size()) {
    return false;
  }
  auto& band_config = config_.bands[band];
  auto& processor = *processors_[band];
  switch (param_id % kBandStride) {
    case 0:
      band_config.drive_db = value;
      // drive_db is parameter 0 on every algorithm except the exciter, whose
      // drive is parameter 1 (parameter 0 is its band-pass frequency).
      return processor.set_parameter(band_config.type == SaturationType::Exciter ? 1u : 0u, value);
    case 1: {
      band_config.mix = std::clamp(value, 0.0f, 1.0f);
      switch (band_config.type) {
        case SaturationType::SoftClip:
          return processor.set_parameter(1u, band_config.mix);
        case SaturationType::Tube:
          return processor.set_parameter(2u, band_config.mix);
        case SaturationType::Exciter:
          return processor.set_parameter(2u, band_config.mix);
        case SaturationType::Tape:
          // The tape model has no wet/dry mix parameter; only config_ is kept
          // in sync so config() reflects the requested value.
          return true;
      }
      return true;
    }
    case 2:
      // output_gain_db is applied by the multiband loop, not the sub-processor.
      band_config.output_gain_db = value;
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> MultibandSaturation::parameter_descriptors() const {
  // Mirrors set_parameter exactly: id = band * kBandStride + band_param, valid
  // for every band that exists (band < processors_.size()) and band_param in
  // [0, kBandStride). Keys use the construction-time band{i}.<field> convention.
  static constexpr const char* kBandParamKeys[kBandStride] = {"driveDb", "mix", "outputGainDb"};
  std::vector<rt::ParamDescriptor> descriptors;
  descriptors.reserve(processors_.size() * kBandStride);
  for (unsigned int band = 0; band < processors_.size(); ++band) {
    const std::string prefix = "band" + std::to_string(band) + ".";
    for (unsigned int band_param = 0; band_param < kBandStride; ++band_param) {
      descriptors.push_back({prefix + kBandParamKeys[band_param], band * kBandStride + band_param});
    }
  }
  return descriptors;
}

void MultibandSaturation::validate_config(const MultibandSaturationConfig& config) {
  const size_t expected_bands = config.crossover.cutoffs_hz.size() + 1;
  if (config.bands.size() != expected_bands) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "multiband saturation band count must match crossover");
  }
  for (const auto& band : config.bands) {
    if (band.mix < 0.0f || band.mix > 1.0f) {
      throw SonareException(ErrorCode::InvalidParameter, "saturation mix must be in [0, 1]");
    }
  }
}

void MultibandSaturation::rebuild_processors() {
  processors_.clear();
  processors_.reserve(config_.bands.size());
  for (const auto& band_config : config_.bands) {
    processors_.push_back(make_processor(band_config));
  }
}

}  // namespace sonare::mastering::multiband
