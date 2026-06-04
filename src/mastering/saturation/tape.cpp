#include "mastering/saturation/tape.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "rt/biquad_design.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {

using sonare::constants::kTwoPiD;

namespace {

constexpr float kMs = 1.0f;        // saturation magnetization
constexpr float kAlpha = 1.6e-3f;  // inter-domain mean-field coupling
constexpr float kC = 0.4f;         // reversibility ratio in [0, 1]
constexpr int kPreparedChannels = 2;

}  // namespace

Tape::Tape(TapeConfig config) : config_(config), hysteresis_(make_ja_config(config_)) {
  validate_config(config_);
}

void Tape::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  sample_rate_ = sample_rate;
  max_block_size_ = max_block_size;
  update_filters(sample_rate_);
  if (config_.oversample_factor > 1) {
    oversampler_.set_factor(config_.oversample_factor);
  }
  // Preallocate the oversampling scratch up front so the audio-thread process()
  // path never allocates. Sized to the worst case (max block * factor); the
  // factor==1 path leaves these empty. Blocks wider than max_block_size_ throw
  // instead of resizing on the audio thread.
  const size_t scratch = static_cast<size_t>(std::max(0, max_block_size_)) *
                         static_cast<size_t>(std::max(1, config_.oversample_factor));
  up_scratch_.assign(scratch, 0.0f);
  down_scratch_.assign(static_cast<size_t>(std::max(0, max_block_size_)), 0.0f);
  states_.assign(static_cast<size_t>(kPreparedChannels), common::JilesAthertonState{});
  head_bump_.assign(static_cast<size_t>(kPreparedChannels), head_bump_coeffs_);
  gap_state_.assign(static_cast<size_t>(kPreparedChannels), 0.0f);
  prepared_ = true;
  reset();
}

void Tape::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "Tape");
  if (num_channels < 0 || num_samples < 0)
    throw SonareException(ErrorCode::InvalidParameter, "invalid dimensions");
  if (num_channels == 0 || num_samples == 0) return;
  if (channels == nullptr)
    throw SonareException(ErrorCode::InvalidParameter, "channels must not be null");
  ensure_state(num_channels);

  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
  }

  if (config_.oversample_factor <= 1) {
    for (int ch = 0; ch < num_channels; ++ch) {
      auto& state = states_[static_cast<size_t>(ch)];
      for (int i = 0; i < num_samples; ++i) {
        float y = process_sample(state, channels[ch][i]);
        y += head_bump_[static_cast<size_t>(ch)].process(y);
        auto& gap = gap_state_[static_cast<size_t>(ch)];
        gap += gap_loss_coeff_ * (y - gap);
        channels[ch][i] = y * (1.0f - config_.gap_loss) + gap * config_.gap_loss;
      }
    }
    return;
  }

  // Oversample only the stateful J-A core to reduce aliasing at high drive.
  // head_bump and gap_loss stay at base rate. Reuse the preallocated scratch
  // buffers (sized in prepare()) so this audio-thread path never allocates;
  // reject blocks wider than the prepared size instead of resizing here.
  const size_t os_samples =
      static_cast<size_t>(num_samples) * static_cast<size_t>(config_.oversample_factor);
  if (os_samples > up_scratch_.size() || static_cast<size_t>(num_samples) > down_scratch_.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_samples exceeds prepared Tape oversampling scratch");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    auto& state = states_[static_cast<size_t>(ch)];
    oversampler_.upsample_to(channels[ch], static_cast<size_t>(num_samples), up_scratch_.data(),
                             up_scratch_.size());
    for (size_t i = 0; i < os_samples; ++i) {
      up_scratch_[i] = process_sample(state, up_scratch_[i]);
    }
    oversampler_.downsample_to(up_scratch_.data(), os_samples, down_scratch_.data(),
                               down_scratch_.size());
    for (int i = 0; i < num_samples; ++i) {
      float y = down_scratch_[static_cast<size_t>(i)];
      y += head_bump_[static_cast<size_t>(ch)].process(y);
      auto& gap = gap_state_[static_cast<size_t>(ch)];
      gap += gap_loss_coeff_ * (y - gap);
      channels[ch][i] = y * (1.0f - config_.gap_loss) + gap * config_.gap_loss;
    }
  }
}

void Tape::reset() {
  for (auto& s : states_) {
    common::JilesAtherton::reset(s);
  }
  for (auto& filter : head_bump_) filter.reset();
  std::fill(gap_state_.begin(), gap_state_.end(), 0.0f);
}

