#pragma once

/// @file dattorro_reverb.h
/// @brief Dattorro plate reverb (JAES 1997) with input diffusion and a
///        modulated figure-8 tank.

#include <cstddef>
#include <vector>

#include "effects/common/mix_law.h"
#include "rt/processor_base.h"
#include "util/constants.h"

namespace sonare::effects::reverb {

/// Behaviour of the output gate once it has opened.
enum class DattorroGateType {
  kNormal,   ///< Full level until the hold runs out, then closed.
  kReverse,  ///< Level ramps up over the hold time, then closed.
  kSweep1,   ///< Normal gate; the wet image sweeps left to right over the hold time.
  kSweep2,   ///< Normal gate; the wet image sweeps right to left over the hold time.
};
inline constexpr int kDattorroGateTypeCount = 4;

/// Highest `character` value; 0 is the canonical tank and 1-6 are the six sets below.
inline constexpr int kDattorroMaxCharacter = 6;

/// Gate thresholds at or below this (dB) leave the gate off.
inline constexpr float kDattorroGateOffDb = sonare::constants::kFloorDb;

struct DattorroReverbConfig {
  float decay = 0.5f;              ///< Tank feedback / tail length, clamped to [0, 0.98].
  float damping = 0.5f;            ///< HF damping, mapped to one-pole d = damping * 0.4.
  float dry_wet = 0.35f;           ///< Wet mix amount, [0, 1].
  float mod_rate_hz = 0.5f;        ///< Tank allpass modulation rate.
  float mod_depth_samples = 6.0f;  ///< Modulation depth (reference rate 29761 Hz), at most 672.
  /// Input pre-delay (at reference rate 29761 Hz). The ring holds the larger of this and
  /// the GS ceiling; a live change clamps to that size.
  float pre_delay_samples = 0.0f;
  /// Corner of the tank's one-pole damping low-pass, in Hz, built at the working
  /// rate. Above 0 it replaces `damping`; 0 keeps `damping`.
  float damping_hz = 0.0f;
  /// Gate on the wet output: it closes `gate_hold_ms` after the wet level last
  /// exceeded this threshold. At or below kDattorroGateOffDb the gate is off.
  float gate_threshold_db = kDattorroGateOffDb;
  float gate_hold_ms = 100.0f;  ///< Hold time after the level falls below the threshold.
  DattorroGateType gate_type = DattorroGateType::kNormal;
  /// Tank length set, in [0, kDattorroMaxCharacter]: 0 is the canonical tank, 1-6 are
  /// Room 1, Room 2, Stage 1, Stage 2, Hall 1, Hall 2. The sets are ratios applied to the
  /// four tank delay lines and the output taps that read them. The maximum line sizes are
  /// prepared up front, so this can be changed by its realtime parameter.
  int character = 0;
  /// How dry_wet maps onto the dry and wet gains.
  common::MixLaw mix_law = common::MixLaw::kCrossfade;
};

class DattorroReverb : public rt::ProcessorBase {
 public:
  /// Reference rate from Dattorro's tables; all delay lengths scale by sr/this.
  /// Pre-delay / modulation depth in DattorroReverbConfig are expressed at this
  /// rate and rescaled to the working sample rate in prepare().
  static constexpr double kReferenceSampleRate = 29761.0;

  explicit DattorroReverb(DattorroReverbConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int tail_samples() const noexcept override;

  // Automatable parameters:
  //   0 = decay (clamped to [0, 0.98] in process())
  //   1 = damping (clamped to [0, 1] in process())
  //   2 = dry_wet (clamped to [0, 1] in process())
  //   3 = mod_rate_hz (recomputes the LFO increment in place)
  //   4 = mod_depth_samples, [0, 672] reference samples (the shorter modulated allpass);
  //       refused outside it. May grow the modulated allpass buffers when the requested
  //       depth exceeds the prepared guard
  //   5 = damping_hz (0 restores `damping`), 6 = gate_threshold_db,
  //   7 = gate_hold_ms, 8 = gate_type (a whole number naming a type, refused otherwise)
  //   9 = pre_delay_ms, 10 = character (a whole number naming a tank set)
  //   11 = mix_law (a whole number naming a law, refused otherwise)
  bool set_parameter_impl(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  /// @brief One-pole coefficient `a` of `y += a * (x - y)` whose response is 3 dB down at
  ///        @p corner_hz when running at @p sample_rate.
  static float damping_coefficient(double corner_hz, double sample_rate) noexcept;

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
    void ensure_capacity(size_t max_depth);
    void reset();
    float process(float in, float mod_offset);
  };

  /// @brief Plain delay with multi-tap reads, write/advance decoupled.
  /// @details Capacity is the prepared maximum length plus one; `length` is the
  ///          active character's delay and stays within that physical ring.
  ///          read_at(length) addresses the sample written `length` steps ago,
  ///          while read_at(0) returns the value written this step.
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

  /// Applies the current pre-delay in the already allocated ring.
  void update_pre_delay_length() noexcept;
  /// Applies the current character's line lengths and output taps.
  void update_character_geometry() noexcept;

  /// Returns the tank to rest once a non-finite value has reached it, once per
  /// block (see util/non_finite_state.h).
  void discard_non_finite() noexcept;

  DattorroReverbConfig config_{};
  double sample_rate_ = 48000.0;
  float max_pre_delay_ms_ = 0.0f;  ///< largest pre-delay the prepared ring holds.

  // Stage 1 input diffusion.
  std::vector<float> pre_delay_buf_;  ///< Fixed-capacity history for all live pre-delay values.
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

  // Output gate (wet path).
  float gate_env_ = 0.0f;   ///< Peak envelope of the ungated wet output.
  float gate_gain_ = 1.0f;  ///< Smoothed gate gain.
  int gate_hold_left_ = 0;  ///< Samples until an open gate closes.
  int gate_elapsed_ = 0;    ///< Samples since the gate opened.
  bool gate_open_ = false;

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
