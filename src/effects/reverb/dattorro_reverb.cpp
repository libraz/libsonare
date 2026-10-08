#include "effects/reverb/dattorro_reverb.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "rt/tail_budget.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/non_finite_state.h"

namespace sonare::effects::reverb {

using sonare::constants::kHalfPi;
using sonare::constants::kTwoPi;

namespace {

// Reference rate from Dattorro's tables; all delay lengths scale by sr/29761.
constexpr double kRefRate = DattorroReverb::kReferenceSampleRate;

// Shorter modulated allpass, in reference-rate samples. A deeper modulation would drive its
// delay below one sample, where ModAllpass::process saturates, so it bounds the depth too.
constexpr double kModAllpassLeftRefLength = 672.0;
constexpr double kModAllpassRightRefLength = 908.0;

bool scale_modulation_depth(float requested, double rate, float* scaled) noexcept {
  if (!(requested >= 0.0f) || static_cast<double>(requested) > kModAllpassLeftRefLength) {
    return false;
  }
  *scaled = static_cast<float>(static_cast<double>(requested) * rate / kRefRate);
  return true;
}
// Live pre-delay ceiling: the GS system reverb PREDELAY conversion reaches 127 ms
// (gs_reverb_predelay_ms) and the EFX pre-delay ladder 100 ms. prepare() sizes the
// ring for max(ceiling, configured value), so the realtime setter never grows it.
constexpr float kLiveMaxPreDelayMs = 127.0f;

size_t scale_len(double ref_samples, double sr) {
  const double scaled = ref_samples * sr / kRefRate;
  return static_cast<size_t>(std::max(1L, std::lround(scaled)));
}

constexpr float kGainIn = 0.75f;   // Input diffusion allpass gain.
constexpr float kGainMod = 0.7f;   // Modulated tank allpass gain.
constexpr float kGainDiff = 0.5f;  // Decay diffusion allpass gain.

// Ratios applied to the four tank delay lines (L1, L2, R1, R2) for characters 1-6
// (Room 1, Room 2, Stage 1, Stage 2, Hall 1, Hall 2). Invented values: shorter
// lines give a denser, quicker tail, longer ones a sparser, longer one.
constexpr double kCharacterRatios[kDattorroMaxCharacter + 1][4] = {
    {1.0, 1.0, 1.0, 1.0},     {0.30, 0.28, 0.32, 0.27}, {0.45, 0.42, 0.47, 0.40},
    {0.70, 0.66, 0.72, 0.62}, {0.85, 0.80, 0.90, 0.78}, {1.15, 1.10, 1.20, 1.05},
    {1.40, 1.30, 1.50, 1.25}};

// Per-column maximum over the table: the size every tank line is prepared for.
constexpr std::array<double, 4> max_character_ratios() {
  std::array<double, 4> max_ratio{};
  for (const auto& row : kCharacterRatios) {
    for (size_t column = 0; column < 4; ++column) {
      max_ratio[column] = std::max(max_ratio[column], row[column]);
    }
  }
  return max_ratio;
}
constexpr std::array<double, 4> kMaxCharacterRatios = max_character_ratios();

int clamp_character(int character) { return std::clamp(character, 0, kDattorroMaxCharacter); }

// Gate time constants: the peak detector's fall, and the gain's approach to its target.
constexpr double kGateDetectorReleaseMs = 5.0;
constexpr double kGateSmoothingMs = 1.0;

// One-pole smoothing coefficient for a time constant in milliseconds at the working rate.
float smoothing_coefficient(double ms, double sr) {
  return static_cast<float>(1.0 - std::exp(-1000.0 / (ms * sr)));
}

// Folds an LFO phase back into [0, 2pi) for any increment. A single
// conditional subtract only covers an increment below one period, and nothing
// bounds the modulation rate against the sample rate, so a rate above it would
// let the phase grow without limit - and sin() loses argument precision long
// before the accumulator overflows. The range test keeps the common case at one
// comparison; fmod folds the rest in a single step rather than looping.
inline float wrap_lfo_phase(float phase) noexcept {
  if (phase >= 0.0f && phase < kTwoPi) return phase;
  phase = std::fmod(phase, kTwoPi);
  return phase < 0.0f ? phase + kTwoPi : phase;
}

}  // namespace

// --- Allpass ---------------------------------------------------------------

void DattorroReverb::Allpass::prepare(size_t length, float g) {
  size = std::max<size_t>(1, length);
  gain = g;
  buf.assign(size, 0.0f);
  index = 0;
}

void DattorroReverb::Allpass::reset() {
  std::fill(buf.begin(), buf.end(), 0.0f);
  index = 0;
}

float DattorroReverb::Allpass::process(float in) {
  const float d = buf[index];
  const float out = -gain * in + d;
  buf[index] = in + gain * out;
  index = (index + 1) % size;
  return out;
}

float DattorroReverb::Allpass::read_at(size_t offset) const {
  // index points at the oldest sample (next write); the most recent write is
  // index-1. Offset is measured backwards from the most recent write.
  const size_t pos = (index + size - 1 - (offset % size)) % size;
  return buf[pos];
}

// --- ModAllpass ------------------------------------------------------------

void DattorroReverb::ModAllpass::prepare(size_t base_len, size_t max_depth, float g) {
  base = std::max<size_t>(1, base_len);
  gain = g;
  capacity = base + max_depth + 2;  // +guard
  buf.assign(capacity, 0.0f);
  index = 0;
}

void DattorroReverb::ModAllpass::ensure_capacity(size_t max_depth) {
  const size_t required = base + max_depth + 2;
  if (capacity >= required) return;

  std::vector<float> grown(required, 0.0f);
  const size_t samples_to_keep = std::min(capacity - 1, required - 1);
  for (size_t delay = 1; delay <= samples_to_keep; ++delay) {
    const size_t old_pos = (index + capacity - delay) % capacity;
    const size_t new_pos = (required - delay) % required;
    grown[new_pos] = buf[old_pos];
  }
  buf = std::move(grown);
  capacity = required;
  index = 0;
}

void DattorroReverb::ModAllpass::reset() {
  std::fill(buf.begin(), buf.end(), 0.0f);
  index = 0;
}

float DattorroReverb::ModAllpass::process(float in, float mod_offset) {
  // Read at a fractional delay. Rounding the modulated delay to whole samples
  // steps the read pointer, and each step is a discontinuity in the tank - a
  // periodic tick whose rate rises with the modulation depth, so the depth
  // control degraded the signal as it was raised. Interpolating between the two
  // neighbouring samples makes a continuous mod_offset produce a continuous
  // output. The upper clamp leaves room for the second tap; prepare() reserves
  // the two guard samples this needs.
  const float requested = static_cast<float>(base) + mod_offset;
  const float delay = std::clamp(requested, 1.0f, static_cast<float>(capacity - 2));
  const float whole = std::floor(delay);
  const float frac = delay - whole;
  const size_t near = static_cast<size_t>(whole);
  const size_t read0 = (index + capacity - near) % capacity;
  const size_t read1 = (index + capacity - near - 1) % capacity;
  const float d = buf[read0] + frac * (buf[read1] - buf[read0]);
  const float out = -gain * in + d;
  buf[index] = in + gain * out;
  index = (index + 1) % capacity;
  return out;
}

// --- TapDelay --------------------------------------------------------------

void DattorroReverb::TapDelay::prepare(size_t delay_length) {
  length = std::max<size_t>(1, delay_length);
  cap = length + 1;
  buf.assign(cap, 0.0f);
  index = 0;
}

void DattorroReverb::TapDelay::reset() {
  std::fill(buf.begin(), buf.end(), 0.0f);
  index = 0;
}

void DattorroReverb::TapDelay::write(float in) { buf[index] = in; }

void DattorroReverb::TapDelay::advance() { index = (index + 1) % cap; }

float DattorroReverb::TapDelay::read_at(size_t offset) const {
  // offset==0 returns the value written this step (at index); larger offsets
  // reach back into history. Capacity guarantees offset<=length is valid.
  const size_t o = offset >= cap ? cap - 1 : offset;
  const size_t pos = (index + cap - o) % cap;
  return buf[pos];
}

// --- DattorroReverb --------------------------------------------------------

float DattorroReverb::damping_coefficient(double corner_hz, double sample_rate) noexcept {
  // Solves |H(w_c)|^2 = 1/2 for y += a (x - y): with b = 1 - a and c = cos(w_c),
  // b^2 - (4 - 2c) b + 1 = 0, taking the root below one.
  const double fc = std::clamp(corner_hz, 0.0, 0.49 * sample_rate);
  const double c = std::cos(kTwoPi * fc / sample_rate);
  const double b = (2.0 - c) - std::sqrt((2.0 - c) * (2.0 - c) - 1.0);
  return static_cast<float>(1.0 - b);
}

DattorroReverb::DattorroReverb(DattorroReverbConfig config) : config_(config) {
  if (!std::isfinite(config_.pre_delay_samples)) config_.pre_delay_samples = 0.0f;
  if (!std::isfinite(config_.mod_depth_samples)) config_.mod_depth_samples = 0.0f;
  config_.mod_depth_samples = std::max(0.0f, config_.mod_depth_samples);
  config_.character = clamp_character(config_.character);
  max_pre_delay_ms_ = std::max(kLiveMaxPreDelayMs,
                               static_cast<float>(config_.pre_delay_samples * 1000.0 / kRefRate));
}

void DattorroReverb::update_pre_delay_length() noexcept {
  if (pre_delay_buf_.empty() || !(config_.pre_delay_samples > 0.0f)) {
    pre_delay_len_ = 0;
    return;
  }
  const size_t requested = scale_len(static_cast<double>(config_.pre_delay_samples), sample_rate_);
  pre_delay_len_ = std::min(requested, pre_delay_buf_.size());
  pre_delay_index_ %= pre_delay_buf_.size();
}

void DattorroReverb::update_character_geometry() noexcept {
  if (delay_l1_.buf.empty() || delay_l2_.buf.empty() || delay_r1_.buf.empty() ||
      delay_r2_.buf.empty()) {
    return;
  }

  const double* ratio = kCharacterRatios[clamp_character(config_.character)];
  const auto set_length = [this](TapDelay& delay, double ref_samples, double ratio_value) {
    const size_t requested = scale_len(ref_samples * ratio_value, sample_rate_);
    delay.length = std::min(requested, delay.cap - 1);
  };
  set_length(delay_l1_, 4453.0, ratio[0]);
  set_length(delay_l2_, 3720.0, ratio[1]);
  set_length(delay_r1_, 4217.0, ratio[2]);
  set_length(delay_r2_, 3163.0, ratio[3]);

  // Output taps; a tap that reads a delay line scales with that line.
  tap_l_l1a_ = scale_len(266.0 * ratio[0], sample_rate_);
  tap_l_l1b_ = scale_len(2974.0 * ratio[0], sample_rate_);
  tap_l_apl_ = scale_len(1913.0 * ratio[1], sample_rate_);
  tap_l_l2_ = scale_len(1996.0 * ratio[1], sample_rate_);
  tap_l_r1_ = scale_len(1990.0 * ratio[2], sample_rate_);
  tap_l_apr_ = scale_len(187.0, sample_rate_);
  tap_l_r2_ = scale_len(1066.0 * ratio[3], sample_rate_);

  tap_r_r1a_ = scale_len(353.0 * ratio[2], sample_rate_);
  tap_r_r1b_ = scale_len(3627.0 * ratio[2], sample_rate_);
  tap_r_apr_ = scale_len(1228.0, sample_rate_);
  tap_r_r2_ = scale_len(2673.0 * ratio[3], sample_rate_);
  tap_r_l1_ = scale_len(2111.0 * ratio[0], sample_rate_);
  tap_r_apl_ = scale_len(335.0, sample_rate_);
  tap_r_l2_ = scale_len(121.0 * ratio[1], sample_rate_);
}

void DattorroReverb::prepare(double sample_rate, int) {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  float depth = 0.0f;
  if (!scale_modulation_depth(config_.mod_depth_samples, sr, &depth)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "modulation depth exceeds the modulated allpass length");
  }
  sample_rate_ = sr;

