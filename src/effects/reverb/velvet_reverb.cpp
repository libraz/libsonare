#include "effects/reverb/velvet_reverb.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::effects::reverb {

using sonare::constants::kTwoPi;

namespace {
constexpr float kShelfCutoffHz = 6000.0f;
constexpr float kT60Drop = 1000.0f;  // 60 dB amplitude ratio for T60.
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

  const float rho = std::clamp(config_.density_hz, 1000.0f, 3000.0f);
  const int grid_ls = std::max(1, static_cast<int>(std::lround(sr / rho)));
  // decay scales the base reverb time around its nominal value.
  const float rt60 =
      std::max(0.05f, config_.reverb_time_s * (0.5f + std::clamp(config_.decay, 0.0f, 1.0f)));
  const int n_seg = std::max(2, static_cast<int>(std::lround(rt60 * sr)));
  const int num_pulses = std::max(1, static_cast<int>(std::lround(rho * rt60)));
  const float decay_rate = std::log(kT60Drop) / rt60;

  ring_l_.prepare(n_seg);
  ring_r_.prepare(n_seg);
  build_table(taps_l_, 0u, grid_ls, n_seg, num_pulses, decay_rate, sr);
  build_table(taps_r_, static_cast<std::uint32_t>(num_pulses), grid_ls, n_seg, num_pulses,
              decay_rate, sr);

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

  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  const bool stereo = right != left;

  for (int i = 0; i < num_samples; ++i) {
    const float in_l = left[i];
    const float in_r = right[i];
    const float input = stereo ? 0.5f * (in_l + in_r) : in_l;

    ring_l_.write(input);
    ring_r_.write(input);

    float wet_l = 0.0f;
    for (const Tap& t : taps_l_) {
      wet_l += t.gain * ring_l_.read_at(t.offset);
    }
    float wet_r = 0.0f;
    for (const Tap& t : taps_r_) {
      wet_r += t.gain * ring_r_.read_at(t.offset);
    }

    if (config_.enable_shelf) {
      shelf_state_l_ += shelf_b0_ * (wet_l - shelf_state_l_);
      wet_l = shelf_state_l_;
      shelf_state_r_ += shelf_b0_ * (wet_r - shelf_state_r_);
      wet_r = shelf_state_r_;
    }

    left[i] = dry * in_l + wet * wet_l;
    right[i] = dry * in_r + wet * wet_r;
  }

  dc_blocker_.process(channels, num_channels, num_samples);
}

void VelvetReverb::reset() {
  ring_l_.reset();
  ring_r_.reset();
  shelf_state_l_ = 0.0f;
  shelf_state_r_ = 0.0f;
  dc_blocker_.reset();
}

}  // namespace sonare::effects::reverb
