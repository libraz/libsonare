#include "mastering/saturation/pedal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "mastering/dynamics/channel_limits.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::mastering::saturation {

namespace {

using sonare::constants::kTwoPiD;

// Input coupling high-pass shared by both pedals.
constexpr double kInputHighpassHz = 16.0;
// Overdrive clip-path high-pass: below it the clip path is at unity gain.
constexpr double kClipHighpassHz = 720.0;
// Distortion op-amp gain-bandwidth product, and the ceiling on the band it leaves.
constexpr double kGainBandwidthHz = 1.0e6;
constexpr double kMaxBandwidthHz = 20000.0;
// Coupling high-pass between the two distortion clippers.
constexpr double kCouplingHighpassHz = 30.0;
// Highest corner a first-order section is built at, as a fraction of its rate.
constexpr double kMaxCornerFraction = 0.45;

// Soft-clip exponent p in x / (1 + |x|^p)^(1/p).
constexpr double kSoftClipExponent = 2.5;
// Antiderivative table: knots over [0, span], Simpson steps per interval.
constexpr int kSoftClipIntervals = 1024;
constexpr double kSoftClipSpan = 32.0;
constexpr int kSoftClipSimpsonSteps = 16;

float corner_w0(double hz, double rate) {
  return static_cast<float>(kTwoPiD * std::min(hz, kMaxCornerFraction * rate) / rate);
}

double soft_clip(double u) {
  return u / std::pow(1.0 + std::pow(std::abs(u), kSoftClipExponent), 1.0 / kSoftClipExponent);
}

struct SoftClipTable {
  std::array<double, kSoftClipIntervals + 1> integral{};
  std::array<double, kSoftClipIntervals + 1> slope{};
};

const SoftClipTable& soft_clip_table() {
  static const SoftClipTable table = [] {
    SoftClipTable t;
    const double h = kSoftClipSpan / kSoftClipIntervals;
    const double step = h / kSoftClipSimpsonSteps;
    for (int k = 0; k <= kSoftClipIntervals; ++k) {
      t.slope[static_cast<size_t>(k)] = soft_clip(k * h);
      if (k == 0) continue;
      const double start = (k - 1) * h;
      double sum = soft_clip(start) + soft_clip(start + h);
      for (int s = 1; s < kSoftClipSimpsonSteps; ++s) {
        sum += (s % 2 == 1 ? 4.0 : 2.0) * soft_clip(start + s * step);
      }
      t.integral[static_cast<size_t>(k)] =
          t.integral[static_cast<size_t>(k - 1)] + sum * step / 3.0;
    }
    return t;
  }();
  return table;
}

// Integral of the soft clip from 0 to u >= 0: cubic Hermite on the table, then
// the tail of 1 - t^-p / p integrated in closed form.
double soft_clip_integral(double u) {
  const SoftClipTable& table = soft_clip_table();
  if (!(u < kSoftClipSpan)) {
    const double tail_power = 1.0 - kSoftClipExponent;
    return table.integral.back() + (u - kSoftClipSpan) +
           (std::pow(u, tail_power) - std::pow(kSoftClipSpan, tail_power)) /
               (kSoftClipExponent * (kSoftClipExponent - 1.0));
  }
  const double h = kSoftClipSpan / kSoftClipIntervals;
  const double position = u / h;
  const auto k = static_cast<size_t>(position);
  const double s = position - static_cast<double>(k);
  const double s2 = s * s;
  const double s3 = s2 * s;
  return (2.0 * s3 - 3.0 * s2 + 1.0) * table.integral[k] +
         (s3 - 2.0 * s2 + s) * h * table.slope[k] + (-2.0 * s3 + 3.0 * s2) * table.integral[k + 1] +
         (s3 - s2) * h * table.slope[k + 1];
}

}  // namespace

float PedalSoftClipNonlinearity::apply(float x) const noexcept {
  return static_cast<float>(soft_clip(static_cast<double>(x)));
}

float PedalSoftClipNonlinearity::antiderivative(float x) const noexcept {
  return static_cast<float>(soft_clip_integral(std::abs(static_cast<double>(x))));
}

