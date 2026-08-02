#include "effects/reverb/velvet_reverb.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::effects::reverb {

using sonare::constants::kTwoPi;

namespace {
constexpr float kShelfCutoffHz = 6000.0f;
constexpr float kShelfHighGain = 0.5f;
constexpr float kT60Drop = 1000.0f;  // 60 dB amplitude ratio for T60.

float bounded_reverb_time(float value) {
  return std::clamp(value, 0.05f, VelvetReverbConfig::kMaxReverbTimeSeconds);
}

float effective_rt60(const VelvetReverbConfig& config) {
  return bounded_reverb_time(config.reverb_time_s) * (0.5f + std::clamp(config.decay, 0.0f, 1.0f));
}
}  // namespace

// --- Ring ------------------------------------------------------------------

void VelvetReverb::Ring::prepare(int length) {
  size = std::max(1, length);
  buf.assign(static_cast<size_t>(size), 0.0f);
  index = 0;
}

void VelvetReverb::Ring::reset() {
  std::fill(buf.begin(), buf.end(), 0.0f);
  index = 0;
}

void VelvetReverb::Ring::write(float in) {
  buf[static_cast<size_t>(index)] = in;
  index = (index + 1) % size;
}

float VelvetReverb::Ring::read_at(int offset) const {
  // index points at the next write; the most recent write is index-1.
  int o = offset % size;
  int pos = (index - 1 - o) % size;
  if (pos < 0) pos += size;
  return buf[static_cast<size_t>(pos)];
}

// --- VelvetReverb ----------------------------------------------------------

VelvetReverb::VelvetReverb(VelvetReverbConfig config) : config_(config) {}

void VelvetReverb::build_table(std::vector<Tap>& taps, std::uint32_t seed_offset, int grid_ls,
                               int n_seg, int num_pulses, float decay_rate, double sr) const {
  taps.clear();
  taps.reserve(static_cast<size_t>(std::max(0, num_pulses)));
  const std::uint32_t ls = static_cast<std::uint32_t>(grid_ls);
  for (int k = 0; k < num_pulses; ++k) {
    const std::uint32_t kk = static_cast<std::uint32_t>(k) + seed_offset;
    const std::uint32_t s_pos = 1664525u * (kk + 1u) + 1013904223u;
    const std::uint32_t s_sign = 1664525u * s_pos + 1013904223u;
    int tap = k * grid_ls + static_cast<int>(s_pos % ls);
    tap = std::clamp(tap, 1, n_seg - 1);
    const float sign = (s_sign & 1u) ? 1.0f : -1.0f;
    const float gain =
        sign * std::exp(-decay_rate * static_cast<float>(tap) / static_cast<float>(sr));
    taps.push_back({tap, gain});
  }
}

void VelvetReverb::prepare(double sample_rate, int max_block_size) {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  sample_rate_ = sr;
  max_block_size_ = max_block_size;
  prepared_ = true;

  const float rho = std::clamp(config_.density_hz, 1000.0f, 3000.0f);
  const int grid_ls = std::max(1, static_cast<int>(std::lround(sr / rho)));
  // decay scales the base reverb time around its nominal value.
  const float rt60 = effective_rt60(config_);
  const int n_seg = std::max(2, static_cast<int>(std::lround(rt60 * sr)));
  const int num_pulses =
      std::clamp(static_cast<int>(std::lround(rho * rt60)), 1, VelvetReverbConfig::kMaxTapCount);
  const float decay_rate = std::log(kT60Drop) / rt60;

  // Evaluate the first partition directly (early reflections). The remaining
  // sparse impulse is moved into a uniformly partitioned FFT convolver. Its
  // one-partition staging delay is exactly compensated by shifting this late
  // IR left by one partition, so the complete processor remains zero-latency.
  ring_l_.prepare(kEarlyPartitionSamples);
  ring_r_.prepare(kEarlyPartitionSamples);
  build_table(taps_l_, 0u, grid_ls, n_seg, num_pulses, decay_rate, sr);
  build_table(taps_r_, static_cast<std::uint32_t>(num_pulses), grid_ls, n_seg, num_pulses,
              decay_rate, sr);
  auto split_taps = [&](const std::vector<Tap>& taps, std::vector<Tap>& early,
                        std::vector<float>& late_ir) {
    early.clear();
    late_ir.assign(static_cast<size_t>(std::max(0, n_seg - kEarlyPartitionSamples)), 0.0f);
    for (const Tap& tap : taps) {
      if (tap.offset < kEarlyPartitionSamples) {
        early.push_back(tap);
      } else {
        late_ir[static_cast<size_t>(tap.offset - kEarlyPartitionSamples)] += tap.gain;
      }
    }
  };
  std::vector<float> late_ir_l;
  std::vector<float> late_ir_r;
  split_taps(taps_l_, early_taps_l_, late_ir_l);
  split_taps(taps_r_, early_taps_r_, late_ir_r);
  late_l_.set_impulse_response(late_ir_l);
  late_r_.set_impulse_response(late_ir_r);
  late_input_.assign(kEarlyPartitionSamples, 0.0f);
  late_output_l_.assign(kEarlyPartitionSamples, 0.0f);
  late_output_r_.assign(kEarlyPartitionSamples, 0.0f);
  late_fill_count_ = 0;

  shelf_pole_ = std::exp(-kTwoPi * kShelfCutoffHz / static_cast<float>(sr));
  shelf_b0_ = 1.0f - shelf_pole_;

  dc_blocker_.prepare(sample_rate, max_block_size);
  reset();
}

