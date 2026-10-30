#include "midi/synth/engine_field_gates.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace sonare::midi::synth {

namespace {

using Scope = ActiveCondition::Scope;
using Relation = ActiveCondition::Relation;
using Kind = ActiveCondition::Kind;

bool same_condition(const ActiveCondition& a, const ActiveCondition& b) {
  return a.key == b.key && a.scope == b.scope && a.relation == b.relation && a.kind == b.kind &&
         a.number == b.number && a.text == b.text;
}

/// Whether @p wide asks for every condition @p narrow does.
bool asks_all_of(const ActiveAlternative& wide, const ActiveAlternative& narrow) {
  for (const ActiveCondition& c : narrow) {
    bool found = false;
    for (const ActiveCondition& d : wide) found = found || same_condition(c, d);
    if (!found) return false;
  }
  return true;
}

/// A gate in disjunctive form: the field reaches the voice when every condition
/// of at least one alternative holds. The default, one empty alternative, is a
/// field that always does; no alternative at all is one that never does.
struct Gate {
  std::vector<ActiveAlternative> any{ActiveAlternative{}};
};

Gate never() {
  Gate g;
  g.any.clear();
  return g;
}

/// @p alternatives without any that another one already implies.
Gate absorbed(std::vector<ActiveAlternative> alternatives) {
  std::vector<bool> keep(alternatives.size(), true);
  for (size_t i = 0; i < alternatives.size(); ++i) {
    for (size_t j = 0; j < alternatives.size() && keep[i]; ++j) {
      if (i == j || !keep[j] || !asks_all_of(alternatives[i], alternatives[j])) continue;
      // Of two equal alternatives the first stays.
      if (i < j && asks_all_of(alternatives[j], alternatives[i])) continue;
      keep[i] = false;
    }
  }
  Gate out = never();
  for (size_t i = 0; i < alternatives.size(); ++i) {
    if (keep[i]) out.any.push_back(std::move(alternatives[i]));
  }
  return out;
}

/// Either gate.
Gate operator|(const Gate& a, const Gate& b) {
  std::vector<ActiveAlternative> all = a.any;
  all.insert(all.end(), b.any.begin(), b.any.end());
  return absorbed(std::move(all));
}

/// Both gates: every pairing of their alternatives.
Gate operator&(const Gate& a, const Gate& b) {
  std::vector<ActiveAlternative> all;
  for (const ActiveAlternative& x : a.any) {
    for (const ActiveAlternative& y : b.any) {
      ActiveAlternative merged = x;
      for (const ActiveCondition& c : y) {
        if (!asks_all_of(merged, {c})) merged.push_back(c);
      }
      all.push_back(std::move(merged));
    }
  }
  return absorbed(std::move(all));
}

Gate condition(std::string key, Scope scope, Relation relation, Kind kind, double number,
               std::string text = {}) {
  Gate g;
  g.any = {{ActiveCondition{std::move(key), scope, relation, kind, number, std::move(text)}}};
  return g;
}
Gate is(const std::string& key, bool value) {
  return condition(key, Scope::kEngine, Relation::kEq, Kind::kBoolean, value ? 1.0 : 0.0);
}
Gate above(const std::string& key, double value) {
  return condition(key, Scope::kEngine, Relation::kGt, Kind::kNumber, value);
}
Gate at_least(const std::string& key, double value) {
  return condition(key, Scope::kEngine, Relation::kGe, Kind::kNumber, value);
}
Gate equals(const std::string& key, double value) {
  return condition(key, Scope::kEngine, Relation::kEq, Kind::kNumber, value);
}
Gate differs(const std::string& key, double value) {
  return condition(key, Scope::kEngine, Relation::kNe, Kind::kNumber, value);
}
Gate wrapper_is(const std::string& key, const char* name) {
  return condition(key, Scope::kPatch, Relation::kEq, Kind::kString, 0.0, name);
}
Gate wrapper_above(const std::string& key, double value) {
  return condition(key, Scope::kPatch, Relation::kGt, Kind::kNumber, value);
}

/// The public key of slot @p index of an indexed field.
std::string slot(const char* key, int index) { return key + std::to_string(index); }

/// The field a gate is being built for.
struct GateSite {
  const std::string& key;
  /// The array index the walker put into the path; 0 at a plain member.
  int index;
  /// The patch the descriptors are taken from.
  const NativeSynthPatch& base;
};

// Percussion. A layer is reached only once something excites it, and every
// excitation the voice has is listed here, so "some layer sounds" is complete.

/// Mode slot @p i is placed: below numModes, with a non-zero ratio.
Gate mode_placed(int i) { return above("numModes", i) & above(slot("modeRatios", i), 0); }

/// Any placed mode whose nodal-diameter order passes @p keep.
template <typename Keep>
Gate modes_where(const GateSite& s, Keep keep) {
  Gate out = never();
  for (int i = 0; i < kMaxPercussionModes; ++i) {
    if (keep(static_cast<int>(s.base.percussion.mode_m[static_cast<size_t>(i)]))) {
      out = out | mode_placed(i);
    }
  }
  return out;
}

Gate tone(const GateSite& s) {
  return modes_where(s, [](int) { return true; });
}
Gate axisymmetric_modes(const GateSite& s) {
  return modes_where(s, [](int m) { return m == 0; });
}
Gate diametral_modes(const GateSite& s) {
  return modes_where(s, [](int m) { return m > 0; });
}
Gate noise(const GateSite&) { return above("noiseGain", 0); }
Gate particles(const GateSite&) { return above("phisemBeans", 0); }
Gate plate_on(const GateSite&) { return above("plateGain", 0); }
/// The plate rung by the stick's pulse alone.
Gate struck_plate(const GateSite& s) { return plate_on(s) & above("plateContact", 0); }
/// The plate with anything driving it: the dry hit or the stick's pulse.
Gate plate(const GateSite& s) {
  return plate_on(s) & (tone(s) | noise(s) | particles(s) | above("plateContact", 0));
}
/// What the shell resonates: the dry hit plus the plate's own ring.
Gate shell_input(const GateSite& s) { return tone(s) | noise(s) | particles(s) | struck_plate(s); }
/// Shell slot @p k resonating something.
Gate shell_slot(const GateSite& s, int k) {
  return above("shellMix", 0) & above("shellNumModes", k) & shell_input(s);
}

/// The strike shape reads alpha at a strike point; the air spring reads it as
/// the piston area of an axisymmetric pair, against every other such mode.
Gate mode_alpha(const GateSite& s) {
  const int i = s.index;
  Gate out = mode_placed(i) & above("strikeR", 0);
  const auto& orders = s.base.percussion.mode_m;
  if (orders[static_cast<size_t>(i)] != 0) return out;
  for (int j = 0; j < kMaxPercussionModes; ++j) {
    if (j == i || orders[static_cast<size_t>(j)] != 0) continue;
    out = out | (above("numModes", std::max(i, j)) & above(slot("modeRatios", i), 0) &
                 above(slot("modeRatios", j), 0) & above("airSpring", 0));
  }
  return out;
}

/// The damping law is a power of the ratio, so a mode at ratio 1 ignores it.
Gate mode_decay_exp(const GateSite&) {
  Gate out = never();
  for (int i = 0; i < kMaxPercussionModes; ++i) {
    out = out | (mode_placed(i) & differs(slot("modeRatios", i), 1));
  }
  return out;
}

/// The key-tracked base sets the modes and every shell slot left at 0 Hz.
Gate base_freq(const GateSite& s) {
  Gate out = tone(s);
  for (int k = 0; k < kMaxShellModes; ++k) {
    out = out | (shell_slot(s, k) & equals(slot("shellFreqHz", k), 0));
  }
  return out;
}

/// One gate an engine-section field takes, keyed by its un-indexed path. A
/// path ending in `*` covers its whole section; a field several rows match
/// takes all of them.
struct GateRow {
  const char* member_path;
  Gate (*gate)(const GateSite&);
};

const GateRow kEngineGates[] = {
    // Subtractive: the interval places an oscillator that is there only at a level.
    {"va.osc2_interval_cents", [](const GateSite&) { return above("osc2Level", 0); }},
    {"va.osc2_dry", [](const GateSite&) { return above("osc2Level", 0); }},
    // Only the square oscillator has a duty to set.
    {"va.pulse_width", [](const GateSite&) { return wrapper_is("waveform", "square"); }},
    // A single oscillator has no detune position to pan by.
    {"va.unison_width", [](const GateSite&) { return wrapper_above("unison", 1); }},
    // The band's corners shape noise that is there only at a level.
    {"va.band_noise_low_hz", [](const GateSite&) { return above("bandNoiseLevel", 0); }},
    {"va.band_noise_low_key_track", [](const GateSite&) { return above("bandNoiseLevel", 0); }},
    {"va.band_noise_high_hz", [](const GateSite&) { return above("bandNoiseLevel", 0); }},
    {"va.band_noise_high_key_track", [](const GateSite&) { return above("bandNoiseLevel", 0); }},
    // Pipe organ: the flat voicing builds the implicit 8' only.
    {"pipe_organ.stopped", [](const GateSite&) { return equals("rankCount", 0); }},
    {"pipe_organ.brightness", [](const GateSite&) { return equals("rankCount", 0); }},
    {"pipe_organ.reed", [](const GateSite&) { return equals("rankCount", 0); }},
    {"pipe_organ.radiation", [](const GateSite&) { return equals("rankCount", 0); }},
    {"pipe_organ.ranks.*", [](const GateSite& s) { return above("rankCount", s.index); }},
    // Bowed string: the bristle model (which the hyperbolic junction outranks),
    // the string the hyperbolic junction reads, and the violin corpus the
    // wrapper selects.
    {"bowed_string.elasto_plastic",
     [](const GateSite&) { return is("hyperbolicFriction", false); }},
    {"bowed_string.stribeck",
     [](const GateSite&) { return is("elastoPlastic", true) & is("hyperbolicFriction", false); }},
    {"bowed_string.string_tension_n",
     [](const GateSite&) { return is("hyperbolicFriction", true); }},
    {"bowed_string.string_length_m",
     [](const GateSite&) { return is("hyperbolicFriction", true); }},
    {"bowed_string.force_ratio", [](const GateSite&) { return is("hyperbolicFriction", true); }},
    {"bowed_string.section_rate_hz",
     [](const GateSite&) { return above("sectionSpreadCents", 0.0); }},
    // Vocal: the section stage over the formant bank, as the bowed string's.
    {"vocal.section_rate_hz", [](const GateSite&) { return above("sectionSpreadCents", 0.0); }},
    {"bowed_string.corpus_scale",
     [](const GateSite&) { return wrapper_is("body", "violin") & wrapper_above("bodyMix", 0); }},
    {"bowed_string.corpus_tilt_hz",
     [](const GateSite&) { return wrapper_is("body", "violin") & wrapper_above("bodyMix", 0); }},
    {"bowed_string.corpus_series",
     [](const GateSite&) { return wrapper_is("body", "violin") & wrapper_above("bodyMix", 0); }},
    {"bowed_string.string_loss_per_octave",
     [](const GateSite&) { return above("stringLossHarmonic", 0); }},
    // Reed: the mass-spring valve, the growth cone and the beating-reed channel.
    {"reed.reed_resonance", [](const GateSite&) { return is("dynamicReed", true); }},
    {"reed.cone_growth", [](const GateSite&) { return is("conical", true); }},
    {"reed.flow_gain", [](const GateSite&) { return above("closingPressure", 0); }},
    {"reed.pressure_scale", [](const GateSite&) { return above("closingPressure", 0); }},
    // Brass: a bell cutoff takes the radiation corner from the reflection pole.
    {"brass.bell_radiation_hz", [](const GateSite&) { return equals("bellCutoffHz", 0); }},
    // Brass, flute and free reed: the settle and the velocity slope shape an overshoot
    // that has to be there.
    {"brass.onset_breath_settle_ms",
     [](const GateSite&) { return above("onsetBreathOvershootDb", 0); }},
    {"brass.onset_breath_overshoot_vel_db",
     [](const GateSite&) { return above("onsetBreathOvershootDb", 0); }},
    {"flute.onset_breath_settle_ms",
     [](const GateSite&) { return above("onsetBreathOvershootDb", 0); }},
    {"flute.onset_breath_overshoot_vel_db",
     [](const GateSite&) { return above("onsetBreathOvershootDb", 0); }},
    // Flute: the jet offset, the receptivity and the deflection limit shape the jet-drive source.
    {"flute.jet_offset", [](const GateSite&) { return is("jetDrive", true); }},
    {"flute.jet_receptivity_window", [](const GateSite&) { return is("jetDrive", true); }},
    {"flute.jet_cycle_mean", [](const GateSite&) { return is("jetDrive", true); }},
    // The servo belongs to the cubic source.
    {"flute.loop_phase_servo", [](const GateSite&) { return is("jetDrive", false); }},
    {"flute.jet_deflection_limit",
     [](const GateSite&) { return is("jetDrive", true) & is("jetCycleMean", true); }},
    {"free_reed.onset_breath_settle_ms",
     [](const GateSite&) { return above("onsetBreathOvershootDb", 0); }},
    {"free_reed.onset_breath_overshoot_vel_db",
     [](const GateSite&) { return above("onsetBreathOvershootDb", 0); }},
    {"free_reed.onset_breath_overshoot_key_db",
     [](const GateSite&) { return above("onsetBreathOvershootDb", 0); }},
    // Modal: a mallet contact time replaces the hardness curve and its two inputs.
    {"modal.strike_brightness", [](const GateSite&) { return equals("malletMs", 0); }},
    {"modal.vel_to_brightness", [](const GateSite&) { return equals("malletMs", 0); }},
    {"modal.mallet_vel_exp", [](const GateSite&) { return above("malletMs", 0); }},
    {"modal.contact_ms", [](const GateSite&) { return above("contact", 0); }},
    // A mode slot sounds only below the mode count.
    {"modal.modes.ratio", [](const GateSite& s) { return above("numModes", s.index); }},
    {"modal.modes.gain", [](const GateSite& s) { return above("numModes", s.index); }},
    {"modal.modes.decay_scale", [](const GateSite& s) { return above("numModes", s.index); }},
    // Karplus-Strong: the pick key track scales a pick position that has to be there, the hand
    // damps only the halo bank, and the air mode belongs to the guitar body the wrapper selects.
    {"ks.pick_position_per_octave", [](const GateSite&) { return above("pickPosition", 0); }},
    {"ks.halo_damp_s", [](const GateSite&) { return is("sympathetic", true); }},
    {"ks.body_air_hz",
     [](const GateSite&) { return wrapper_is("body", "guitar") & wrapper_above("bodyMix", 0); }},
    {"ks.body_air_t60_s",
     [](const GateSite&) { return wrapper_is("body", "guitar") & wrapper_above("bodyMix", 0); }},
    {"ks.body_knock",
     [](const GateSite&) { return wrapper_is("body", "guitar") & wrapper_above("bodyMix", 0); }},
    // Plucked string: the mid anchor is a second pole of the quoted loss solve, and the pick key
    // track scales a pick position that has to be there.
    {"plucked_string.mid_decay_s", [](const GateSite&) { return above("hfDecayS", 0); }},
    {"plucked_string.pick_position_per_octave",
     [](const GateSite&) { return above("pickPosition", 0); }},
    // Free reed: the flat tongue joins only a detuned musette.
    {"free_reed.musette_flat", [](const GateSite&) { return above("detune", 0); }},
    {"free_reed.musette_partner_level", [](const GateSite&) { return above("detune", 0); }},
    {"free_reed.musette_taper", [](const GateSite&) { return above("detune", 0); }},
    // Free reed: the slot flow replaces the shaped saw.
    {"free_reed.slot_return", [](const GateSite&) { return above("slotDuty", 0); }},
    {"free_reed.slot_gap", [](const GateSite&) { return above("slotDuty", 0); }},
    {"free_reed.radiation_keytrack",
     [](const GateSite&) { return above("slotDuty", 0) & above("radiation", 0); }},
    {"free_reed.slot_swing", [](const GateSite&) { return above("slotDuty", 0); }},
    {"free_reed.radiation", [](const GateSite&) { return above("slotDuty", 0); }},
    // Harpsichord: each choir's fields reach it only once it is drawn.
    {"harpsichord.pluck_8b", [](const GateSite&) { return is("eightB", true); }},
    {"harpsichord.unison_detune_cents", [](const GateSite&) { return is("eightB", true); }},
    {"harpsichord.pluck_4", [](const GateSite&) { return is("four", true); }},
    {"harpsichord.octave_detune_cents", [](const GateSite&) { return is("four", true); }},
    {"harpsichord.undamped_from_note", [](const GateSite&) { return is("four", true); }},
    // The ripple's length acts only with a depth, the second plane's tuning and
    // decay only with a coupling.
    {"harpsichord.ripple_ratio", [](const GateSite&) { return above("rippleLossShare", 0); }},
    {"harpsichord.pol_detune_cents", [](const GateSite&) { return above("polCoupling", 0); }},
    {"harpsichord.pol_t60_ratio", [](const GateSite&) { return above("polCoupling", 0); }},
    // Additive: the percussion harmonic.
    {"additive.percussion_decay_ms",
     [](const GateSite&) { return at_least("percussionHarmonic", 2); }},
    {"additive.percussion_level",
     [](const GateSite&) { return at_least("percussionHarmonic", 2); }},
    // Percussion: the kit switch resolves every note through the GM map.
    {"percussion.*",
     [](const GateSite& s) { return s.key == "gmKit" ? Gate{} : is("gmKit", false); }},
    // A choke fades the amplitude envelope, which the direct contact bypasses.
    {"percussion.exclusive_class", shell_input},
    {"percussion.num_modes",
     [](const GateSite&) {
       Gate out = never();
       for (int i = 0; i < kMaxPercussionModes; ++i) out = out | above(slot("modeRatios", i), 0);
       return out;
     }},
    {"percussion.mode_ratios", [](const GateSite& s) { return above("numModes", s.index); }},
    {"percussion.mode_alpha", mode_alpha},
    {"percussion.mode_decay_s", tone},
    {"percussion.mode_decay_exp", mode_decay_exp},
    {"percussion.tone_gain", tone},
    // The split is between the direct path and the plate.
    {"percussion.tone_direct", [](const GateSite& s) { return tone(s) & plate_on(s); }},
    {"percussion.base_freq_hz", base_freq},
    {"percussion.pitch_drop", tone},
    {"percussion.pitch_drop_ms", [](const GateSite& s) { return tone(s) & above("pitchDrop", 0); }},
    {"percussion.strike_r", tone},
    // Mode order 0 is axisymmetric, so the angle needs a mode with a diameter.
    {"percussion.strike_theta",
     [](const GateSite& s) { return diametral_modes(s) & above("strikeR", 0); }},
    {"percussion.mallet_ms", tone},
    {"percussion.mallet_vel_exp", [](const GateSite& s) { return tone(s) & above("malletMs", 0); }},
    {"percussion.air_spring", axisymmetric_modes},
    {"percussion.head_diameter_m", diametral_modes},
    {"percussion.shell_depth_m",
     [](const GateSite& s) { return axisymmetric_modes(s) & above("airSpring", 0); }},
    {"percussion.noise_decay_ms", noise},
    {"percussion.noise_cutoff_hz", noise},
    {"percussion.noise_q", noise},
    {"percussion.noise_output", noise},
    {"percussion.noise_burst_count", noise},
    {"percussion.noise_burst_interval_ms",
     [](const GateSite& s) { return noise(s) & above("noiseBurstCount", 0); }},
    {"percussion.noise_burst_decay_ms",
     [](const GateSite& s) { return noise(s) & above("noiseBurstCount", 0); }},
    // The ceiling bounds the burst, the wire rattle and the shimmer wash.
    {"percussion.noise_air_hz",
     [](const GateSite& s) {
       return noise(s) | (tone(s) & (above("wireBuzz", 0) | above("shimmer", 0)));
     }},
    {"percussion.shell_mix",
     [](const GateSite& s) { return above("shellNumModes", 0) & shell_input(s); }},
    {"percussion.shell_num_modes",
     [](const GateSite& s) { return above("shellMix", 0) & shell_input(s); }},
    {"percussion.shell_freq_hz", [](const GateSite& s) { return shell_slot(s, s.index); }},
    {"percussion.shell_t60_s", [](const GateSite& s) { return shell_slot(s, s.index); }},
    {"percussion.shell_weight", [](const GateSite& s) { return shell_slot(s, s.index); }},
    {"percussion.wire_buzz", tone},
    {"percussion.wire_threshold", [](const GateSite& s) { return tone(s) & above("wireBuzz", 0); }},
    {"percussion.wire_cutoff_hz", [](const GateSite& s) { return tone(s) & above("wireBuzz", 0); }},
    {"percussion.wire_decay_ms", [](const GateSite& s) { return tone(s) & above("wireBuzz", 0); }},
    {"percussion.shimmer", tone},
    {"percussion.shimmer_attack_ms",
     [](const GateSite& s) { return tone(s) & above("shimmer", 0); }},
    {"percussion.shimmer_cutoff_hz",
     [](const GateSite& s) { return tone(s) & above("shimmer", 0); }},
    // One pulse length times the direct contact and the plate's.
    {"percussion.contact_ms",
     [](const GateSite& s) { return above("contact", 0) | struck_plate(s); }},
    {"percussion.plate_gain",
     [](const GateSite& s) {
       return tone(s) | noise(s) | particles(s) | above("plateContact", 0);
     }},
    {"percussion.plate_t60_s", plate},
    {"percussion.plate_hf_ratio", plate},
    {"percussion.plate_low_hz", plate},
    {"percussion.plate_air_hz", plate},
    {"percussion.plate_floor_hz", plate},
    {"percussion.plate_cascade", plate},
    {"percussion.plate_contact", plate_on},
    {"percussion.plate_cascade_hz",
     [](const GateSite& s) { return plate(s) & above("plateCascade", 0); }},
    {"percussion.plate_cascade_drop_db",
     [](const GateSite& s) { return plate(s) & above("plateCascade", 0); }},
    {"percussion.phisem_energy_ms", particles},
    {"percussion.phisem_sound_ms", particles},
    {"percussion.phisem_res_hz", particles},
    {"percussion.phisem_scrape_hz", particles},
    {"percussion.phisem_res_q",
     [](const GateSite& s) { return particles(s) & above("phisemResHz", 0); }},
    {"percussion.phisem_pitch_glide",
     [](const GateSite& s) { return particles(s) & above("phisemResHz", 0); }},
    {"percussion.phisem_body_hz",
     [](const GateSite& s) { return particles(s) & above("phisemBodyGain", 0); }},
    {"percussion.phisem_body_q",
     [](const GateSite& s) {
       return particles(s) & above("phisemBodyHz", 0) & above("phisemBodyGain", 0);
     }},
    {"percussion.phisem_body_gain",
     [](const GateSite& s) { return particles(s) & above("phisemBodyHz", 0); }},
};

bool gate_row_matches(const char* row_path, const std::string& member_path) {
  const size_t n = std::strlen(row_path);
  if (n > 0 && row_path[n - 1] == '*') return member_path.compare(0, n - 1, row_path, n - 1) == 0;
  return member_path == row_path;
}

}  // namespace

namespace patch_tuning_detail {

std::vector<ActiveAlternative> engine_field_gates(const std::string& member_path, int index,
                                                  const std::string& own_key,
                                                  const NativeSynthPatch& base) {
  const GateSite s{own_key, index, base};
  Gate gate;
  for (const GateRow& row : kEngineGates) {
    if (gate_row_matches(row.member_path, member_path)) gate = gate & row.gate(s);
  }
  if (gate.any.size() == 1 && gate.any.front().empty()) return {};
  return std::move(gate.any);
}

}  // namespace patch_tuning_detail

}  // namespace sonare::midi::synth
