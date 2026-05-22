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
};

struct JilesAthertonState {
  float magnetization = 0.0f;
  float previous_field = 0.0f;
};

class JilesAtherton {
 public:
  explicit JilesAtherton(JilesAthertonConfig config = {});

  void set_config(const JilesAthertonConfig& config);
  const JilesAthertonConfig& config() const noexcept { return config_; }

  float process(JilesAthertonState& state, float field) const;
  static void reset(JilesAthertonState& state) noexcept;

  static float langevin(float x);
  static float langevin_derivative(float x);

 private:
  static void validate_config(const JilesAthertonConfig& config);

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
