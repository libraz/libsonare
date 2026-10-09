#include "mastering/saturation/bitcrusher.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/non_finite_state.h"

namespace sonare::mastering::saturation {

using sonare::constants::kDefaultDawSampleRate;

using sonare::discard_if_non_finite;
using sonare::discard_run_if_non_finite;
using sonare::constants::kPiD;
using sonare::constants::kTwoPiD;

namespace {

constexpr std::array<float, 9> kNoiseShapingCoeffs = {2.412f,  -3.370f, 3.937f,  -4.174f, 3.353f,
                                                      -2.205f, 1.281f,  -0.569f, 0.0847f};

// A whole period of the hold rate: the phase starts here so the first sample of
// a run is latched, which is where the sample-count cadence starts too.
constexpr double kLatchNextSample = 1.0;

// The generators are the classic engine's, so one noise source has one design.
constexpr uint32_t kNoiseSeed = 0x9E3779B9u;
constexpr uint32_t kNoiseAux[] = {1u, 2u, 3u, 4u, 5u};  // radio, white, pink, disc, hum
constexpr double kPinkPole[3] = {0.99765, 0.96300, 0.57000};
constexpr double kPinkWeight[3] = {0.0990460, 0.2965164, 1.0526913};
constexpr double kPinkDirect = 0.1848;
constexpr double kPinkScale = 0.336;  // brings the pink sum to white noise's RMS
constexpr double kHumSecond = 0.5;
constexpr double kHumThird = 0.25;
constexpr double kHumNorm = 1.75;
constexpr double kMaxCornerRatio = 0.49;  // a corner stays below this fraction of the rate

uint32_t xorshift32(uint32_t& s) noexcept {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

/// The top 16 bits of a draw as a signed word over full scale.
double noise_word(uint32_t u) noexcept {
  return static_cast<double>(static_cast<int16_t>(static_cast<uint16_t>(u >> 16))) / 32768.0;
}

bool valid_hz(float hz) noexcept { return std::isfinite(hz) && hz >= 0.0f; }
bool valid_level(float level) noexcept {
  return std::isfinite(level) && level >= 0.0f && level <= 1.0f;
}

}  // namespace

BitCrusher::BitCrusher(BitCrusherConfig config) : config_(config) { validate_config(config_); }

void BitCrusher::prepare(double sample_rate, int max_block_size) {
  validate_prepare_args(sample_rate, max_block_size);
  prepared_ = true;
  sample_rate_ = sample_rate;
  update_hold_increment();
  update_coefficients();
  // Preallocate per-channel state so process() never resizes on the audio
  // thread (matches Tube/AmpSim).
  const size_t n = dynamics::kRealtimePreparedChannels;
  noise_.assign(static_cast<size_t>(std::max(max_block_size, 1)), 0.0f);
  pre_state_.assign(n, 0.0f);
  post_state_.assign(n, 0.0f);
  held_.assign(n, 0.0f);
  counters_.assign(n, 0);
  hold_phase_.assign(n, kLatchNextSample);
  rng_state_.assign(n, 0);
  error_history_.assign(n, {});
  reset();
}

void BitCrusher::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "BitCrusher");
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
  ensure_state(num_channels);
  // One cadence or the other: a hold rate in hertz replaces the sample count.
  const bool held_by_rate = hold_increment_ > 0.0;
  const bool prefilter = pre_filter_gate_.admit(
      pre_gain_ > 0.0, [this] { std::fill(pre_state_.begin(), pre_state_.end(), 0.0f); });
  const bool postfilter = post_filter_gate_.admit(
      post_gain_ > 0.0, [this] { std::fill(post_state_.begin(), post_state_.end(), 0.0f); });
  const bool highpass = config_.filter_type == BitCrusherFilterType::kHighpass;
  const effects::common::MixGains mix =
      effects::common::mix_gains(config_.mix_law, std::clamp(config_.mix, 0.0f, 1.0f));
  const bool noisy = config_.radio_noise_level * config_.noise_detune > 0.0f ||
                     config_.wp_noise_level > 0.0f || config_.disc_noise_level > 0.0f ||
                     config_.hum_level > 0.0f;
  bool discarded = false;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (channels[ch] == nullptr)
      throw SonareException(ErrorCode::InvalidParameter, "channel buffer must not be null");
  }
  // The noise is drawn once per frame and shared by the channels, in spans no
  // longer than the scratch prepare() sized.
  const int span = static_cast<int>(noise_.size());
  for (int start = 0; start < num_samples; start += span) {
    const int count = std::min(span, num_samples - start);
    if (noisy) render_noise(count);
    for (int ch = 0; ch < num_channels; ++ch) {
      const size_t c = static_cast<size_t>(ch);
      float* const data = channels[ch] + start;
      for (int i = 0; i < count; ++i) {
        bool latch = false;
        if (held_by_rate) {
          latch = hold_phase_[c] >= 1.0;
          if (latch) hold_phase_[c] -= 1.0;
          hold_phase_[c] += hold_increment_;
        } else {
          latch = counters_[c] == 0;
          counters_[c] = (counters_[c] + 1) % config_.downsample_factor;
        }
        const float dry = data[i];
        float in = dry;
        if (prefilter) in = lowpass_tick(pre_gain_, in, pre_state_[c]);
        if (latch) {
          // kOff is a branch and not a transparent setting: a quantizer asked for
          // a very fine step still moves the sample it was given.
          held_[c] = config_.quantizer_mode == QuantizerMode::kOff
                         ? in
                         : quantize(in, config_.bit_depth, ch);
        }
        float wet = held_[c];
        if (postfilter) {
          const float low = lowpass_tick(post_gain_, wet, post_state_[c]);
          wet = highpass ? wet - low : low;
        }
        if (noisy) wet += noise_[static_cast<size_t>(i)];
        data[i] = dry * mix.dry + wet * mix.wet;
      }
      discarded |= discard_non_finite_state(c);
    }
  }
  if (config_.mono && num_channels > 1) {
    const float scale = 1.0f / static_cast<float>(num_channels);
    for (int i = 0; i < num_samples; ++i) {
      float sum = 0.0f;
      for (int ch = 0; ch < num_channels; ++ch) sum += channels[ch][i];
      const float mono = sum * scale;
      for (int ch = 0; ch < num_channels; ++ch) channels[ch][i] = mono;
    }
  }
  if (discarded) note_non_finite_discard();
}

