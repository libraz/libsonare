#include "mastering/stereo/binaural_panner.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>

#include "core/fft.h"
#include "core/resample.h"
#include "rt/fractional_delay.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/non_finite_state.h"

namespace sonare::mastering::stereo {
namespace {

namespace ring {
#include "mastering/stereo/binaural_ring.inc"
}  // namespace ring

using sonare::constants::kButterworthQ;
using sonare::constants::kTwoPi;
using sonare::constants::kTwoPiD;

constexpr int kHalfRing = ring::kRingAzimuths / 2;
constexpr float kFullTurnDeg = 360.0f;
constexpr float kHalfTurnDeg = 180.0f;
constexpr float kMaxTurnRateHz = 10.0f;
// Glide of an azimuth change and fade of an output change, in seconds.
constexpr double kGlideSeconds = 0.020;
// Loudspeakers sit at +/-30 degrees; the canceller inverts the ring's own plant there.
constexpr float kSpeakerAzimuthDeg = 30.0f;
// Canceller length at the ring rate; its modelling delay is half of it.
constexpr int kCancellerTapsAtRingRate = 128;
// Tikhonov weight, relative to the ring's unit frontal DC gain: the value at which the mean
// crosstalk reduction through the ring's +/-30 degree plant (200 Hz - 8 kHz, 48 kHz, 128 taps)
// peaks, 19.1 dB; below it truncation loses more than regularisation costs above it.
constexpr float kCancellerRegularization = 0.08f;
// Below this the canceller is crossed out (LR4), so the low end stays uncancelled.
constexpr float kCancellerCrossoverHz = 200.0f;
// Design transform length, as a multiple of the canceller length.
constexpr int kCancellerDesignOversampling = 32;
// Q8.8 fixed point of the shared Lagrange kernel.
constexpr float kDelayQ8 = 256.0f;

float wrap_signed_deg(float degrees) noexcept {
  float wrapped = std::fmod(degrees + kHalfTurnDeg, kFullTurnDeg);
  if (wrapped < 0.0f) wrapped += kFullTurnDeg;
  return wrapped - kHalfTurnDeg;
}

double wrap_turn_deg(double degrees) noexcept {
  double wrapped = std::fmod(degrees, static_cast<double>(kFullTurnDeg));
  return wrapped < 0.0 ? wrapped + kFullTurnDeg : wrapped;
}

size_t next_power_of_two(size_t value) {
  size_t out = 1;
  while (out < value) out <<= 1;
  return out;
}

float dot(const float* a, const float* b, int n) noexcept {
  float sum = 0.0f;
  for (int k = 0; k < n; ++k) sum += a[k] * b[k];
  return sum;
}

}  // namespace

BinauralPanner::BinauralPanner(BinauralPannerConfig config) : config_(config) {
  validate_config(config_);
  config_.azimuth_deg = wrap_signed_deg(config_.azimuth_deg);
}

void BinauralPanner::validate_config(const BinauralPannerConfig& config) {
  if (!std::isfinite(config.azimuth_deg)) {
    throw SonareException(ErrorCode::InvalidParameter, "binaural azimuthDeg must be finite");
  }
  if (!(config.turn_rate_hz >= 0.0f && config.turn_rate_hz <= kMaxTurnRateHz)) {
    throw SonareException(ErrorCode::InvalidParameter, "binaural turnRateHz must be in [0, 10]");
  }
  if (!(config.dry_wet >= 0.0f && config.dry_wet <= 1.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "binaural dryWet must be in [0, 1]");
  }
  const int output = static_cast<int>(config.output);
  if (output < 0 || output >= kBinauralOutputCount) {
    throw SonareException(ErrorCode::InvalidParameter, "binaural output must be 0 or 1");
  }
}

void BinauralPanner::prepare(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  sample_rate_ = sample_rate;
  const int host_rate = static_cast<int>(std::lround(sample_rate));
  const double ratio = sample_rate / static_cast<double>(ring::kRingSampleRate);

  // The ring's responses resampled to the host rate; the ITD scales with the rate.
  std::vector<float> stored(ring::kRingTaps);
  ring_.clear();
  taps_ = 0;
  for (int az = 0; az < ring::kRingAzimuths; ++az) {
    for (int t = 0; t < ring::kRingTaps; ++t) {
      stored[static_cast<size_t>(t)] =
          static_cast<float>(ring::kRingHrir[az][t]) * ring::kRingScale;
    }
    const std::vector<float> host =
        resample_impulse_response(stored.data(), stored.size(), ring::kRingSampleRate, host_rate);
    if (taps_ == 0) {
      taps_ = static_cast<int>(host.size());
      ring_.assign(static_cast<size_t>(ring::kRingAzimuths) * static_cast<size_t>(taps_), 0.0f);
    }
    std::copy_n(host.begin(), std::min(host.size(), static_cast<size_t>(taps_)),
                ring_.begin() + static_cast<std::ptrdiff_t>(az) * taps_);
  }
  itd_.resize(kHalfRing + 1);
  float max_itd = 0.0f;
  for (int i = 0; i <= kHalfRing; ++i) {
    itd_[static_cast<size_t>(i)] = static_cast<float>(ring::kRingItdSamples[i] * ratio);
    max_itd = std::max(max_itd, std::fabs(itd_[static_cast<size_t>(i)]));
  }

  // Four Lagrange taps past the longest ITD.
  mono_.assign(next_power_of_two(static_cast<size_t>(std::ceil(max_itd)) + 4), 0.0f);
  for (auto& history : ear_history_) history.assign(2 * static_cast<size_t>(taps_), 0.0f);
  ramp_samples_ = std::max(1, static_cast<int>(std::lround(kGlideSeconds * sample_rate)));

  xtc_taps_ = std::max(2, 2 * static_cast<int>(std::lround(kCancellerTapsAtRingRate * ratio / 2)));
  xtc_delay_ = xtc_taps_ / 2;
  design_canceller();
  for (auto& history : ms_history_) history.assign(2 * static_cast<size_t>(xtc_taps_), 0.0f);
  const float w0 = static_cast<float>(kTwoPiD * kCancellerCrossoverHz / sample_rate);
  const rt::BiquadCoeffs low = rt::rbj_lowpass(w0, kButterworthQ);
  const rt::BiquadCoeffs high = rt::rbj_highpass(w0, kButterworthQ);
  for (int ch = 0; ch < 2; ++ch) {
    for (int stage = 0; stage < 2; ++stage) {
      low_[ch][stage].set(low);
      high_[ch][stage].set(high);
    }
  }
  for (auto& line : dry_) line.assign(static_cast<size_t>(xtc_delay_) + 1, 0.0f);
  prepared_ = true;
  reset();
}

void BinauralPanner::design_canceller() {
  // Symmetric plant [[Hi, Hc], [Hc, Hi]] diagonalised by mid/side: each of Hi + Hc and
  // Hi - Hc gets its own regularised inverse conj(X) / (|X|^2 + beta), delayed to centre.
  const int n = static_cast<int>(next_power_of_two(
      static_cast<size_t>(kCancellerDesignOversampling) * static_cast<size_t>(xtc_taps_)));
  const int bins = n / 2 + 1;
  FFT fft(n);
  std::vector<float> buffer(static_cast<size_t>(n), 0.0f);
  std::vector<std::complex<float>> ipsi(static_cast<size_t>(bins));
  std::vector<std::complex<float>> contra(static_cast<size_t>(bins));
  const int speaker = static_cast<int>(std::lround(kSpeakerAzimuthDeg / ring::kRingAzimuthStepDeg));
  const auto load = [&](int az, std::vector<std::complex<float>>& out) {
    std::fill(buffer.begin(), buffer.end(), 0.0f);
    std::copy_n(ring_.begin() + static_cast<std::ptrdiff_t>(az) * taps_, std::min(taps_, n),
                buffer.begin());
    fft.forward(buffer.data(), out.data());
  };
  load(speaker, ipsi);
  load(ring::kRingAzimuths - speaker, contra);
  const double lag = static_cast<double>(itd_[static_cast<size_t>(speaker)]);
  for (int k = 0; k < bins; ++k) {
    const double omega = kTwoPiD * k / n;
    contra[static_cast<size_t>(k)] *= std::polar(1.0f, static_cast<float>(-omega * lag));
  }

  const int fade = std::max(1, xtc_taps_ / 8);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(bins));
  for (int part = 0; part < 2; ++part) {
    const float sign = part == 0 ? 1.0f : -1.0f;
    for (int k = 0; k < bins; ++k) {
      const auto i = static_cast<size_t>(k);
      const std::complex<float> x = ipsi[i] + sign * contra[i];
      const double omega = kTwoPiD * k / n;
      const std::complex<float> centre = std::polar(1.0f, static_cast<float>(-omega * xtc_delay_));
      spectrum[i] = std::conj(x) * centre / (std::norm(x) + kCancellerRegularization);
    }
    fft.inverse(spectrum.data(), buffer.data());
    std::vector<float>& taps = part == 0 ? xtc_mid_ : xtc_side_;
    taps.assign(buffer.begin(), buffer.begin() + xtc_taps_);
    // Half-Hann fades over the outer eighths keep truncation from ringing.
    for (int t = 0; t < fade; ++t) {
      const float g = 0.5f - 0.5f * std::cos(kTwoPi * 0.5f * (t + 0.5f) / fade);
      taps[static_cast<size_t>(t)] *= g;
      taps[static_cast<size_t>(xtc_taps_ - 1 - t)] *= g;
    }
  }
}

