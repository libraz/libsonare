#pragma once

/// @file dattorro_reverb.h
/// @brief Dattorro plate reverb (JAES 1997) with input diffusion and a
///        modulated figure-8 tank.

#include <cstddef>
#include <vector>

#include "rt/processor_base.h"

namespace sonare::effects::reverb {

struct DattorroReverbConfig {
  float decay = 0.5f;             ///< Tank feedback / tail length, clamped to [0, 0.98].
  float damping = 0.5f;           ///< HF damping, mapped to one-pole d = damping * 0.4.
  float dry_wet = 0.35f;          ///< Wet mix amount, [0, 1].
  float mod_rate_hz = 0.5f;       ///< Tank allpass modulation rate.
  float mod_depth_samples = 6.0f; ///< Modulation depth (at reference rate 29761 Hz).
  float pre_delay_samples = 0.0f; ///< Input pre-delay (at reference rate 29761 Hz).
};

class DattorroReverb : public rt::ProcessorBase {
 public:
  explicit DattorroReverb(DattorroReverbConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

 private:
  /// @brief Schroeder allpass: out = -g*in + buf[read]; buf[write] = in + g*out.
  struct Allpass {
    std::vector<float> buf;
    size_t size = 1;
    size_t index = 0;
    float gain = 0.0f;

    void prepare(size_t length, float g);
    void reset();
    float process(float in);
    float read_at(size_t offset) const;  ///< Read offset samples behind the write head.
  };

  /// @brief LFO-modulated allpass; delay length = base + round(depth*sin(phase)).
  struct ModAllpass {
    std::vector<float> buf;
    size_t capacity = 1;
    size_t base = 1;
    size_t index = 0;
    float gain = 0.0f;

    void prepare(size_t base_len, size_t max_depth, float g);
    void reset();
    float process(float in, float mod_offset);
  };

  /// @brief Plain delay with multi-tap reads, write/advance decoupled.
  /// @details Capacity is length+1 so that read_at(length) (the main delay
  ///          output) addresses the sample written `length` steps ago.
  ///          read_at(0) returns the value written this step.
  struct TapDelay {
    std::vector<float> buf;
    size_t cap = 2;
    size_t length = 1;
    size_t index = 0;

    void prepare(size_t delay_length);
    void reset();
    void write(float in);
    void advance();
    float read_at(size_t offset) const;
  };

  DattorroReverbConfig config_{};
  double sample_rate_ = 48000.0;

  // Stage 1 input diffusion.
  std::vector<float> pre_delay_buf_;
  size_t pre_delay_len_ = 0;
  size_t pre_delay_index_ = 0;
  Allpass in_ap_[4];

  // Stage 2 figure-8 tank.
  ModAllpass mod_ap_l_;
  ModAllpass mod_ap_r_;
  TapDelay delay_l1_;
  TapDelay delay_l2_;
  TapDelay delay_r1_;
  TapDelay delay_r2_;
  Allpass decay_ap_l_;
  Allpass decay_ap_r_;
  float damp_l_ = 0.0f;
  float damp_r_ = 0.0f;
  float tail_l_ = 0.0f;
  float tail_r_ = 0.0f;

  // Output tap offsets (scaled to the working sample rate).
  size_t tap_l_l1a_ = 0, tap_l_l1b_ = 0, tap_l_apl_ = 0, tap_l_l2_ = 0;
  size_t tap_l_r1_ = 0, tap_l_apr_ = 0, tap_l_r2_ = 0;
  size_t tap_r_r1a_ = 0, tap_r_r1b_ = 0, tap_r_apr_ = 0, tap_r_r2_ = 0;
  size_t tap_r_l1_ = 0, tap_r_apl_ = 0, tap_r_l2_ = 0;

  // Modulation.
  float lfo_phase_l_ = 0.0f;
  float lfo_phase_r_ = 0.0f;
  float lfo_inc_ = 0.0f;
  float mod_depth_ = 0.0f;
};

}  // namespace sonare::effects::reverb