bool BitCrusher::discard_non_finite_state(size_t channel) noexcept {
  // Twelve floats per channel, once per block. The nine shaping taps are the error
  // the quantizer feeds back into its own input, so one non-finite tap re-poisons
  // every later block; they are one history and any of them returns all of them
  // to rest. The held sample only survives to the end of its downsample period,
  // and is scrubbed so that period does not start from it.
  auto& history = error_history_[channel];
  bool discarded = discard_run_if_non_finite(history.begin(), history.end(), 0.0f);
  discarded |= discard_if_non_finite(held_[channel], 0.0f);
  discarded |= discard_if_non_finite(pre_state_[channel], 0.0f);
  discarded |= discard_if_non_finite(post_state_[channel], 0.0f);
  return discarded;
}

void BitCrusher::reset() {
  std::fill(held_.begin(), held_.end(), 0.0f);
  std::fill(counters_.begin(), counters_.end(), 0);
  std::fill(hold_phase_.begin(), hold_phase_.end(), kLatchNextSample);
  std::fill(pre_state_.begin(), pre_state_.end(), 0.0f);
  std::fill(post_state_.begin(), post_state_.end(), 0.0f);
  std::fill(std::begin(lpf_state_), std::end(lpf_state_), 0.0f);
  for (size_t k = 0; k < kSourceCount; ++k) {
    sources_[k] = NoiseSource{};
    sources_[k].rng = kNoiseSeed ^ kNoiseAux[k];
  }
  for (size_t ch = 0; ch < rng_state_.size(); ++ch) {
    rng_state_[ch] = config_.dither_seed + static_cast<uint32_t>(ch * 747796405u);
  }
  for (auto& history : error_history_) {
    history.fill(0.0f);
  }
}

void BitCrusher::set_config(const BitCrusherConfig& config) {
  validate_config(config);
  // Hold rate and filter gains are scalars over running state; nothing resets.
  rt::apply_config_diff(config_, config, [this](const rt::ConfigDiff<BitCrusherConfig>&) {
    update_hold_increment();
    update_coefficients();
  });
}