  // Stage 1: pre-delay + four series input-diffusion allpasses.
  const double requested_pre = std::max(0.0, static_cast<double>(config_.pre_delay_samples));
  const double max_pre_samples = static_cast<double>(max_pre_delay_ms_) * kRefRate / 1000.0;
  const size_t max_pre = scale_len(std::max(max_pre_samples, requested_pre), sr);
  pre_delay_buf_.assign(max_pre, 0.0f);
  pre_delay_len_ = 0;
  pre_delay_index_ = 0;
  update_pre_delay_length();

  in_ap_[0].prepare(scale_len(142.0, sr), kGainIn);
  in_ap_[1].prepare(scale_len(107.0, sr), kGainIn);
  in_ap_[2].prepare(scale_len(379.0, sr), kGainIn);
  in_ap_[3].prepare(scale_len(277.0, sr), kGainIn);

  // Modulation: depth scaled to working rate, guard buffer sized for it.
  mod_depth_ = depth;
  const size_t max_depth = static_cast<size_t>(std::lround(mod_depth_)) + 1;
  mod_ap_l_.prepare(scale_len(kModAllpassLeftRefLength, sr), max_depth, kGainMod);
  mod_ap_r_.prepare(scale_len(kModAllpassRightRefLength, sr), max_depth, kGainMod);