void VelvetReverb::process(float* const* channels, int num_channels, int num_samples) {
  rt::ScopedNoDenormals no_denormals;
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0 || channels[0] == nullptr) {
    return;
  }
  float* left = channels[0];
  float* right = num_channels > 1 && channels[1] != nullptr ? channels[1] : channels[0];

  // Block-rate dry/wet: smoothed across blocks by the engine parameter slot
  // smoother, not per-sample (see Chorus::process for the rationale).
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  const bool stereo = right != left;

  for (int i = 0; i < num_samples; ++i) {
    const float in_l = left[i];
    const float in_r = right[i];
    const float input = stereo ? 0.5f * (in_l + in_r) : in_l;

    ring_l_.write(input);
    ring_r_.write(input);

    float wet_l = late_output_l_[static_cast<size_t>(late_fill_count_)];
    for (const Tap& t : early_taps_l_) {
      wet_l += t.gain * ring_l_.read_at(t.offset);
    }
    float wet_r = late_output_r_[static_cast<size_t>(late_fill_count_)];
    for (const Tap& t : early_taps_r_) {
      wet_r += t.gain * ring_r_.read_at(t.offset);
    }

    late_input_[static_cast<size_t>(late_fill_count_)] = input;
    if (++late_fill_count_ == kEarlyPartitionSamples) {
      late_l_.process_block(late_input_.data(), late_output_l_.data());
      late_r_.process_block(late_input_.data(), late_output_r_.data());
      late_fill_count_ = 0;
    }

    if (config_.enable_shelf) {
      shelf_state_l_ += shelf_b0_ * (wet_l - shelf_state_l_);
      wet_l = shelf_state_l_ + kShelfHighGain * (wet_l - shelf_state_l_);
      shelf_state_r_ += shelf_b0_ * (wet_r - shelf_state_r_);
      wet_r = shelf_state_r_ + kShelfHighGain * (wet_r - shelf_state_r_);
    }

    wet_l = dc_blocker_.process_sample(0, wet_l);
    wet_r = dc_blocker_.process_sample(1, wet_r);

    if (stereo) {
      left[i] = dry * in_l + wet * wet_l;
      right[i] = dry * in_r + wet * wet_r;
    } else {
      // Mono: collapse the two tap-table outputs into the single output buffer
      // so it is not written twice with different values.
      left[i] = dry * in_l + wet * 0.5f * (wet_l + wet_r);
    }
  }
}

int VelvetReverb::tail_samples() const noexcept {
  if (std::clamp(config_.dry_wet, 0.0f, 1.0f) <= 0.0f) return 0;
  // The velvet-noise taps span one effective T60 (the same rt60 the tap tables
  // are built for in prepare()), so the tail decays over that window.
  const float rt60 = effective_rt60(config_);
  const double samples = static_cast<double>(rt60) * sample_rate_;
  if (samples <= 0.0) return 0;
  if (samples >= static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(std::ceil(samples));
}

bool VelvetReverb::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      // Rebuilds tap tables / ring buffers (not RT-safe). prepare() clamps.
      config_.decay = value;
      if (prepared_) prepare(sample_rate_, max_block_size_);
      return true;
    case 1:
      config_.reverb_time_s = value;
      if (prepared_) prepare(sample_rate_, max_block_size_);
      return true;
    case 2:
      // RT-safe: only read in process(), where it is clamped to [0, 1].
      config_.dry_wet = value;
      return true;
    case 3:
      config_.density_hz = value;
      if (prepared_) prepare(sample_rate_, max_block_size_);
      return true;
    default:
      return false;
  }
}

bool VelvetReverb::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  return param_id == 2;
}

std::vector<rt::ParamDescriptor> VelvetReverb::parameter_descriptors() const {
  return {{"decay", 0}, {"reverbTimeS", 1}, {"dryWet", 2}, {"densityHz", 3}};
}

void VelvetReverb::reset() {
  ring_l_.reset();
  ring_r_.reset();
  late_l_.reset();
  late_r_.reset();
  std::fill(late_input_.begin(), late_input_.end(), 0.0f);
  std::fill(late_output_l_.begin(), late_output_l_.end(), 0.0f);
  std::fill(late_output_r_.begin(), late_output_r_.end(), 0.0f);
  late_fill_count_ = 0;
  shelf_state_l_ = 0.0f;
  shelf_state_r_ = 0.0f;
  dc_blocker_.reset();
}

}  // namespace sonare::effects::reverb
