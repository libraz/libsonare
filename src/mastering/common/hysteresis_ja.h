#pragma once

/// @file hysteresis_ja.h
/// @brief Shared Jiles-Atherton hysteresis engine.

namespace sonare::mastering::common {

struct JilesAthertonConfig {
  /// Saturation magnetization.
  float saturation_magnetization = 1.0f;
  /// Anhysteretic shape parameter. Smaller values saturate earlier.
  float anhysteretic_shape = 0.3f;
  /// Loss/coercivity parameter. Larger values widen the hysteresis loop.
  float coercivity = 0.1f;
  /// Inter-domain mean-field coupling.
  float mean_field_coupling = 1.6e-3f;
  /// Reversibility ratio in [0, 1].
  float reversibility = 0.4f;
  /// Magnetic after-effect time constant in seconds. When positive, and a
  /// sample rate is supplied to process(), magnetization relaxes toward the
  /// anhysteretic curve while the drive field is held constant instead of
  /// staying where the last field change left it. Zero keeps the classic
  /// rate-independent behaviour.
  float viscosity_time_constant_s = 0.025f;
  /// Largest drive-field change integrated in one Euler step. A field change
  /// bigger than this is split into equal sub-steps, which keeps the
  /// loop-transition slope from overshooting its target when the field moves
  /// fast. Scale it with `coercivity`: the single-step scheme loses accuracy as
  /// the field change approaches that value. Zero keeps one step per sample.
  float max_field_step = 0.0f;
};

struct JilesAthertonState {
  float magnetization = 0.0f;
  float previous_field = 0.0f;
};

class JilesAtherton {
 public:
  /// Upper bound on the sub-steps one process() call may take, so the worst-case
  /// cost of an audio-thread call stays bounded no matter how large a field jump
  /// the caller hands in. A field change beyond kMaxSubSteps * max_field_step is
  /// integrated in sub-steps larger than max_field_step asks for.
  static constexpr int kMaxSubSteps = 32;

  explicit JilesAtherton(JilesAthertonConfig config = {});

  void set_config(const JilesAthertonConfig& config);
  const JilesAthertonConfig& config() const noexcept { return config_; }

  /// @brief Advances the magnetization state by one field sample.
  /// @param state Per-voice magnetization state.
  /// @param field Drive field for this sample.
  /// @param sample_rate Rate at which process() is being called, in Hz. Only
  ///        used for the held-field relaxation; pass 0 to disable it.
  float process(JilesAthertonState& state, float field, float sample_rate = 0.0f) const;
  static void reset(JilesAthertonState& state) noexcept;

  static float langevin(float x);
  static float langevin_derivative(float x);

 private:
  static void validate_config(const JilesAthertonConfig& config);
  /// Advances the state by one Euler step of `d_field`, evaluating the loop
  /// slope at `field`.
  void integrate_step(JilesAthertonState& state, float field, float d_field) const;

  JilesAthertonConfig config_{};
};

using JaParams = JilesAthertonConfig;
using HysteresisJa = JilesAtherton;

namespace jiles_atherton_presets {

JilesAthertonConfig oxide_tape();
JilesAthertonConfig tape();
JilesAthertonConfig silicon_steel();
JilesAthertonConfig mu_metal();

}  // namespace jiles_atherton_presets

namespace presets {

JilesAthertonConfig oxide_tape();
JilesAthertonConfig tape();
JilesAthertonConfig silicon_steel();
JilesAthertonConfig mu_metal();

}  // namespace presets

}  // namespace sonare::mastering::common