  const std::array<double, 4>& max_ratio = kMaxCharacterRatios;
  delay_l1_.prepare(scale_len(4453.0 * max_ratio[0], sr));
  delay_l2_.prepare(scale_len(3720.0 * max_ratio[1], sr));
  delay_r1_.prepare(scale_len(4217.0 * max_ratio[2], sr));
  delay_r2_.prepare(scale_len(3163.0 * max_ratio[3], sr));
  decay_ap_l_.prepare(scale_len(1800.0, sr), kGainDiff);
  decay_ap_r_.prepare(scale_len(2656.0, sr), kGainDiff);
  update_character_geometry();

  lfo_inc_ = static_cast<float>(kTwoPi * config_.mod_rate_hz / sr);

  reset();
}

void DattorroReverb::process(float* const* channels, int num_channels, int num_samples) {
  rt::ScopedNoDenormals no_denormals;
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0 || channels[0] == nullptr) {
    return;
  }
  float* left = channels[0];
  const bool stereo = num_channels > 1 && channels[1] != nullptr;
  // For mono, alias right to left for reading the (identical) input, but only
  // one output write is performed below so the left result is not clobbered.
  float* right = stereo ? channels[1] : channels[0];

  const float decay = std::clamp(config_.decay, 0.0f, 0.98f);
  const float damp_d = config_.damping_hz > 0.0f
                           ? damping_coefficient(config_.damping_hz, sample_rate_)
                           : std::clamp(config_.damping, 0.0f, 1.0f) * 0.4f;
  // Block-rate dry/wet: smoothed across blocks by the engine parameter slot
  // smoother, not per-sample (see Chorus::process for the rationale).
  const common::MixGains mix =
      common::mix_gains(config_.mix_law, std::clamp(config_.dry_wet, 0.0f, 1.0f));
  const float wet = mix.wet;
  const float dry = mix.dry;

  const bool gate_on = config_.gate_threshold_db > kDattorroGateOffDb;
  const float gate_threshold = gate_on ? std::pow(10.0f, config_.gate_threshold_db / 20.0f) : 0.0f;
  const int gate_hold =
      static_cast<int>(std::lround(std::max(0.0f, config_.gate_hold_ms) * sample_rate_ / 1000.0));
  const int gate_ramp = std::max(1, gate_hold);
  const float detector_release = 1.0f - smoothing_coefficient(kGateDetectorReleaseMs, sample_rate_);
  const float gate_smoothing = smoothing_coefficient(kGateSmoothingMs, sample_rate_);
  gate_stage_.admit(gate_on, [this] { reset_gate(); });

  for (int i = 0; i < num_samples; ++i) {
    const float in_l = left[i];
    const float in_r = right[i];

    // Stage 1: mono sum, optional pre-delay, four diffusion allpasses.
    float x = 0.5f * (in_l + in_r);
    if (!pre_delay_buf_.empty()) {
      const size_t read_index =
          (pre_delay_index_ + pre_delay_buf_.size() - pre_delay_len_) % pre_delay_buf_.size();
      const float delayed = pre_delay_buf_[read_index];
      pre_delay_buf_[pre_delay_index_] = x;
      pre_delay_index_ = (pre_delay_index_ + 1) % pre_delay_buf_.size();
      if (pre_delay_len_ > 0) x = delayed;
    }
    x = in_ap_[0].process(x);
    x = in_ap_[1].process(x);
    x = in_ap_[2].process(x);
    x = in_ap_[3].process(x);
    const float tank_in = x;

    // Capture previous tails so both halves cross-couple from the same step.
    const float prev_tail_l = tail_l_;
    const float prev_tail_r = tail_r_;

    // LFO offsets for the two modulated tank allpasses.
    const float mod_l = mod_depth_ * std::sin(lfo_phase_l_);
    const float mod_r = mod_depth_ * std::sin(lfo_phase_r_);
    lfo_phase_l_ += lfo_inc_;
    lfo_phase_r_ += lfo_inc_;
    // Wrap in both directions, and for any magnitude: a negative mod_rate gives
    // a negative lfo_inc_, and a rate above the sample rate gives an increment
    // past a full period, neither of which a single conditional subtract folds.
    lfo_phase_l_ = wrap_lfo_phase(lfo_phase_l_);
    lfo_phase_r_ = wrap_lfo_phase(lfo_phase_r_);

    // Half-L: cross-coupled from the previous right tail.
    float l = mod_ap_l_.process(tank_in + decay * prev_tail_r, mod_l);
    delay_l1_.write(l);
    const float l1_out = delay_l1_.read_at(delay_l1_.length);
    damp_l_ += damp_d * (l1_out - damp_l_);
    float l_dec = decay * damp_l_;
    l_dec = decay_ap_l_.process(l_dec);
    delay_l2_.write(l_dec);
    tail_l_ = delay_l2_.read_at(delay_l2_.length);

    // Half-R: cross-coupled from the previous left tail.
    float r = mod_ap_r_.process(tank_in + decay * prev_tail_l, mod_r);
    delay_r1_.write(r);
    const float r1_out = delay_r1_.read_at(delay_r1_.length);
    damp_r_ += damp_d * (r1_out - damp_r_);
    float r_dec = decay * damp_r_;
    r_dec = decay_ap_r_.process(r_dec);
    delay_r2_.write(r_dec);
    tail_r_ = delay_r2_.read_at(delay_r2_.length);

    delay_l1_.advance();
    delay_l2_.advance();
    delay_r1_.advance();
    delay_r2_.advance();

    // Output taps (read after writes/advance so offsets address valid history).
    // The 1913-sample tap reads from the delay line following the left decay
    // diffuser (delay_l2_, length 3720), per the canonical Dattorro topology.
    // It must NOT read decay_ap_l_ (length 1800): 1913 > 1800 would wrap the
    // allpass ring and return the wrong node (sample 113).
    float out_l = delay_l1_.read_at(tap_l_l1a_) + delay_l1_.read_at(tap_l_l1b_) -
                  delay_l2_.read_at(tap_l_apl_) + delay_l2_.read_at(tap_l_l2_) -
                  delay_r1_.read_at(tap_l_r1_) - decay_ap_r_.read_at(tap_l_apr_) -
                  delay_r2_.read_at(tap_l_r2_);
    float out_r = delay_r1_.read_at(tap_r_r1a_) + delay_r1_.read_at(tap_r_r1b_) -
                  decay_ap_r_.read_at(tap_r_apr_) + delay_r2_.read_at(tap_r_r2_) -
                  delay_l1_.read_at(tap_r_l1_) - decay_ap_l_.read_at(tap_r_apl_) -
                  delay_l2_.read_at(tap_r_l2_);

    if (gate_on) {
      const float level = std::max(std::fabs(out_l), std::fabs(out_r));
      gate_env_ = std::max(level, gate_env_ * detector_release);
      if (gate_env_ >= gate_threshold) {
        if (!gate_open_) gate_elapsed_ = 0;
        gate_open_ = true;
        gate_hold_left_ = gate_hold;
      } else if (gate_open_) {
        if (gate_hold_left_ > 0) {
          --gate_hold_left_;
        } else {
          gate_open_ = false;
        }
      }
      if (gate_open_ && gate_elapsed_ < gate_ramp) ++gate_elapsed_;
      const float progress = static_cast<float>(gate_elapsed_) / static_cast<float>(gate_ramp);
      float target = gate_open_ ? 1.0f : 0.0f;
      if (config_.gate_type == DattorroGateType::kReverse && gate_open_) target = progress;
      gate_gain_ += gate_smoothing * (target - gate_gain_);
      float gain_l = gate_gain_;
      float gain_r = gate_gain_;
      if (config_.gate_type == DattorroGateType::kSweep1 ||
          config_.gate_type == DattorroGateType::kSweep2) {
        const float p = config_.gate_type == DattorroGateType::kSweep1 ? progress : 1.0f - progress;
        gain_l *= std::min(1.0f, 2.0f * (1.0f - p));
        gain_r *= std::min(1.0f, 2.0f * p);
      }
      out_l *= gain_l;
      out_r *= gain_r;
    }

    if (stereo) {
      left[i] = dry * in_l + wet * out_l;
      right[i] = dry * in_r + wet * out_r;
    } else {
      // Mono: collapse the stereo reverb to a single channel so the lone output
      // buffer is not written twice with different values.
      left[i] = dry * in_l + wet * 0.5f * (out_l + out_r);
    }
  }
  discard_non_finite();
}