bool BitCrusher::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      // Match validate_config's [2, 24] integer range for bit_depth.
      config_.bit_depth = std::clamp(static_cast<int>(std::lround(value)), kBitCrusherMinBitDepth,
                                     kBitCrusherMaxBitDepth);
      return true;
    case 1:
      config_.mix = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 2:
      // Refused rather than clamped: a rate the increment cannot be derived from
      // would otherwise arrive as an in-domain hold nothing downstream can
      // separate from one that was asked for.
      if (value < 0.0f) return false;
      config_.hold_hz = value;
      // In place, and the phase runs on: a moved rate changes when the next
      // sample is latched, not which one is being held now.
      update_hold_increment();
      return true;
    case 3:
    case 4:
    case 5:
    case 6:
    case 7: {
      float& level = param_id == 3   ? config_.radio_noise_level
                     : param_id == 4 ? config_.wp_noise_level
                     : param_id == 5 ? config_.disc_noise_level
                     : param_id == 6 ? config_.hum_level
                                     : config_.noise_detune;
      level = std::clamp(value, 0.0f, 1.0f);
      return true;
    }
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14: {
      if (!valid_hz(value)) return false;
      float& hz = param_id == 8    ? config_.noise_lpf_hz
                  : param_id == 9  ? config_.wp_noise_lpf_hz
                  : param_id == 10 ? config_.disc_noise_lpf_hz
                  : param_id == 11 ? config_.hum_lpf_hz
                  : param_id == 12 ? config_.pre_filter_hz
                  : param_id == 13 ? config_.post_filter_hz
                                   : config_.hum_hz;
      hz = value;
      update_coefficients();
      return true;
    }
    case 15:
    case 18:
      (param_id == 15 ? config_.wp_noise_pink : config_.mono) = value >= 0.5f;
      return true;
    case 16:
    case 17: {
      // Refused rather than clamped, like the other enumerations.
      const float rounded = std::round(value);
      if (rounded != value) return false;
      if (param_id == 16) {
        if (rounded < 0.0f || rounded >= static_cast<float>(kBitCrusherDiscTypeCount)) return false;
        config_.disc_type = static_cast<BitCrusherDiscType>(static_cast<int>(rounded));
      } else {
        if (rounded < 0.0f || rounded > 2.0f) return false;
        config_.filter_type = static_cast<BitCrusherFilterType>(static_cast<int>(rounded));
      }
      update_coefficients();
      return true;
    }
    case 19: {
      // A ladder entry is a named set; refuse an unnamed value rather than pick a neighbour.
      const float rounded = std::round(value);
      if (rounded != value || rounded < 0.0f ||
          rounded > static_cast<float>(kTypeLadderHoldHz.size())) {
        return false;
      }
      config_.type_ladder = static_cast<int>(rounded);
      update_hold_increment();
      return true;
    }
    case 20:
      return effects::common::mix_law_from_value(value, &config_.mix_law);
    default:
      return false;
  }
}

bool BitCrusher::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  // Every one is an in-place scalar update on preallocated state. hold_hz is a
  // frequency rather than a structure: it moves where the aperture null sits
  // without changing what the processor is made of. quantizer_mode remains a
  // construction choice; unknown ids are rejected by set_parameter before this
  // query is reached.
  return param_id <= 20;
}

std::vector<rt::ParamDescriptor> BitCrusher::parameter_descriptors() const {
  return {{"bitDepth", 0},        {"mix", 1},
          {"holdHz", 2},          {"radioNoiseLevel", 3},
          {"wpNoiseLevel", 4},    {"discNoiseLevel", 5},
          {"humLevel", 6},        {"noiseDetune", 7},
          {"noiseLpfHz", 8},      {"wpNoiseLpfHz", 9},
          {"discNoiseLpfHz", 10}, {"humLpfHz", 11},
          {"preFilterHz", 12},    {"postFilterHz", 13},
          {"humHz", 14},          {"wpNoisePink", 15},
          {"discType", 16},       {"filterType", 17},
          {"mono", 18},           {"typeLadder", 19},
          {"mixLaw", 20}};
}

