#pragma once

/// @file amp_presets.h
/// @brief Named whole-amp starting points for the amp-sim insert.
///
/// The amp's discrete switches — `topology`, `amp_model`, `cab_model`,
/// `mic_model`, `power_tube`, `preamp_stages` — are deliberately NOT automatable
/// (each one either moves the reported latency, re-prepares state, or jumps a
/// delay tap). They can therefore only be reached through the config, and a rig
/// is the interaction of six of them plus a dozen continuous controls. A preset
/// is what makes that reachable: one name selects a whole coherent amp, and the
/// caller then rides the knobs on top.
///
/// This mirrors `mastering::api::preset_config()` one level down: the returned
/// config is a plain value the caller may inspect and mutate freely. Presets are
/// code, not data — no table is loaded and nothing is embedded in the binary
/// beyond the switch itself.

#include <string>
#include <vector>

#include "mastering/saturation/amp_sim.h"

namespace sonare::mastering::saturation {

/// @brief Built-in amp-sim rig identifiers.
///
/// The three `kVoiced` entries are the cheap ones and stay on the original
/// filter chain; the `kCircuit` entries cost more CPU and are where the
/// cascade, the passive ladder and the class-AB behaviour live. Nothing here
/// changes the processor's defaults — an `AmpSim` built without a preset is
/// unchanged.
enum class AmpPreset {
  /// American clean combo: lots of headroom, a scooped-but-present voice,
  /// barely into the power stage. The pedal-platform / clean-rhythm setting.
  kCleanCombo = 0,
  /// British class-A chime: bright and airy, breaking up only on hard picking.
  /// No global feedback, which is what lets its top end stay open.
  kChimeEdge = 1,
  /// The original amp-sim character as a rig: mid-forward crunch with the power
  /// stage and a soft supply working.
  kClassicCrunch = 2,
  /// Low-wattage American tweed, circuit-level: a warm, dark, spongy breakup
  /// with a heavily sagging supply and no feedback loop.
  kTweedGrind = 3,
  /// British stack, circuit-level: a three-stage cascade into an EL34 pair,
  /// close dynamic mic blended with a darker ribbon further back.
  kBritStack = 4,
  /// Modern high-gain lead, circuit-level: a four-stage cascade, scooped mids,
  /// and enough grid-current tracking to sputter on hard attacks.
  kModernLead = 5,
  /// Modern high-gain rhythm: the heaviest rig here — deep scoop, a sagging
  /// supply, a saturating transformer and a hard-driven cone.
  kRectifierChug = 6,
  /// A coldly biased class-AB amp: the crossover notch is the voice rather than
  /// an artefact — buzzy and gated on quiet playing, cleaning up when pushed.
  kColdBiasBuzz = 7,
  /// Bass DI: the amp without a cabinet, for feeding a host cab IR or a DI
  /// blend downstream.
  kBassDi = 8,
  /// Bass 8x10 rig: an extended low end with the output transformer doing most
  /// of the thickening, and a cone worked hard enough to compress itself.
  kBassRig = 9,
};

/// @brief Returns string identifiers of all built-in amp presets, in display
///        order.
std::vector<std::string> amp_preset_names();

/// @brief Parses an amp-preset string identifier.
/// @throws SonareException (ErrorCode::InvalidParameter) if the name is unknown.
AmpPreset amp_preset_from_string(const std::string& name);

/// @brief Returns the canonical string identifier of an amp preset.
/// Returns "unknown" for invalid values; never throws.
const char* amp_preset_to_string(AmpPreset preset) noexcept;

/// @brief Returns an AmpSimConfig pre-populated for the given rig.
/// @details Every field is set explicitly, so the result does not depend on the
///          struct's own defaults. Callers may mutate the returned config; that
///          is the intended way to keep a preset's switches while overriding its
///          knobs.
AmpSimConfig amp_preset_config(AmpPreset preset);

}  // namespace sonare::mastering::saturation