void DattorroReverb::discard_non_finite() noexcept {
  // The four cells are one cross-coupled tank; a rested damping cell in the
  // half whose tail is poisoned is not half a tank.
  if (!discard_group_if_non_finite(damp_l_, damp_r_, tail_l_, tail_r_)) return;
  note_non_finite_discard();
  // The lines and allpasses upstream are the loop that feeds these cells --
  // including the input diffusers, which recirculate their own output -- so the
  // poison cycles back instead of flowing out. O(line), recovery only.
  std::fill(pre_delay_buf_.begin(), pre_delay_buf_.end(), 0.0f);
  pre_delay_index_ = 0;
  for (auto& ap : in_ap_) ap.reset();
  mod_ap_l_.reset();
  mod_ap_r_.reset();
  delay_l1_.reset();
  delay_l2_.reset();
  delay_r1_.reset();
  delay_r2_.reset();
  decay_ap_l_.reset();
  decay_ap_r_.reset();
  reset_gate();
}

void DattorroReverb::reset_gate() noexcept {
  gate_env_ = 0.0f;
  gate_gain_ = 1.0f;
  gate_open_ = false;
  gate_hold_left_ = 0;
  gate_elapsed_ = 0;
}

int DattorroReverb::tail_samples() const noexcept {
  if (std::clamp(config_.dry_wet, 0.0f, 1.0f) <= 0.0f) return 0;
  const double decay = std::clamp(static_cast<double>(config_.decay), 0.0, 0.98);
  const double rate = sample_rate_ / kRefRate;
  const double* ratio = kCharacterRatios[clamp_character(config_.character)];

  // The front end is lossless allpasses (four input diffusers, the modulated tank allpass): their
  // cascade rings no longer than its longest member, so they are budgeted alongside each other.
  rt::TailBudget diffusion;
  for (const double length : {142.0, 107.0, 379.0, 277.0}) {
    diffusion.alongside(rt::TailBudget().recirculation(length * rate, kGainIn));
  }
  diffusion.alongside(rt::TailBudget().recirculation(
      std::max(kModAllpassLeftRefLength, kModAllpassRightRefLength) * rate +
          std::fabs(static_cast<double>(mod_depth_)) + 1.0,
      kGainMod));
  // The tank: its first line is heard through the output taps even when decay closes the loop;
  // otherwise each figure-8 pass (both halves' allpasses 672 + 908 + 1800 + 2656 plus the four
  // lines at their character's ratios) crosses four decay multiplies, carried to the floor.
  const double tank_loop = (672.0 + 908.0 + 1800.0 + 2656.0 + 4453.0 * ratio[0] +
                            3720.0 * ratio[1] + 4217.0 * ratio[2] + 3163.0 * ratio[3]) *
                           rate;
  rt::TailBudget tank;
  tank.delay(std::max(4453.0 * ratio[0], 4217.0 * ratio[2]) * rate);
  if (decay > 0.0) {
    tank.alongside(
        rt::TailBudget().delay(tank_loop * std::log(rt::kTailFloor) / (4.0 * std::log(decay))));
  }
  rt::TailBudget tail;
  tail.delay(std::max(0.0, static_cast<double>(config_.pre_delay_samples)) * rate);
  tail.then(diffusion).then(tank);
  return tail.samples();
}