void BitCrusher::validate_config(const BitCrusherConfig& config) {
  if (config.bit_depth < kBitCrusherMinBitDepth || config.bit_depth > kBitCrusherMaxBitDepth ||
      config.downsample_factor < 1 || config.mix < 0.0f || config.mix > 1.0f ||
      !std::isfinite(config.hold_hz) || config.hold_hz < 0.0f ||
      !valid_level(config.radio_noise_level) || !valid_level(config.wp_noise_level) ||
      !valid_level(config.disc_noise_level) || !valid_level(config.hum_level) ||
      !valid_level(config.noise_detune) || static_cast<int>(config.disc_type) < 0 ||
      static_cast<int>(config.disc_type) >= kBitCrusherDiscTypeCount || !valid_hz(config.hum_hz) ||
      !valid_hz(config.noise_lpf_hz) || !valid_hz(config.wp_noise_lpf_hz) ||
      !valid_hz(config.disc_noise_lpf_hz) || !valid_hz(config.hum_lpf_hz) ||
      !valid_hz(config.pre_filter_hz) || !valid_hz(config.post_filter_hz) ||
      config.type_ladder < 0 || config.type_ladder > static_cast<int>(kTypeLadderHoldHz.size()) ||
      static_cast<int>(config.filter_type) < 0 || static_cast<int>(config.filter_type) > 2) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid bitcrusher configuration");
  }
}

void BitCrusher::update_hold_increment() noexcept {
  // A hold rate at or above the host's rate is no hold at all: a whole period
  // per sample latches every sample, which is what the increment is capped to.
  const double rate = sample_rate_ > 0.0 ? sample_rate_ : kDefaultDawSampleRate;
  const float hold_hz = config_.type_ladder > 0
                            ? kTypeLadderHoldHz[static_cast<size_t>(config_.type_ladder - 1)]
                            : config_.hold_hz;
  const double increment = static_cast<double>(hold_hz) / rate;
  hold_increment_ = increment > 0.0 ? std::min(increment, 1.0) : 0.0;
}

double BitCrusher::one_pole_gain(float corner_hz) const noexcept {
  if (!(corner_hz > 0.0f)) return 0.0;
  const double hz = std::min(static_cast<double>(corner_hz), kMaxCornerRatio * sample_rate_);
  const double t = std::tan(kPiD * hz / sample_rate_);
  return t / (1.0 + t);
}

float BitCrusher::lowpass_tick(double g, float x, float& state) noexcept {
  const float v = (x - state) * static_cast<float>(g);
  const float low = v + state;
  state = low + v;
  return low;
}

void BitCrusher::update_coefficients() noexcept {
  const double rate = sample_rate_ > 0.0 ? sample_rate_ : kDefaultDawSampleRate;
  pre_gain_ = one_pole_gain(config_.pre_filter_hz);
  post_gain_ = config_.filter_type == BitCrusherFilterType::kOff
                   ? 0.0
                   : one_pole_gain(config_.post_filter_hz);
  lpf_gain_[kRadioLpf] = one_pole_gain(config_.noise_lpf_hz);
  lpf_gain_[kWpLpf] = one_pole_gain(config_.wp_noise_lpf_hz);
  lpf_gain_[kDiscLpf] = one_pole_gain(config_.disc_noise_lpf_hz);
  lpf_gain_[kHumLpf] = one_pole_gain(config_.hum_lpf_hz);
  radio_shape_pole_ = std::exp(-kTwoPiD * static_cast<double>(kRadioNoiseCornerHz) / rate);
  // A click's chance per sample is its rate over the sample rate, so the density
  // per second holds at every rate. Scaled to the full 32-bit draw, whose step is
  // far below the chance, so the threshold keeps its fraction.
  disc_chance_ = static_cast<double>(kDiscClickRateHz[static_cast<size_t>(config_.disc_type)]) *
                 4294967296.0 / rate;
  hum_step_ = static_cast<double>(config_.hum_hz) / rate;
}