void Tape::set_config(const TapeConfig& config) {
  validate_config(config);
  config_ = config;
  hysteresis_.set_config(make_ja_config(config_));
  if (config_.oversample_factor > 1) {
    oversampler_.set_factor(config_.oversample_factor);
  }
  if (prepared_) {
    update_filters(sample_rate_);
    // oversample_factor may have changed; resize the scratch on this
    // control-thread path (allocation here is acceptable, never on the audio
    // thread). Matches the sizing done in prepare().
    const size_t scratch = static_cast<size_t>(std::max(0, max_block_size_)) *
                           static_cast<size_t>(std::max(1, config_.oversample_factor));
    up_scratch_.assign(scratch, 0.0f);
    down_scratch_.assign(static_cast<size_t>(std::max(0, max_block_size_)), 0.0f);
  }
}

bool Tape::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.drive_db = value;
      return true;
    case 1:
      config_.saturation = std::clamp(value, 0.0f, 1.0f);
      hysteresis_.set_config(make_ja_config(config_));
      return true;
    case 2:
      config_.hysteresis = std::clamp(value, 0.0f, 1.0f);
      hysteresis_.set_config(make_ja_config(config_));
      return true;
    case 3:
      config_.output_gain_db = value;
      return true;
    case 4:
      config_.speed_ips = std::max(value, std::numeric_limits<float>::min());
      if (prepared_) update_filters(sample_rate_);
      return true;
    case 5:
      config_.head_bump_db = std::max(0.0f, value);
      if (prepared_) update_filters(sample_rate_);
      return true;
    case 6:
      config_.bias = value;
      return true;
    case 7:
      config_.gap_loss = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

void Tape::validate_config(const TapeConfig& config) {
  if (config.saturation < 0.0f || config.saturation > 1.0f || config.hysteresis < 0.0f ||
      config.hysteresis > 1.0f || config.speed_ips <= 0.0f || config.head_bump_db < 0.0f ||
      config.gap_loss < 0.0f || config.gap_loss > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid tape configuration");
  }
  if (config.oversample_factor != 1 && config.oversample_factor != 2 &&
      config.oversample_factor != 4) {
    throw SonareException(ErrorCode::InvalidParameter, "tape oversample_factor must be 1, 2, or 4");
  }
}

common::JilesAthertonConfig Tape::make_ja_config(const TapeConfig& config) {
  // Map user controls to J-A parameters:
  //   a (anhysteretic shape) is smaller when saturation is large → earlier saturation.
  //   k (loss/coercivity)    is larger when hysteresis is large → wider loop.
  const float a = std::max(0.05f, 0.5f - 0.4f * config.saturation);
  const float k = std::max(0.01f, 0.05f + 0.30f * config.hysteresis);
  return {kMs, a, k, kAlpha, kC};
}

float Tape::process_sample(common::JilesAthertonState& state, float input) const {
  const float drive = db_to_linear(config_.drive_db);
  const float H = input * drive + config_.bias * 0.1f;
  return hysteresis_.process(state, H) * db_to_linear(config_.output_gain_db);
}

void Tape::ensure_state(int num_channels) {
  if (static_cast<size_t>(num_channels) > states_.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "num_channels exceeds prepared Tape state");
  }
}

void Tape::update_filters(double sample_rate) {
  const float speed_scale = 15.0f / std::max(config_.speed_ips, 1.0f);
  const float frequency =
      std::clamp(80.0f * speed_scale, 20.0f, static_cast<float>(sample_rate * 0.45));
  const float gain = db_to_linear(config_.head_bump_db) - 1.0f;
  const float q = 1.0f;
  const float w0 = static_cast<float>(kTwoPiD * frequency / sample_rate);
  const auto coeffs = rt::rbj_bandpass(w0, q);
  head_bump_coeffs_.c.b0 = coeffs.b0 * gain;
  head_bump_coeffs_.c.b1 = coeffs.b1 * gain;
  head_bump_coeffs_.c.b2 = coeffs.b2 * gain;
  head_bump_coeffs_.c.a1 = coeffs.a1;
  head_bump_coeffs_.c.a2 = coeffs.a2;
  for (auto& filter : head_bump_) {
    const float z1 = filter.z1;
    const float z2 = filter.z2;
    filter = head_bump_coeffs_;
    filter.z1 = z1;
    filter.z2 = z2;
  }
  const float gap_cutoff = std::clamp(18000.0f * config_.speed_ips / 15.0f, 1000.0f,
                                      static_cast<float>(sample_rate * 0.45));
  gap_loss_coeff_ = static_cast<float>(1.0 - std::exp(-kTwoPiD * gap_cutoff / sample_rate));
}

}  // namespace sonare::mastering::saturation