void BinauralPanner::reset() {
  std::fill(mono_.begin(), mono_.end(), 0.0f);
  mono_write_ = 0;
  for (auto& history : ear_history_) std::fill(history.begin(), history.end(), 0.0f);
  ear_pos_ = 0;
  for (auto& history : ms_history_) std::fill(history.begin(), history.end(), 0.0f);
  ms_pos_ = 0;
  for (auto& line : dry_) std::fill(line.begin(), line.end(), 0.0f);
  dry_pos_ = 0;
  for (int ch = 0; ch < 2; ++ch) {
    for (int stage = 0; stage < 2; ++stage) {
      low_[ch][stage].reset();
      high_[ch][stage].reset();
    }
  }
  ramp_remaining_ = 0;
  primed_ = false;
}

int BinauralPanner::tail_samples() const noexcept {
  const float max_itd = itd_.empty() ? 0.0f : *std::max_element(itd_.begin(), itd_.end());
  return taps_ + static_cast<int>(std::ceil(max_itd)) + 4 + xtc_taps_;
}

void BinauralPanner::advance_position() noexcept {
  if (config_.auto_turn) {
    const double step = kFullTurnDeg * config_.turn_rate_hz / sample_rate_;
    position_deg_ = wrap_turn_deg(position_deg_ + (config_.clockwise ? step : -step));
  } else if (ramp_remaining_ > 0) {
    position_deg_ = --ramp_remaining_ == 0 ? wrap_turn_deg(ramp_target_deg_)
                                           : wrap_turn_deg(position_deg_ + ramp_step_deg_);
  }
}