bool DattorroReverb::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      // process() clamps decay to [0, 0.98]; store the raw target.
      config_.decay = value;
      return true;
    case 1:
      // process() clamps damping to [0, 1] and maps it to the one-pole coeff.
      config_.damping = value;
      return true;
    case 2:
      config_.dry_wet = value;
      return true;
    case 3:
      // Recompute the LFO increment in place; preserves the modulation phase.
      config_.mod_rate_hz = value;
      lfo_inc_ = static_cast<float>(kTwoPi * config_.mod_rate_hz / sample_rate_);
      return true;
    case 4: {
      const float requested = std::max(0.0f, value);
      float depth = 0.0f;
      if (!scale_modulation_depth(requested, sample_rate_, &depth)) return false;
      config_.mod_depth_samples = requested;
      mod_depth_ = depth;
      {
        const size_t max_depth = static_cast<size_t>(std::lround(mod_depth_)) + 1;
        mod_ap_l_.ensure_capacity(max_depth);
        mod_ap_r_.ensure_capacity(max_depth);
      }
      return true;
    }
    case 5:
      config_.damping_hz = std::max(0.0f, value);
      return true;
    case 6:
      config_.gate_threshold_db = value;
      return true;
    case 7:
      config_.gate_hold_ms = std::max(0.0f, value);
      return true;
    case 8:
      // An unnamed type is refused rather than rounded onto a neighbour.
      if (value < 0.0f || value != std::floor(value) ||
          value >= static_cast<float>(kDattorroGateTypeCount)) {
        return false;
      }
      config_.gate_type = static_cast<DattorroGateType>(static_cast<int>(value));
      return true;
    case 9:
      config_.pre_delay_samples =
          std::clamp(value, 0.0f, max_pre_delay_ms_) * static_cast<float>(kRefRate) / 1000.0f;
      update_pre_delay_length();
      return true;
    case 10:
      // A tank set is discrete, but every set's maximum line was prepared up front.
      if (value < 0.0f || value != std::floor(value) ||
          value > static_cast<float>(kDattorroMaxCharacter)) {
        return false;
      }
      config_.character = static_cast<int>(value);
      update_character_geometry();
      return true;
    case 11:
      return common::mix_law_from_value(value, &config_.mix_law);
    default:
      return false;
  }
}

