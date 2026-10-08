#include "mastering/multiband/multiband_saturation.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "mastering/eq/parametric.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/soft_clipper.h"
#include "mastering/saturation/tape.h"
#include "mastering/saturation/tube.h"
#include "rt/scoped_no_denormals.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::mastering::multiband {

namespace {

std::unique_ptr<rt::ProcessorBase> make_processor(const SaturationBandConfig& band) {
  // Map the shared band config onto each algorithm's native parameters. drive_db
  // and mix carry over where the algorithm supports them; output_gain_db, and
  // the tape's mix, are applied by the multiband loop.
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
  rebuild_band_compensation();
}

MultibandSaturation::~MultibandSaturation() = default;

void MultibandSaturation::prepare(double sample_rate, int max_block_size) {
  // Realtime callers use the two-argument ProcessorBase API and may switch
  // between any supported channel count without an allocation in process().
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void MultibandSaturation::prepare(double sample_rate, int max_block_size, int max_channels) {
  validate_prepare_args(sample_rate, max_block_size, max_channels, "MultibandSaturation");

  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  max_working_channels_ = max_channels;
  prepared_ = true;
  crossover_.prepare(sample_rate_, max_block_size_, max_working_channels_);
  // Size the caller-owned split buffers to the effective bound. Crossover
  // scratch accepts at least this many channels, so the process path can reject
  // an oversized block rather than growing scratch on the audio thread.
  crossover_.prepare_scratch(scratch_, max_working_channels_, max_block_size_);
  dry_scratch_.assign(static_cast<size_t>(max_working_channels_),
                      std::vector<float>(static_cast<size_t>(max_block_size_), 0.0f));
  for (auto& processor : processors_) {
    processor->prepare(sample_rate_, max_block_size_, max_working_channels_);
  }
  // Derived after the sub-processors are prepared, since a stage's latency may
  // depend on its prepared state.
  rebuild_band_compensation();
  reset();
}

void MultibandSaturation::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "MultibandSaturation");
  if (!validate_block_size(num_channels, num_samples)) {
    return;
  }
  if (num_channels > max_working_channels_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels exceeds prepared MultibandSaturation capacity");
  }
  if (num_samples > max_block_size_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared MultibandSaturation block size");
  }
  validate_channel_buffers(channels, num_channels);

  // Summed over the crossover and every band, read once on either side of the
  // whole block: the sum detects that some member moved without standing in for
  // this processor's own count, which is one per process() call.
  const auto member_discards = [this]() noexcept -> uint64_t {
    uint64_t total = crossover_.non_finite_discard_count();
    for (const auto& processor : processors_) {
      if (processor) total += processor->non_finite_discard_count();
    }
    return total;
  };
  const uint64_t member_discards_before = member_discards();

