#include "mastering/saturation/amp_presets.h"

#include "mastering/saturation/amp_physics.h"
#include "util/exception.h"

namespace sonare::mastering::saturation {

namespace {

/// @brief The supply of a rig, in the units its schematic is drawn in.
/// @details Written out per preset rather than as a `sag` number so the value in
///          the source is the amp's, not a taste judgement: `sag_from_supply()`
///          turns it into the control. The B+ and idle-current figures are
///          representative of the class of amp, not a measurement of one unit.
struct Supply {
  Rectifier rectifier;
  float transformer_ohms;
  float b_plus_v;
  float full_output_current_a;
};

float sag_for(const Supply& supply) {
  return sag_from_supply(rectifier_resistance_ohms(supply.rectifier) + supply.transformer_ohms,
                         supply.full_output_current_a, supply.b_plus_v);
}

/// A rig with no cabinet and no mic, used as the base every preset fills in.
/// Starting from a zeroed rig rather than from AmpSimConfig's defaults is
/// deliberate: a preset must describe a whole amp, so a field nobody sets here
/// should read as "off", not as "whatever the struct happens to default to".
AmpSimConfig bare() {
  AmpSimConfig c;
  c.topology = AmpTopology::kVoiced;
  c.drive = 0.5f;
  c.bass_db = 0.0f;
  c.mid_db = 0.0f;
  c.treble_db = 0.0f;
  c.presence_db = 0.0f;
  c.cab = true;
  c.cab_model = CabModel::kGuitar4x12;
  c.amp_model = AmpModel::kClassicCrunch;
  c.level_db = 0.0f;
  c.power = 0.0f;
  c.sag = 0.0f;
  c.transformer = 0.0f;
  c.nfb = 0.0f;
  c.mic_model = MicModel::kNone;
  c.mic_axis = 0.0f;
  c.mic_distance_cm = kMicReferenceDistanceCm;
  c.mic_blend = 0.0f;
  c.mic_b_model = MicModel::kNone;
  c.mic_b_axis = 0.0f;
  c.mic_b_distance_cm = 15.0f;
  c.mic_b_invert = false;
  c.cone = 0.0f;
  c.doppler = 0.0f;
  c.preamp_stages = 2;
  c.bias_shift = 0.0f;
  c.crossover = 0.0f;
  c.power_tube = PowerTube::k6L6;
  return c;
}

AmpSimConfig clean_combo() {
  AmpSimConfig c = bare();
  c.amp_model = AmpModel::kFenderClean;
  c.drive = 0.25f;
  c.bass_db = 2.0f;
  c.mid_db = -1.0f;
  c.treble_db = 3.0f;
  c.presence_db = 1.5f;
  c.mic_model = MicModel::kDynamic;
  c.mic_axis = 0.35f;
  c.mic_distance_cm = 3.0f;
  c.power = 0.15f;
  // A big blackface-style combo: GZ34, a 440 V rail, a 6L6 pair drawing 150 mA
  // at full output. The stiffest supply in the set, which is most of why this
  // rig stays clean.
  c.sag = sag_for({Rectifier::kGz34, kTypicalPowerTransformerOhms, 440.0f, 0.150f});
  // Biased hot, so the pair conducts through the crossing.
  c.crossover = crossover_from_bias_fraction(0.70f);
  c.nfb = 0.4f;
  return c;
}

AmpSimConfig chime_edge() {
  AmpSimConfig c = bare();
  c.amp_model = AmpModel::kVoxChime;
  c.drive = 0.45f;
  c.mid_db = 1.5f;
  c.treble_db = 3.5f;
  c.presence_db = 2.0f;
  c.mic_model = MicModel::kCondenser;
  c.mic_axis = 0.2f;
  c.mic_distance_cm = 4.0f;
  c.power_tube = PowerTube::kEL84;
  c.power = 0.35f;
  // GZ34 on a 320 V rail: a modest supply, but a stiff rectifier.
  c.sag = sag_for({Rectifier::kGz34, kTypicalPowerTransformerOhms, 320.0f, 0.120f});
  // Cathode-biased and idling very hot, effectively class A: no dead zone.
  c.crossover = crossover_from_bias_fraction(0.90f);
  c.transformer = 0.15f;
  // No global feedback: the top-boost class-A circuit this is voiced after runs
  // open loop, and that is exactly what keeps its high end open.
  c.nfb = 0.0f;
  return c;
}

AmpSimConfig classic_crunch() {
  AmpSimConfig c = bare();
  c.amp_model = AmpModel::kClassicCrunch;
  c.drive = 0.6f;
  c.presence_db = 1.0f;
  c.mic_model = MicModel::kDynamic;
  c.mic_axis = 0.3f;
  c.mic_distance_cm = 2.5f;
  c.power = 0.4f;
  // An early British stack: still tube-rectified (GZ34) on a 440 V rail.
  c.sag = sag_for({Rectifier::kGz34, kTypicalPowerTransformerOhms, 440.0f, 0.130f});
  c.crossover = crossover_from_bias_fraction(0.65f);
  c.transformer = 0.2f;
  c.nfb = 0.3f;
  return c;
}

AmpSimConfig tweed_grind() {
  AmpSimConfig c = bare();
  c.topology = AmpTopology::kCircuit;
  c.amp_model = AmpModel::kTweed;
  c.preamp_stages = 2;
  c.drive = 0.62f;
  c.bass_db = 3.0f;
  c.mid_db = 1.0f;
  c.treble_db = -2.0f;
  c.power_tube = PowerTube::k6V6;
  c.power = 0.65f;
  // The softest supply in the set, and the reason this amp is known for its
  // sponginess: a 5Y3 (350 ohms) plus the transformer on a 350 V rail drops
  // about 50 V at full output — the figure a small tweed combo is measured at.
  c.sag = sag_for({Rectifier::k5Y3, kTypicalPowerTransformerOhms, 350.0f, 0.100f});
  c.transformer = 0.35f;
  c.nfb = 0.0f;
  // Cathode-biased, so it idles close to class A: no crossover notch.
  c.crossover = crossover_from_bias_fraction(0.90f);
  c.bias_shift = 0.15f;
  c.mic_model = MicModel::kDynamic;
  c.mic_axis = 0.45f;
  c.mic_distance_cm = 3.0f;
  c.level_db = 1.0f;
  return c;
}

AmpSimConfig brit_stack() {
  AmpSimConfig c = bare();
  c.topology = AmpTopology::kCircuit;
  c.amp_model = AmpModel::kClassicCrunch;
  c.preamp_stages = 3;
  c.drive = 0.7f;
  c.bass_db = -1.0f;
  c.mid_db = 1.0f;
  c.treble_db = 3.0f;
  c.presence_db = 2.0f;
  c.power_tube = PowerTube::kEL34;
  c.power = 0.6f;
  // Silicon-rectified: only the transformer's own resistance is left, so this
  // rig barely sags however hard it is pushed.
  c.sag = sag_for({Rectifier::kSolidState, 120.0f, 470.0f, 0.180f});
  c.transformer = 0.25f;
  c.nfb = 0.35f;
  c.crossover = crossover_from_bias_fraction(0.60f);
  c.bias_shift = 0.2f;
  c.mic_model = MicModel::kDynamic;
  c.mic_axis = 0.25f;
  c.mic_distance_cm = 2.5f;
  // The standard stack capture: a close dynamic for the edge, a darker ribbon
  // further back for the body, combing through their path-length difference.
  c.mic_blend = 0.3f;
  c.mic_b_model = MicModel::kRibbon;
  c.mic_b_axis = 0.6f;
  c.mic_b_distance_cm = 12.0f;
  return c;
}

AmpSimConfig modern_lead() {
  AmpSimConfig c = bare();
  c.topology = AmpTopology::kCircuit;
  c.amp_model = AmpModel::kModernHiGain;
  c.preamp_stages = 4;
  c.drive = 0.8f;
  c.bass_db = 1.0f;
  c.mid_db = -3.0f;
  c.treble_db = 4.0f;
  c.presence_db = 3.0f;
  c.power_tube = PowerTube::kEL34;
  c.power = 0.55f;
  // A modern high-gain amp is silicon-rectified and deliberately stiff: the
  // sponginess a vintage supply gives would blur a fast palm-muted attack.
  c.sag = sag_for({Rectifier::kSolidState, 100.0f, 480.0f, 0.200f});
  c.transformer = 0.2f;
  c.nfb = 0.45f;
  c.crossover = crossover_from_bias_fraction(0.60f);
  c.bias_shift = 0.35f;
  c.cone = 0.25f;
  c.mic_model = MicModel::kDynamic;
  c.mic_axis = 0.2f;
  c.mic_distance_cm = 2.5f;
  c.level_db = -1.0f;
  return c;
}

AmpSimConfig rectifier_chug() {
  AmpSimConfig c = bare();
  c.topology = AmpTopology::kCircuit;
  c.amp_model = AmpModel::kRectifier;
  c.preamp_stages = 4;
  c.drive = 0.85f;
  c.bass_db = 5.0f;
  c.mid_db = -6.0f;
  c.treble_db = 2.0f;
  c.presence_db = 1.0f;
  c.power_tube = PowerTube::k6L6;
  c.power = 0.7f;
  // The one modern rig here running its tube-rectifier option (5U4): the same
  // circuit on silicon would be far stiffer, and the choice is the voice.
  c.sag = sag_for({Rectifier::k5U4, 120.0f, 480.0f, 0.220f});
  c.transformer = 0.4f;
  c.nfb = 0.2f;
  // Biased cool, as modern high-power amps usually are.
  c.crossover = crossover_from_bias_fraction(0.50f);
  c.bias_shift = 0.45f;
  c.cone = 0.4f;
  c.mic_model = MicModel::kDynamic;
  c.mic_axis = 0.3f;
  c.mic_distance_cm = 2.5f;
  c.mic_blend = 0.35f;
  c.mic_b_model = MicModel::kCondenser;
  c.mic_b_axis = 0.5f;
  c.mic_b_distance_cm = 20.0f;
  c.level_db = -2.0f;
  return c;
}

AmpSimConfig cold_bias_buzz() {
  AmpSimConfig c = bare();
  c.topology = AmpTopology::kCircuit;
  c.amp_model = AmpModel::kClassicCrunch;
  c.preamp_stages = 2;
  c.drive = 0.5f;
  c.power_tube = PowerTube::kEL84;
  c.power = 0.75f;
  // The point of the rig: idling at 0.35 of maximum dissipation is a genuinely
  // cold bias, cold enough that the dead zone is audible on quiet playing and
  // closes up as the signal clears it.
  c.crossover = crossover_from_bias_fraction(0.35f);
  c.bias_shift = 0.1f;
  c.sag = sag_for({Rectifier::kGz34, kTypicalPowerTransformerOhms, 320.0f, 0.100f});
  c.transformer = 0.3f;
  c.nfb = 0.15f;
  c.mic_model = MicModel::kDynamic;
  c.mic_axis = 0.4f;
  c.mic_distance_cm = 3.0f;
  return c;
}

AmpSimConfig bass_di() {
  AmpSimConfig c = bare();
  c.amp_model = AmpModel::kFenderClean;
  c.drive = 0.15f;
  c.bass_db = 2.0f;
  c.treble_db = 1.0f;
  // No cabinet: this is the rig to put in front of a host cab IR, or to blend
  // against a mic'd track.
  c.cab = false;
  c.power = 0.2f;
  c.sag = sag_for({Rectifier::kSolidState, 100.0f, 450.0f, 0.150f});
  c.crossover = crossover_from_bias_fraction(0.65f);
  c.transformer = 0.3f;
  c.nfb = 0.3f;
  c.level_db = -1.0f;
  return c;
}

AmpSimConfig bass_rig() {
  AmpSimConfig c = bare();
  c.amp_model = AmpModel::kClassicCrunch;
  c.cab_model = CabModel::kBass8x10;
  c.drive = 0.35f;
  c.bass_db = 3.0f;
  c.mid_db = -1.0f;
  c.treble_db = 1.0f;
  c.power = 0.45f;
  // A big bass head: silicon-rectified on a high rail, drawing far more current
  // than any of the guitar rigs — the current is what keeps its droop
  // comparable despite the stiff supply.
  c.sag = sag_for({Rectifier::kSolidState, 80.0f, 600.0f, 0.350f});
  c.crossover = crossover_from_bias_fraction(0.60f);
  // A bass amp is where the output transformer matters most: the core
  // saturates on flux, and a bass signal is where the flux is.
  c.transformer = 0.55f;
  c.nfb = 0.25f;
  c.cone = 0.3f;
  c.mic_model = MicModel::kDynamic;
  c.mic_axis = 0.5f;
  c.mic_distance_cm = 5.0f;
  return c;
}

}  // namespace

std::vector<std::string> amp_preset_names() {
  return {"cleanCombo", "chimeEdge",     "classicCrunch", "tweedGrind", "britStack",
          "modernLead", "rectifierChug", "coldBiasBuzz",  "bassDi",     "bassRig"};
}

const char* amp_preset_to_string(AmpPreset preset) noexcept {
  switch (preset) {
    case AmpPreset::kCleanCombo:
      return "cleanCombo";
    case AmpPreset::kChimeEdge:
      return "chimeEdge";
    case AmpPreset::kClassicCrunch:
      return "classicCrunch";
    case AmpPreset::kTweedGrind:
      return "tweedGrind";
    case AmpPreset::kBritStack:
      return "britStack";
    case AmpPreset::kModernLead:
      return "modernLead";
    case AmpPreset::kRectifierChug:
      return "rectifierChug";
    case AmpPreset::kColdBiasBuzz:
      return "coldBiasBuzz";
    case AmpPreset::kBassDi:
      return "bassDi";
    case AmpPreset::kBassRig:
      return "bassRig";
  }
  return "unknown";
}

AmpPreset amp_preset_from_string(const std::string& name) {
  const std::vector<std::string> names = amp_preset_names();
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i] == name) return static_cast<AmpPreset>(i);
  }
  throw SonareException(ErrorCode::InvalidParameter, "unknown amp preset: " + name);
}

AmpSimConfig amp_preset_config(AmpPreset preset) {
  switch (preset) {
    case AmpPreset::kCleanCombo:
      return clean_combo();
    case AmpPreset::kChimeEdge:
      return chime_edge();
    case AmpPreset::kClassicCrunch:
      return classic_crunch();
    case AmpPreset::kTweedGrind:
      return tweed_grind();
    case AmpPreset::kBritStack:
      return brit_stack();
    case AmpPreset::kModernLead:
      return modern_lead();
    case AmpPreset::kRectifierChug:
      return rectifier_chug();
    case AmpPreset::kColdBiasBuzz:
      return cold_bias_buzz();
    case AmpPreset::kBassDi:
      return bass_di();
    case AmpPreset::kBassRig:
      return bass_rig();
  }
  throw SonareException(ErrorCode::InvalidParameter, "unknown amp preset");
}

}  // namespace sonare::mastering::saturation
