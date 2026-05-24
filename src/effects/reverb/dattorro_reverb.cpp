#include "effects/reverb/dattorro_reverb.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::effects::reverb {

using sonare::constants::kTwoPi;

namespace {

// Reference rate from Dattorro's tables; all delay lengths scale by sr/29761.
constexpr double kRefRate = DattorroReverb::kReferenceSampleRate;

size_t scale_len(double ref_samples, double sr) {
  const double scaled = ref_samples * sr / kRefRate;
  return static_cast<size_t>(std::max(1L, std::lround(scaled)));
}

constexpr float kGainIn = 0.75f;   // Input diffusion allpass gain.
constexpr float kGainMod = 0.7f;   // Modulated tank allpass gain.
constexpr float kGainDiff = 0.5f;  // Decay diffusion allpass gain.

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

void DattorroReverb::ModAllpass::reset() {
  std::fill(buf.begin(), buf.end(), 0.0f);
  index = 0;
}

float DattorroReverb::ModAllpass::process(float in, float mod_offset) {
  long delay = static_cast<long>(base) + std::lround(mod_offset);
  delay = std::clamp<long>(delay, 1, static_cast<long>(capacity) - 1);
  const size_t read = (index + capacity - static_cast<size_t>(delay)) % capacity;
  const float d = buf[read];
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

DattorroReverb::DattorroReverb(DattorroReverbConfig config) : config_(config) {}

void DattorroReverb::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  const double sr = sample_rate_;

  // Stage 1: pre-delay + four series input-diffusion allpasses.
  const size_t pre = scale_len(static_cast<double>(config_.pre_delay_samples), sr);
  pre_delay_len_ = config_.pre_delay_samples > 0.0f ? pre : 0;
  pre_delay_buf_.assign(std::max<size_t>(1, pre_delay_len_), 0.0f);
  pre_delay_index_ = 0;

  in_ap_[0].prepare(scale_len(142.0, sr), kGainIn);
  in_ap_[1].prepare(scale_len(107.0, sr), kGainIn);
  in_ap_[2].prepare(scale_len(379.0, sr), kGainIn);
  in_ap_[3].prepare(scale_len(277.0, sr), kGainIn);

  // Modulation: depth scaled to working rate, guard buffer sized for it.
  mod_depth_ = static_cast<float>(config_.mod_depth_samples * sr / kRefRate);
  const size_t max_depth = static_cast<size_t>(std::lround(mod_depth_)) + 1;
  mod_ap_l_.prepare(scale_len(672.0, sr), max_depth, kGainMod);
  mod_ap_r_.prepare(scale_len(908.0, sr), max_depth, kGainMod);

  delay_l1_.prepare(scale_len(4453.0, sr));
  delay_l2_.prepare(scale_len(3720.0, sr));
  delay_r1_.prepare(scale_len(4217.0, sr));
  delay_r2_.prepare(scale_len(3163.0, sr));
  decay_ap_l_.prepare(scale_len(1800.0, sr), kGainDiff);
  decay_ap_r_.prepare(scale_len(2656.0, sr), kGainDiff);

  // Output taps.
  tap_l_l1a_ = scale_len(266.0, sr);
  tap_l_l1b_ = scale_len(2974.0, sr);
  tap_l_apl_ = scale_len(1913.0, sr);
  tap_l_l2_ = scale_len(1996.0, sr);
  tap_l_r1_ = scale_len(1990.0, sr);
  tap_l_apr_ = scale_len(187.0, sr);
  tap_l_r2_ = scale_len(1066.0, sr);

  tap_r_r1a_ = scale_len(353.0, sr);
  tap_r_r1b_ = scale_len(3627.0, sr);
  tap_r_apr_ = scale_len(1228.0, sr);
  tap_r_r2_ = scale_len(2673.0, sr);
  tap_r_l1_ = scale_len(2111.0, sr);
  tap_r_apl_ = scale_len(335.0, sr);
  tap_r_l2_ = scale_len(121.0, sr);

  lfo_inc_ = static_cast<float>(kTwoPi * config_.mod_rate_hz / sr);

  reset();
}

void DattorroReverb::process(float* const* channels, int num_channels, int num_samples) {
  rt::ScopedNoDenormals no_denormals;
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0 || channels[0] == nullptr) {
    return;
  }
  float* left = channels[0];
  float* right = num_channels > 1 && channels[1] != nullptr ? channels[1] : channels[0];

  const float decay = std::clamp(config_.decay, 0.0f, 0.98f);
  const float damp_d = std::clamp(config_.damping, 0.0f, 1.0f) * 0.4f;
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;

  for (int i = 0; i < num_samples; ++i) {
    const float in_l = left[i];
    const float in_r = right[i];

    // Stage 1: mono sum, optional pre-delay, four diffusion allpasses.
    float x = 0.5f * (in_l + in_r);
    if (pre_delay_len_ > 0) {
      const float delayed = pre_delay_buf_[pre_delay_index_];
      pre_delay_buf_[pre_delay_index_] = x;
      pre_delay_index_ = (pre_delay_index_ + 1) % pre_delay_len_;
      x = delayed;
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
    if (lfo_phase_l_ > kTwoPi) lfo_phase_l_ -= kTwoPi;
    if (lfo_phase_r_ > kTwoPi) lfo_phase_r_ -= kTwoPi;

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
    const float out_l = delay_l1_.read_at(tap_l_l1a_) + delay_l1_.read_at(tap_l_l1b_) -
                        decay_ap_l_.read_at(tap_l_apl_) + delay_l2_.read_at(tap_l_l2_) -
                        delay_r1_.read_at(tap_l_r1_) - decay_ap_r_.read_at(tap_l_apr_) -
                        delay_r2_.read_at(tap_l_r2_);
    const float out_r = delay_r1_.read_at(tap_r_r1a_) + delay_r1_.read_at(tap_r_r1b_) -
                        decay_ap_r_.read_at(tap_r_apr_) + delay_r2_.read_at(tap_r_r2_) -
                        delay_l1_.read_at(tap_r_l1_) - decay_ap_l_.read_at(tap_r_apl_) -
                        delay_l2_.read_at(tap_r_l2_);

    left[i] = dry * in_l + wet * out_l;
    right[i] = dry * in_r + wet * out_r;
  }
}

bool DattorroReverb::set_parameter(unsigned int param_id, float value) {
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
    case 4:
      // Rescale the modulation depth to the working rate. The tank allpass
      // clamps reads to the guard buffer sized at prepare(), so growing the
      // depth never overruns.
      config_.mod_depth_samples = std::max(0.0f, value);
      mod_depth_ = static_cast<float>(config_.mod_depth_samples * sample_rate_ / kRefRate);
      return true;
    default:
      return false;
  }
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
  lfo_phase_r_ = 0.0f;
}

}  // namespace sonare::effects::reverb