void BinauralPanner::process(float* const* channels, int num_channels, int num_samples) {
  sonare::rt::ScopedNoDenormals guard;
  ensure_prepared(prepared_, "BinauralPanner");
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
  if (num_channels < 2) return;

  const float speakers_target = config_.output == BinauralOutput::kSpeakers ? 1.0f : 0.0f;
  if (!primed_) {
    position_deg_ = wrap_turn_deg(config_.azimuth_deg);
    ramp_target_deg_ = config_.azimuth_deg;
    speakers_gain_ = speakers_target;
    was_turning_ = config_.auto_turn;
    primed_ = true;
  } else if (!config_.auto_turn && (config_.azimuth_deg != ramp_target_deg_ || was_turning_)) {
    // Retarget from wherever the source is now, along the shorter arc.
    ramp_target_deg_ = config_.azimuth_deg;
    const float delta =
        wrap_signed_deg(static_cast<float>(wrap_turn_deg(config_.azimuth_deg) - position_deg_));
    ramp_step_deg_ = static_cast<double>(delta) / ramp_samples_;
    ramp_remaining_ = ramp_samples_;
  }
  was_turning_ = config_.auto_turn;
  const float speakers_step = 1.0f / static_cast<float>(ramp_samples_);

  const float wet = config_.dry_wet;
  const float dry = 1.0f - wet;
  const size_t mono_mask = mono_.size() - 1;
  const size_t dry_size = dry_[0].size();
  float* const left = channels[0];
  float* const right = channels[1];
  for (int i = 0; i < num_samples; ++i) {
    const float in_left = left[i];
    const float in_right = right[i];

    // Ring interpolation: the two neighbouring points, weights summing to one.
    const double units = position_deg_ / ring::kRingAzimuthStepDeg;
    const double floor_units = std::floor(units);
    const int i0 = static_cast<int>(floor_units) % ring::kRingAzimuths;
    const int i1 = (i0 + 1) % ring::kRingAzimuths;
    const float w1 = static_cast<float>(units - floor_units);
    const float w0 = 1.0f - w1;
    const auto signed_itd = [this](int az) {
      return az <= kHalfRing ? itd_[static_cast<size_t>(az)]
                             : -itd_[static_cast<size_t>(ring::kRingAzimuths - az)];
    };
    const float itd = w0 * signed_itd(i0) + w1 * signed_itd(i1);

    // The far ear reads the mono input through the fractional ITD.
    mono_[mono_write_] = 0.5f * (in_left + in_right);
    const float ear_in[2] = {
        rt::lagrange3_read(mono_.data(), mono_.size(), mono_write_,
                           static_cast<int>(std::lround(std::max(itd, 0.0f) * kDelayQ8))),
        rt::lagrange3_read(mono_.data(), mono_.size(), mono_write_,
                           static_cast<int>(std::lround(std::max(-itd, 0.0f) * kDelayQ8)))};
    mono_write_ = (mono_write_ + 1) & mono_mask;

    ear_pos_ = ear_pos_ == 0 ? taps_ - 1 : ear_pos_ - 1;
    for (int ear = 0; ear < 2; ++ear) {
      ear_history_[ear][static_cast<size_t>(ear_pos_)] = ear_in[ear];
      ear_history_[ear][static_cast<size_t>(ear_pos_ + taps_)] = ear_in[ear];
    }
    // The left ear at azimuth a is the stored right ear at -a.
    const float* left0 =
        &ring_[static_cast<size_t>((ring::kRingAzimuths - i0) % ring::kRingAzimuths) *
               static_cast<size_t>(taps_)];
    const float* left1 =
        &ring_[static_cast<size_t>((ring::kRingAzimuths - i1) % ring::kRingAzimuths) *
               static_cast<size_t>(taps_)];
    const float* right0 = &ring_[static_cast<size_t>(i0) * static_cast<size_t>(taps_)];
    const float* right1 = &ring_[static_cast<size_t>(i1) * static_cast<size_t>(taps_)];
    const float* heard_left = &ear_history_[0][static_cast<size_t>(ear_pos_)];
    const float* heard_right = &ear_history_[1][static_cast<size_t>(ear_pos_)];
    const float bin_left = w0 * dot(left0, heard_left, taps_) + w1 * dot(left1, heard_left, taps_);
    const float bin_right =
        w0 * dot(right0, heard_right, taps_) + w1 * dot(right1, heard_right, taps_);

    // Mid/side history: the modelling delay for the phones pair, the canceller's input.
    ms_pos_ = ms_pos_ == 0 ? xtc_taps_ - 1 : ms_pos_ - 1;
    const float ms_in[2] = {0.5f * (bin_left + bin_right), 0.5f * (bin_left - bin_right)};
    for (int part = 0; part < 2; ++part) {
      ms_history_[part][static_cast<size_t>(ms_pos_)] = ms_in[part];
      ms_history_[part][static_cast<size_t>(ms_pos_ + xtc_taps_)] = ms_in[part];
    }
    const size_t delayed = static_cast<size_t>(ms_pos_ + xtc_delay_);
    const float mid = ms_history_[0][delayed];
    const float side = ms_history_[1][delayed];
    float out_left = mid + side;
    float out_right = mid - side;

    if (speakers_gain_ > 0.0f || speakers_target > 0.0f) {
      const float c_mid =
          dot(xtc_mid_.data(), &ms_history_[0][static_cast<size_t>(ms_pos_)], xtc_taps_);
      const float c_side =
          dot(xtc_side_.data(), &ms_history_[1][static_cast<size_t>(ms_pos_)], xtc_taps_);
      const float cancelled[2] = {c_mid + c_side, c_mid - c_side};
      const float plain[2] = {out_left, out_right};
      float speakers[2];
      for (int ch = 0; ch < 2; ++ch) {
        const float lows = low_[ch][1].process(low_[ch][0].process(plain[ch]));
        const float highs = high_[ch][1].process(high_[ch][0].process(cancelled[ch]));
        speakers[ch] = lows + highs;
      }
      out_left += speakers_gain_ * (speakers[0] - out_left);
      out_right += speakers_gain_ * (speakers[1] - out_right);
      speakers_gain_ = speakers_target > speakers_gain_
                           ? std::min(speakers_target, speakers_gain_ + speakers_step)
                           : std::max(speakers_target, speakers_gain_ - speakers_step);
      if (speakers_gain_ == 0.0f) {
        // Faded out: the next fade-in starts the crossover from rest.
        for (int ch = 0; ch < 2; ++ch) {
          for (int stage = 0; stage < 2; ++stage) {
            low_[ch][stage].reset();
            high_[ch][stage].reset();
          }
        }
      }
    }

    // The dry pair is delayed by the latency so it lines up with the wet pair.
    dry_[0][dry_pos_] = in_left;
    dry_[1][dry_pos_] = in_right;
    dry_pos_ = dry_pos_ + 1 == dry_size ? 0 : dry_pos_ + 1;
    left[i] = dry * dry_[0][dry_pos_] + wet * out_left;
    right[i] = dry * dry_[1][dry_pos_] + wet * out_right;

    advance_position();
  }

  bool discarded = false;
  for (int ch = 0; ch < 2; ++ch) {
    for (int stage = 0; stage < 2; ++stage) {
      discarded |= discard_group_if_non_finite(low_[ch][stage].z1, low_[ch][stage].z2);
      discarded |= discard_group_if_non_finite(high_[ch][stage].z1, high_[ch][stage].z2);
    }
  }
  if (discarded) note_non_finite_discard();
}

bool BinauralPanner::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.azimuth_deg = wrap_signed_deg(value);
      return true;
    case 1:
      config_.auto_turn = value != 0.0f;
      return true;
    case 2:
      config_.turn_rate_hz = std::clamp(value, 0.0f, kMaxTurnRateHz);
      return true;
    case 3:
      config_.clockwise = value != 0.0f;
      return true;
    case 4:
      // An unnamed value is refused rather than rounded onto a neighbour.
      if (value < 0.0f || value != std::floor(value) ||
          value >= static_cast<float>(kBinauralOutputCount)) {
        return false;
      }
      config_.output = static_cast<BinauralOutput>(static_cast<int>(value));
      return true;
    case 5:
      config_.dry_wet = std::clamp(value, 0.0f, 1.0f);
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> BinauralPanner::parameter_descriptors() const {
  return {{"azimuthDeg", 0}, {"autoTurn", 1}, {"turnRateHz", 2},
          {"clockwise", 3},  {"output", 4},   {"dryWet", 5}};
}

}  // namespace sonare::mastering::stereo