namespace detail {

// --- overdrive ---------------------------------------------------------------

void OverdriveCore::prepare(double oversampled_rate) {
  clip_highpass_.set(rt::first_order_highpass(corner_w0(kClipHighpassHz, oversampled_rate)));
}

void OverdriveCore::set_gain_db(float gain_db, double /*oversampled_rate*/) noexcept {
  gain_ = db_to_linear(gain_db);
}

void OverdriveCore::process(float* samples, size_t count) noexcept {
  for (size_t i = 0; i < count; ++i) {
    const float x = samples[i];
    const float clipped = clip_.process(gain_ * clip_highpass_.process(x));
    // ADAA averages adjacent samples; the direct path takes the same average to stay aligned.
    const float direct = 0.5f * (x + previous_direct_);
    previous_direct_ = x;
    samples[i] = direct + clipped;
  }
}

void OverdriveCore::reset() noexcept {
  clip_highpass_.reset();
  clip_.reset();
  previous_direct_ = 0.0f;
}

bool OverdriveCore::state_finite() const noexcept {
  return std::isfinite(clip_highpass_.z1) && std::isfinite(clip_highpass_.z2) &&
         std::isfinite(previous_direct_);
}

// --- distortion --------------------------------------------------------------

void DistortionCore::prepare(double oversampled_rate) {
  coupling_.set(rt::first_order_highpass(corner_w0(kCouplingHighpassHz, oversampled_rate)));
}

void DistortionCore::set_gain_db(float gain_db, double oversampled_rate) noexcept {
  gain_ = db_to_linear(gain_db);
  const double bandwidth_hz = std::min(kGainBandwidthHz / gain_, kMaxBandwidthHz);
  bandwidth_.set(rt::first_order_lowpass(corner_w0(bandwidth_hz, oversampled_rate)));
}

void DistortionCore::process(float* samples, size_t count) noexcept {
  for (size_t i = 0; i < count; ++i) {
    const float limited = bandwidth_.process(gain_ * samples[i]);
    samples[i] = second_clip_.process(coupling_.process(first_clip_.process(limited)));
  }
}

void DistortionCore::reset() noexcept {
  bandwidth_.reset();
  first_clip_.reset();
  coupling_.reset();
  second_clip_.reset();
}

bool DistortionCore::state_finite() const noexcept {
  return std::isfinite(bandwidth_.z1) && std::isfinite(bandwidth_.z2) &&
         std::isfinite(coupling_.z1) && std::isfinite(coupling_.z2);
}

// --- frame -------------------------------------------------------------------

template <typename Core>
Pedal<Core>::Pedal(Config config) : config_(config) {
  validate_config(config_);
  // Integrate the soft-clip table here, on the control thread, not on first use.
  (void)soft_clip_table();
}

template <typename Core>
void Pedal<Core>::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

template <typename Core>
void Pedal<Core>::prepare(double sample_rate, int max_block_size, int max_channels) {
  if (!std::isfinite(sample_rate) || !(sample_rate > 0.0))
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  if (max_block_size < 0)
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  if (max_channels < 1 || max_channels > static_cast<int>(dynamics::kRealtimePreparedChannels)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string("max_channels exceeds ") + Core::kName + " capacity");
  }
  sample_rate_ = sample_rate;
  const auto block = static_cast<size_t>(max_block_size);
  base_scratch_.assign(block, 0.0f);
  up_scratch_.assign(block * kOversampleFactor, 0.0f);
  states_.assign(static_cast<size_t>(max_channels), ChannelState{});
  const double oversampled_rate = sample_rate * kOversampleFactor;
  for (ChannelState& state : states_) {
    oversampler_.prepare_streaming(&state.oversampling, block);
    state.input_highpass.set(rt::first_order_highpass(corner_w0(kInputHighpassHz, sample_rate)));
    state.core.prepare(oversampled_rate);
  }
  build_coefficients();
  prepared_ = true;
  reset();
}

template <typename Core>
void Pedal<Core>::build_coefficients() noexcept {
  const double oversampled_rate = sample_rate_ * kOversampleFactor;
  const rt::BiquadCoeffs tone = rt::first_order_lowpass(corner_w0(config_.tone_hz, sample_rate_));
  for (ChannelState& state : states_) {
    state.core.set_gain_db(config_.gain_db, oversampled_rate);
    state.tone.set(tone);
  }
  built_gain_db_ = config_.gain_db;
  built_tone_hz_ = config_.tone_hz;
}