  // Valid prepared blocks reuse the exact bound-sized scratch without
  // allocation; oversized blocks fail closed above rather than growing scratch
  // on the audio thread.
  crossover_.ensure_scratch(scratch_, num_channels, num_samples);
  crossover_.split_into(channels, num_channels, num_samples, scratch_);
  const int num_bands = scratch_.num_bands();
  for (int band = 0; band < num_bands; ++band) {
    const auto& band_config = config_.bands[static_cast<size_t>(band)];
    auto& blend = blend_paths_[static_cast<size_t>(band)];
    auto& processor = *processors_[static_cast<size_t>(band)];
    // A band coming back into the path starts from rest, never from the
    // saturator and blend history it froze with when it was disabled.
    if (band_gates_[static_cast<size_t>(band)].admit(band_config.enabled, [&processor, &blend] {
          processor.reset();
          blend.reset();
        })) {
      auto& band_channels = scratch_.band_channels[static_cast<size_t>(band)];
      const bool blended = blend.num_paths() != 0;
      if (blended) {
        for (int ch = 0; ch < num_channels; ++ch) {
          std::copy_n(band_channels[static_cast<size_t>(ch)], num_samples,
                      dry_scratch_[static_cast<size_t>(ch)].data());
        }
      }
      processor.process(band_channels.data(), num_channels, num_samples);
      if (blended) {
        const float mix = band_config.mix;
        for (int ch = 0; ch < num_channels; ++ch) {
          const auto lane = static_cast<size_t>(ch);
          float* wet = band_channels[lane];
          const float* dry = dry_scratch_[lane].data();
          for (int i = 0; i < num_samples; ++i) {
            wet[i] =
                blend.align(0, lane, dry[i]) * (1.0f - mix) + blend.align(1, lane, wet[i]) * mix;
          }
        }
      }
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
    // Pad the shallower bands (including a bypassed one) up to the deepest band
    // delay so the sum below reconstructs the crossover.
    for (int ch = 0; ch < num_channels; ++ch) {
      band_paths_.align_block(
          static_cast<size_t>(band), static_cast<size_t>(ch),
          scratch_.band_channels[static_cast<size_t>(band)][static_cast<size_t>(ch)], num_samples);
    }
  }

  scratch_.sum_into(channels, num_channels, num_samples);

  if (member_discards() != member_discards_before) note_non_finite_discard();
}

void MultibandSaturation::reset() {
  crossover_.reset();
  for (auto& processor : processors_) {
    processor->reset();
  }
  band_paths_.reset();
  for (auto& blend : blend_paths_) blend.reset();
}

void MultibandSaturation::set_config(const MultibandSaturationConfig& config) {
  validate_config(config);
  if (prepared_) multiband::Crossover::validate_config(config.crossover, sample_rate_);
  rt::apply_config_diff(
      config_, config, [this](const rt::ConfigDiff<MultibandSaturationConfig>& diff) {
        // The crossover rebuilds (and zeroes) itself only for a changed split.
        if (diff.changed(&MultibandSaturationConfig::crossover)) {
          crossover_.set_config(config_.crossover);
          if (prepared_)
            crossover_.prepare_scratch(scratch_, max_working_channels_, max_block_size_);
        }
        if (processors_.size() == config_.bands.size()) {
          // Only a band whose own settings changed is rebuilt; the others keep
          // their saturator, oversampler and filter history.
          for (size_t band = 0; band < processors_.size(); ++band) {
            if (config_.bands[band] == diff.held().bands[band]) continue;
            processors_[band] = make_processor(config_.bands[band]);
            band_gates_[band].close();
            if (prepared_) {
              processors_[band]->prepare(sample_rate_, max_block_size_, max_working_channels_);
            }
          }
        } else {
          rebuild_processors();
          if (prepared_) {
            for (auto& processor : processors_) {
              processor->prepare(sample_rate_, max_block_size_, max_working_channels_);
            }
          }
        }
        // A band's type sets its delay; unchanged latencies keep their lines.
        rebuild_band_compensation();
      });
}

bool MultibandSaturation::set_parameter_impl(unsigned int param_id, float value) {
  const size_t band = param_id / kBandStride;
  if (band >= config_.bands.size()) {
    return false;
  }
  auto& band_config = config_.bands[band];
  auto& processor = *processors_[band];
  switch (param_id % kBandStride) {
    case 0:
      // drive_db is parameter 0 on every algorithm except the exciter, whose
      // drive is parameter 1 (parameter 0 is its band-pass frequency).
      if (!processor.set_parameter(band_config.type == SaturationType::Exciter ? 1u : 0u, value)) {
        return false;
      }
      band_config.drive_db = value;
      return true;
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
          // Blended by process(), which reads band_config.mix per block.
          return true;
      }
      return true;
    }
    case 2:
      // output_gain_db is applied by the multiband loop, not the sub-processor.
      if (!numeric::finite(db_to_linear(value))) return false;
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
  return eq::banded_parameter_descriptors(processors_.size(), kBandParamKeys, kBandStride);
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
    if (!numeric::finite(db_to_linear(band.drive_db)) ||
        !numeric::finite(db_to_linear(band.output_gain_db))) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "saturation drive and output gain must produce finite linear gains");
    }
  }
  // The crossover and every band are checked here, so set_config() refuses
  // before it commits anything.
  multiband::Crossover::validate_config(config.crossover);
}

void MultibandSaturation::rebuild_processors() {
  processors_.clear();
  processors_.reserve(config_.bands.size());
  for (const auto& band_config : config_.bands) {
    processors_.push_back(make_processor(band_config));
  }
  band_gates_.assign(processors_.size(), rt::StageGate{});
}

int MultibandSaturation::band_latency_q8(size_t band) const noexcept {
  // A disabled band is summed in unprocessed (process() skips its stage), so it
  // contributes nothing regardless of what the stage would report.
  if (!config_.bands[band].enabled) {
    return 0;
  }
  return processors_[band]->latency_samples_q8();
}

void MultibandSaturation::rebuild_band_compensation() {
  std::vector<int> latencies(processors_.size());
  for (size_t band = 0; band < processors_.size(); ++band) latencies[band] = band_latency_q8(band);
  band_paths_.set_path_latencies_q8(std::move(latencies));
  band_paths_.ensure_channels(static_cast<size_t>(max_working_channels_));
  blend_paths_.resize(processors_.size());
  for (size_t band = 0; band < processors_.size(); ++band) {
    auto& blend = blend_paths_[band];
    if (config_.bands[band].type != SaturationType::Tape) {
      blend = rt::ParallelPaths{};
      continue;
    }
    blend.set_path_latencies_q8({0, processors_[band]->latency_samples_q8()});
    blend.ensure_channels(static_cast<size_t>(max_working_channels_));
  }
}

}  // namespace sonare::mastering::multiband