void BitCrusher::render_noise(int count) noexcept {
  const float radio_level = config_.radio_noise_level * config_.noise_detune;
  for (int i = 0; i < count; ++i) {
    float sum = 0.0f;
    if (radio_level > 0.0f) {
      auto& g = sources_[kRadio];
      g.mem[0] =
          (1.0 - radio_shape_pole_) * noise_word(xorshift32(g.rng)) + radio_shape_pole_ * g.mem[0];
      float v = static_cast<float>(g.mem[0]);
      if (lpf_gain_[kRadioLpf] > 0.0)
        v = lowpass_tick(lpf_gain_[kRadioLpf], v, lpf_state_[kRadioLpf]);
      sum += v * radio_level;
    }
    if (config_.wp_noise_level > 0.0f) {
      double raw;
      if (config_.wp_noise_pink) {
        auto& g = sources_[kPink];
        const double w = noise_word(xorshift32(g.rng));
        for (int k = 0; k < 3; ++k) g.mem[k] = kPinkPole[k] * g.mem[k] + w * kPinkWeight[k];
        raw = (g.mem[0] + g.mem[1] + g.mem[2] + w * kPinkDirect) * kPinkScale;
      } else {
        raw = noise_word(xorshift32(sources_[kWhite].rng));
      }
      float v = static_cast<float>(raw);
      if (lpf_gain_[kWpLpf] > 0.0) v = lowpass_tick(lpf_gain_[kWpLpf], v, lpf_state_[kWpLpf]);
      sum += v * config_.wp_noise_level;
    }
    if (config_.disc_noise_level > 0.0f) {
      auto& g = sources_[kDisc];
      const uint32_t u = xorshift32(g.rng);
      float v = static_cast<double>(u) < disc_chance_
                    ? static_cast<float>(noise_word(xorshift32(g.rng)))
                    : 0.0f;
      if (lpf_gain_[kDiscLpf] > 0.0) v = lowpass_tick(lpf_gain_[kDiscLpf], v, lpf_state_[kDiscLpf]);
      sum += v * config_.disc_noise_level;
    }
    if (config_.hum_level > 0.0f) {
      auto& g = sources_[kHum];
      const double t = kTwoPiD * g.mem[0];
      float v = static_cast<float>(
          (std::sin(t) + kHumSecond * std::sin(2.0 * t) + kHumThird * std::sin(3.0 * t)) /
          kHumNorm);
      g.mem[0] += hum_step_;
      g.mem[0] -= std::floor(g.mem[0]);
      if (lpf_gain_[kHumLpf] > 0.0) v = lowpass_tick(lpf_gain_[kHumLpf], v, lpf_state_[kHumLpf]);
      sum += v * config_.hum_level;
    }
    noise_[static_cast<size_t>(i)] = sum;
  }
}

float BitCrusher::quantize(float sample, int bit_depth, int channel) {
  const float levels = std::pow(2.0f, static_cast<float>(bit_depth - 1)) - 1.0f;
  float shaped = 0.0f;
  if (config_.dither_type == final::DitherType::NoiseShaped) {
    auto& history = error_history_[static_cast<size_t>(channel)];
    for (size_t i = 0; i < history.size(); ++i) {
      shaped += kNoiseShapingCoeffs[i] * history[i];
    }
  }
  const float dithered = sample + dither_noise(channel) / std::max(levels, 1.0f) + shaped / levels;
  const float clamped = std::clamp(dithered, -1.0f, 1.0f);
  const float quantized = std::round(clamped * levels) / levels;
  if (config_.dither_type == final::DitherType::NoiseShaped) {
    auto& history = error_history_[static_cast<size_t>(channel)];
    const float error = (clamped - quantized) * levels;
    for (size_t i = history.size() - 1; i > 0; --i) {
      history[i] = history[i - 1];
    }
    history[0] = error;
  }
  return quantized;
}

float BitCrusher::dither_noise(int channel) {
  if (config_.dither_type == final::DitherType::None) {
    return 0.0f;
  }
  auto& state = rng_state_[static_cast<size_t>(channel)];
  const auto uniform = [&state] {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>((state >> 8) & 0x00FFFFFFu) / 16777216.0f - 0.5f;
  };
  if (config_.dither_type == final::DitherType::Rpdf) {
    return uniform();
  }
  return uniform() + uniform();
}

void BitCrusher::ensure_state(int num_channels) {
  // prepare() preallocates kRealtimePreparedChannels; only grow (control thread)
  // if a caller exceeds it, preserving existing channel state instead of wiping
  // every channel. Seed the newly added channels' dither RNG to match reset().
  const auto n = static_cast<size_t>(num_channels);
  if (held_.size() < n) {
    const size_t old = held_.size();
    held_.resize(n, 0.0f);
    pre_state_.resize(n, 0.0f);
    post_state_.resize(n, 0.0f);
    counters_.resize(n, 0);
    hold_phase_.resize(n, kLatchNextSample);
    rng_state_.resize(n, 0);
    error_history_.resize(n, {});
    for (size_t ch = old; ch < n; ++ch) {
      rng_state_[ch] = config_.dither_seed + static_cast<uint32_t>(ch * 747796405u);
    }
  }
}

}  // namespace sonare::mastering::saturation