bool DattorroReverb::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  // Id 4 may grow the modulated allpass buffers; the rest are in-place scalar updates.
  return param_id != 4u;
}

std::vector<rt::ParamDescriptor> DattorroReverb::parameter_descriptors() const {
  return {{"decay", 0},           {"damping", 1},    {"dryWet", 2},          {"modRateHz", 3},
          {"modDepthSamples", 4}, {"dampingHz", 5},  {"gateThresholdDb", 6}, {"gateHoldMs", 7},
          {"gateType", 8},        {"preDelayMs", 9}, {"character", 10},      {"mixLaw", 11}};
}

void DattorroReverb::reset() {
  std::fill(pre_delay_buf_.begin(), pre_delay_buf_.end(), 0.0f);
  pre_delay_index_ = 0;
  for (auto& ap : in_ap_) ap.reset();
  mod_ap_l_.reset();
  mod_ap_r_.reset();
  delay_l1_.reset();
  delay_l2_.reset();
  delay_r1_.reset();
  delay_r2_.reset();
  decay_ap_l_.reset();
  decay_ap_r_.reset();
  damp_l_ = 0.0f;
  damp_r_ = 0.0f;
  tail_l_ = 0.0f;
  tail_r_ = 0.0f;
  lfo_phase_l_ = 0.0f;
  // Quadrature, so the two tank allpasses lengthen and shorten in opposition
  // instead of together. Starting both at zero leaves the two offsets identical
  // for every sample of every run, which makes the modulation common-mode and
  // contributes nothing to the width of the tail. Dattorro's "figure-8" names
  // the tank's cross-coupled topology, not a trajectory for the LFOs, so the
  // offset is a design choice; a quarter cycle is the one that makes two
  // same-rate sinusoids uncorrelated.
  lfo_phase_r_ = kHalfPi;
  reset_gate();
  gate_stage_.close();
}

}  // namespace sonare::effects::reverb