template <typename Core>
void Pedal<Core>::process(float* const* channels, int num_channels, int num_samples) {
  rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, Core::kName);
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
  if (static_cast<size_t>(num_channels) > states_.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string("num_channels exceeds prepared ") + Core::kName + " state");
  }
  const auto count = static_cast<size_t>(num_samples);
  const size_t oversampled = count * kOversampleFactor;
  if (count > base_scratch_.size()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string("num_samples exceeds prepared ") + Core::kName + " scratch");
  }
  if (config_.gain_db != built_gain_db_ || config_.tone_hz != built_tone_hz_) build_coefficients();
  const float level = db_to_linear(config_.level_db);

  bool discarded = false;
  for (int ch = 0; ch < num_channels; ++ch) {
    ChannelState& state = states_[static_cast<size_t>(ch)];
    float* samples = channels[ch];
    for (size_t i = 0; i < count; ++i) base_scratch_[i] = state.input_highpass.process(samples[i]);
    oversampler_.upsample_to_streaming(base_scratch_.data(), count, up_scratch_.data(),
                                       up_scratch_.size(), &state.oversampling);
    state.core.process(up_scratch_.data(), oversampled);
    oversampler_.downsample_to_streaming(up_scratch_.data(), oversampled, base_scratch_.data(),
                                         base_scratch_.size(), &state.oversampling);
    for (size_t i = 0; i < count; ++i) samples[i] = level * state.tone.process(base_scratch_[i]);
    // Every cell of a channel feeds the next, so one non-finite cell resets them all.
    if (!channel_finite(state)) {
      reset_channel(state);
      discarded = true;
    }
  }
  if (discarded) note_non_finite_discard();
}

template <typename Core>
bool Pedal<Core>::channel_finite(const ChannelState& state) const noexcept {
  const auto finite = [](const std::vector<float>& cells) {
    return std::all_of(cells.begin(), cells.end(), [](float v) { return std::isfinite(v); });
  };
  return std::isfinite(state.input_highpass.z1) && std::isfinite(state.input_highpass.z2) &&
         std::isfinite(state.tone.z1) && std::isfinite(state.tone.z2) &&
         state.core.state_finite() && finite(state.oversampling.up_history) &&
         finite(state.oversampling.down_history);
}

template <typename Core>
void Pedal<Core>::reset_channel(ChannelState& state) noexcept {
  state.input_highpass.reset();
  state.core.reset();
  state.tone.reset();
  oversampler_.reset_streaming(&state.oversampling);
}

template <typename Core>
void Pedal<Core>::reset() {
  for (ChannelState& state : states_) reset_channel(state);
}

template <typename Core>
int Pedal<Core>::latency_samples() const noexcept {
  return latency_samples_q8() >> 8;
}

template <typename Core>
int Pedal<Core>::latency_samples_q8() const noexcept {
  return (oversampler_.streaming_round_trip_latency_samples() << 8) +
         Core::kLatencySamplesQ8 / kOversampleFactor;
}

template <typename Core>
void Pedal<Core>::set_config(const Config& config) {
  validate_config(config);
  config_ = config;
}

template <typename Core>
bool Pedal<Core>::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.gain_db = std::clamp(value, 0.0f, Core::kMaxGainDb);
      return true;
    case 1:
      config_.tone_hz = std::clamp(value, Core::kMinToneHz, Core::kMaxToneHz);
      return true;
    case 2:
      if (!numeric::finite(db_to_linear(value))) return false;
      config_.level_db = value;
      return true;
    default:
      return false;
  }
}

template <typename Core>
std::vector<rt::ParamDescriptor> Pedal<Core>::parameter_descriptors() const {
  return {{"gainDb", 0}, {"toneHz", 1}, {"levelDb", 2}};
}

template <typename Core>
void Pedal<Core>::validate_config(const Config& config) {
  const std::string name = Core::kName;
  if (!(config.gain_db >= 0.0f && config.gain_db <= Core::kMaxGainDb)) {
    throw SonareException(ErrorCode::InvalidParameter, name + " gainDb is out of range");
  }
  if (!(config.tone_hz >= Core::kMinToneHz && config.tone_hz <= Core::kMaxToneHz)) {
    throw SonareException(ErrorCode::InvalidParameter, name + " toneHz is out of range");
  }
  if (!numeric::finite(db_to_linear(config.level_db))) {
    throw SonareException(ErrorCode::InvalidParameter,
                          name + " level must produce a finite linear gain");
  }
}

template class Pedal<OverdriveCore>;
template class Pedal<DistortionCore>;

}  // namespace detail

}  // namespace sonare::mastering::saturation
