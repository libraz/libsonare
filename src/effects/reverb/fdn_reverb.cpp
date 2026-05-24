#include "effects/reverb/fdn_reverb.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::effects::reverb {

using sonare::constants::kEpsilon;

FdnReverb::FdnReverb(FdnReverbConfig config) : config_(config) {}

void FdnReverb::prepare(double sample_rate, int max_block_size) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  const double sr = sample_rate_;
  // Reference delay lengths defined at 48 kHz, scaled to the working rate.
  const std::array<double, 4> seconds{{0.0311, 0.0377, 0.0419, 0.0533}};
  for (size_t i = 0; i < delays_.size(); ++i) {
    const size_t len = static_cast<size_t>(std::max(1.0, seconds[i] * sr));
    delays_[i].prepare(len);
    lengths_[i] = static_cast<int>(len);
  }

  prepared_ = true;
  update_absorption();

  dc_blocker_.prepare(sample_rate, max_block_size);
  reset();
}

void FdnReverb::update_absorption() {
  // Per-line absorption: derive one-pole coefficients from LF/HF T60 targets.
  const float t60_lf = std::max(0.01f, std::clamp(config_.decay, 0.0f, 1.5f) * 10.0f);
  const float hf_damping = std::clamp(config_.hf_damping, 0.0f, 1.0f);
  const float t60_hf = t60_lf * (1.0f - 0.9f * hf_damping);
  const float sr = static_cast<float>(sample_rate_);
  for (size_t i = 0; i < delays_.size(); ++i) {
    const float d = static_cast<float>(lengths_[i]);
    const float g_lf = std::pow(10.0f, -3.0f * d / (t60_lf * sr));
    const float g_hf = std::pow(10.0f, -3.0f * d / (t60_hf * sr));
    const float denom = g_lf + g_hf + kEpsilon;
    a1_[i] = std::clamp((g_lf - g_hf) / denom, -0.999f, 0.999f);
    b0_[i] = std::clamp(2.0f * g_lf * g_hf / denom, 0.0f, 1.0f);
  }
}

void FdnReverb::process(float* const* channels, int num_channels, int num_samples) {
  rt::ScopedNoDenormals no_denormals;
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0 || channels[0] == nullptr) {
    return;
  }
  float* left = channels[0];
  float* right = num_channels > 1 && channels[1] != nullptr ? channels[1] : channels[0];
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;

  for (int i = 0; i < num_samples; ++i) {
    const float in_l = left[i];
    const float in_r = right[i];
    const float input = 0.5f * (in_l + in_r);

    // Per-line absorption: y = b0*x + a1*y_prev applied to the delay outputs.
    std::array<float, 4> absorbed{};
    for (size_t k = 0; k < 4; ++k) {
      absorbed[k] = b0_[k] * state_[k] + a1_[k] * filt_state_[k];
      filt_state_[k] = absorbed[k];
    }

    // Unnormalized 4x4 Hadamard with 0.5 scaling.
    const float h0 = absorbed[0] + absorbed[1] + absorbed[2] + absorbed[3];
    const float h1 = absorbed[0] - absorbed[1] + absorbed[2] - absorbed[3];
    const float h2 = absorbed[0] + absorbed[1] - absorbed[2] - absorbed[3];
    const float h3 = absorbed[0] - absorbed[1] - absorbed[2] + absorbed[3];

    state_[0] = delays_[0].process(input + 0.5f * h0);
    state_[1] = delays_[1].process(input + 0.5f * h1);
    state_[2] = delays_[2].process(input + 0.5f * h2);
    state_[3] = delays_[3].process(input + 0.5f * h3);

    left[i] = dry * in_l + wet * (state_[0] - state_[2]);
    right[i] = dry * in_r + wet * (state_[1] - state_[3]);
  }

  dc_blocker_.process(channels, num_channels, num_samples);
}

bool FdnReverb::set_parameter(unsigned int param_id, float value) {
  // update_absorption() depends on the prepared delay lengths and sample rate.
  if (!prepared_) return false;
  switch (param_id) {
    case 0:
      config_.decay = value;
      // Recompute absorption coefficients in place; preserves filter/delay state.
      update_absorption();
      return true;
    case 1:
      config_.hf_damping = value;
      update_absorption();
      return true;
    case 2:
      config_.dry_wet = value;
      return true;
    default:
      return false;
  }
}

void FdnReverb::reset() {
  for (auto& delay : delays_) {
    delay.reset();
  }
  state_ = {0.0f, 0.0f, 0.0f, 0.0f};
  filt_state_ = {0.0f, 0.0f, 0.0f, 0.0f};
  dc_blocker_.reset();
}

}  // namespace sonare::effects::reverb
