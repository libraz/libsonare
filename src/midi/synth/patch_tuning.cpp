#include "midi/synth/patch_tuning.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "midi/synth/engine_field_gates.h"
#include "midi/synth/engine_field_units.h"
#include "midi/synth/native_synth.h"
#include "util/tunable.h"

// The field table, the walker, the bounds probe and the key mapper compile in
// every build, because the public engine-param access reads them. Only the
// environment override source and the catalogue dump are tuning-only, and with
// nothing given the walk changes no field, so no shipped render depends on it.

namespace sonare::midi::synth {

namespace {

using mastering::api::detail::Unit;

/// What a walk over the field table does with each field it visits.
enum class FieldPass {
  kApply,   ///< tuning build: replace the value with its override, if one is set
  kFill,    ///< write a probe value into every field, to be clamped afterwards
  kRead,    ///< record each field's path, type and value, leaving the value alone
  kAssign,  ///< set each field a caller named by public key
};

/// One field as the read pass records it.
struct FieldSite {
  std::string path;
  /// The path with the walker-inserted index removed; equal to @ref path at a
  /// plain member.
  std::string member_path;
  bool indexed = false;
  bool integer = false;
  /// Representable range of an integer field (its type, or a narrower one the
  /// walker states).
  int type_lo = 0;
  int type_hi = 0;
  float value = 0.0f;
};

/// The magnitude written into every field before clamping. Finite (so
/// `clamp_synth_patch`'s non-finite fallback does not fire and substitute a
/// default) and far outside every interval the engines accept, so what comes
/// back is the clamp bound itself. A field that comes back unchanged is one
/// `clamp_synth_patch` does not bound at all.
constexpr float kBoundProbe = 1e30f;

/// The probe an integer field is filled with. The float probe is 1e30, which
/// no integer type can hold, so the sign is taken from it and the magnitude
/// from what an `int` can represent. `clamp_synth_patch` narrows it to the
/// real bound exactly as it does for a float, so the bound stays measured
/// rather than mirrored from the clamp.
constexpr float kIntBoundProbe = static_cast<float>(std::numeric_limits<int>::max() / 2);

/// A field's range as the probe measured it; an open side is one the clamp
/// left at the probe value.
struct ProbedBound {
  std::string path;
  float lo = 0.0f;
  float hi = 0.0f;
  bool lo_open = true;
  bool hi_open = true;
};

/// Every field's probed range for one engine mode, sorted by path.
using ModeBounds = std::vector<ProbedBound>;

const ProbedBound* find_bound(const ModeBounds& bounds, const char* path) {
  const auto it = std::lower_bound(
      bounds.begin(), bounds.end(), path,
      [](const ProbedBound& b, const char* p) { return std::strcmp(b.path.c_str(), p) < 0; });
  return it != bounds.end() && it->path == path ? &*it : nullptr;
}

/// The public key of an engine-section @p path: the first segment dropped,
/// the rest in lowerCamelCase.
std::string public_key(const std::string& path) {
  const size_t dot = path.find('.');
  std::string key;
  key.reserve(path.size());
  bool upper = false;
  for (size_t i = dot == std::string::npos ? 0 : dot + 1; i < path.size(); ++i) {
    const char c = path[i];
    if (c == '_') {
      upper = true;
      continue;
    }
    key += upper && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    upper = false;
  }
  return key;
}

/// A range end as a refusal message spells it: whole numbers exactly, the rest in `%g`.
std::string format_number(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), value == std::floor(value) ? "%.0f" : "%g", value);
  return buffer;
}

/// The state of one `apply_engine_params` walk: the caller's keys, which of
/// them a field took, and the first refusal.
struct Assignment {
  const EngineParam* params = nullptr;
  size_t count = 0;
  const ModeBounds* bounds = nullptr;
  std::vector<bool> used;
  std::string error;

  /// Whether a caller's key names @p path; on success @p out holds the value.
  bool take(const char* path, bool integer, int type_lo, int type_hi, double* out) {
    if (!error.empty()) return false;
    const std::string key = public_key(path);
    size_t j = 0;
    while (j < count && key != params[j].key) ++j;
    if (j == count) return false;
    used[j] = true;

    const double value = params[j].value;
    const ProbedBound* bound = find_bound(*bounds, path);
    bool has_lo = bound != nullptr && !bound->lo_open;
    bool has_hi = bound != nullptr && !bound->hi_open;
    double lo = has_lo ? bound->lo : 0.0;
    double hi = has_hi ? bound->hi : 0.0;
    // An integer field also refuses what its type cannot hold, open or not.
    if (integer && !has_lo) lo = type_lo, has_lo = true;
    if (integer && !has_hi) hi = type_hi, has_hi = true;
    // A float field is judged by the value it would hold, so a published bound read back
    // from its shortest decimal spelling lands on the bound rather than just past it.
    const bool held_as_float = !integer && std::fabs(value) <= std::numeric_limits<float>::max();
    const double held = held_as_float ? static_cast<double>(static_cast<float>(value)) : value;

    const char* reason = nullptr;
    if (!std::isfinite(value)) {
      reason = "not a finite number";
    } else if (integer && value != std::floor(value)) {
      reason = "not an integer";
    } else if ((has_lo && held < lo) || (has_hi && held > hi)) {
      reason = "out of range";
    } else if (!integer && std::fabs(value) > std::numeric_limits<float>::max()) {
      reason = "outside the 32-bit float range";
    }
    if (reason == nullptr) {
      *out = value;
      return true;
    }
    error = "engine param '" + key + "': " + reason;
    if (has_lo || has_hi) {
      error += " [" + (has_lo ? format_number(lo) : std::string("-inf")) + ", " +
               (has_hi ? format_number(hi) : std::string("inf")) + "]";
    }
    return false;
  }
};

/// One field: apply an override, fill a probe, read it, or assign it by key.
///
/// @p member, where given, is the path with its index removed: the walker
/// passes it at every site whose path it built from an array index, so an
/// index is never stripped from a member whose own name ends in a digit.
struct Fields {
  FieldPass pass = FieldPass::kRead;
  float fill = 0.0f;
  std::vector<FieldSite>* sites = nullptr;
  Assignment* assignment = nullptr;
  const std::string* prefix = nullptr;

  float operator()(const char* path, float current, const char* member = nullptr) const {
    switch (pass) {
      case FieldPass::kFill:
        return fill;
      case FieldPass::kRead:
        record(path, member, false, 0, 0, current);
        return current;
      case FieldPass::kAssign: {
        double value = 0.0;
        return assignment->take(path, false, 0, 0, &value) ? static_cast<float>(value) : current;
      }
      case FieldPass::kApply:
        break;
    }
#if defined(SONARE_TUNING) && SONARE_TUNING
    return ::sonare::tuning::tunable_keyed((*prefix + '.' + path).c_str(), current);
#else
    return current;
#endif
  }

  /// The same walk for a field that counts rather than measures.
  ///
  /// Every field above is a float, and for a while that was the whole table —
  /// which meant the questions a count answers could not be asked without a
  /// rebuild. That is worse than it sounds: a count is often the switch that
  /// decides whether the fields around it do anything at all, so sweeping
  /// `shell_freq_hz` and `shell_mix` while `shell_num_modes` sits at 0 reads
  /// as a clean structural negative — "the shell cannot reach this" — when the
  /// finding is only that the shell was off.
  ///
  /// The override arrives as a float like every other, and is rounded rather
  /// than truncated so a fitter stepping across 2.5 lands on 3 and not on 2.
  /// `lo`/`hi` are the field's REPRESENTABLE range, which for a plain `int`
  /// field is wide open and left to `clamp_synth_patch` to narrow — the same
  /// arrangement the float probe relies on. A narrow type states its own,
  /// because a probe that overflows it measures the wraparound.
  int as_int(const char* path, int current, int lo, int hi, const char* member = nullptr) const {
    switch (pass) {
      case FieldPass::kFill:
        return std::clamp(
            static_cast<int>(std::lround(fill < 0.0f ? -kIntBoundProbe : kIntBoundProbe)), lo, hi);
      case FieldPass::kRead:
        record(path, member, true, lo, hi, static_cast<float>(current));
        return current;
      case FieldPass::kAssign: {
        double value = 0.0;
        return assignment->take(path, true, lo, hi, &value) ? static_cast<int>(value) : current;
      }
      case FieldPass::kApply:
        break;
    }
#if defined(SONARE_TUNING) && SONARE_TUNING
    const float value = ::sonare::tuning::tunable_keyed((*prefix + '.' + path).c_str(),
                                                        static_cast<float>(current));
    if (!std::isfinite(value)) return current;
    return std::clamp(static_cast<int>(std::lround(value)), lo, hi);
#else
    return current;
#endif
  }

 private:
  void record(const char* path, const char* member, bool integer, int lo, int hi,
              float value) const {
    FieldSite site;
    site.path = path;
    site.member_path = member != nullptr ? member : path;
    site.indexed = member != nullptr;
    site.integer = integer;
    site.type_lo = lo;
    site.type_hi = hi;
    site.value = value;
    sites->push_back(std::move(site));
  }
};

/// `f(x)` rewrites `patch.x` from the key whose path is the same `x`, so the
/// table below reads as a list of member paths and nothing else.
#define F(path) p.path = f(#path, p.path)

/// An `int` field, bounded by `clamp_synth_patch` like every float here.
#define I(path) \
  p.path = f.as_int(#path, p.path, std::numeric_limits<int>::min(), std::numeric_limits<int>::max())

/// A field whose own type is the range — the clamp does not narrow it, and a
/// probe wide enough for an `int` would measure the type's wraparound instead.
#define I_TYPED(path, type_lo, type_hi)   \
  p.path = static_cast<decltype(p.path)>( \
      f.as_int(#path, static_cast<int>(p.path), (type_lo), (type_hi)))

/// A DAHDSR section (`amp_env`, `filter_env`, an FM operator's `env`). @p member
/// is the section's un-indexed path where @p path carries an index.
void apply_env(DahdsrConfig& e, const Fields& f, const std::string& path,
               const char* member = nullptr) {
  const auto at = [&](const char* leaf, float current) {
    const std::string leaf_member = member != nullptr ? std::string(member) + '.' + leaf : "";
    return f((path + '.' + leaf).c_str(), current,
             member != nullptr ? leaf_member.c_str() : nullptr);
  };
  e.delay_ms = at("delay_ms", e.delay_ms);
  e.attack_ms = at("attack_ms", e.attack_ms);
  e.hold_ms = at("hold_ms", e.hold_ms);
  e.decay_ms = at("decay_ms", e.decay_ms);
  e.sustain = at("sustain", e.sustain);
  e.release_ms = at("release_ms", e.release_ms);
}

/// The patch sections every engine shares: oscillator detune / drift, gain,
/// both envelopes, the filter, the LFOs, glide, body and stereo.
void apply_common(NativeSynthPatch& p, const Fields& f) {
  // The shared fields that switch a mechanism rather than trim one, and whose
  // absence made a sweep of `detune_cents`, `body_mix`, `cutoff_hz` or a
  // release read as a structural answer.
  I(unison);
  I_TYPED(body, static_cast<int>(BodyType::kNone), static_cast<int>(BodyType::kVocal));
  I_TYPED(one_shot, 0, 1);
  I_TYPED(lfo_shape, static_cast<int>(LfoShape::kTriangle), static_cast<int>(LfoShape::kRandom));
  I_TYPED(filter_output, static_cast<int>(SynthFilterOutput::kLowpass),
          static_cast<int>(SynthFilterOutput::kHighpass));
  I_TYPED(filter_model, static_cast<int>(SynthFilterModel::kSvf),
          static_cast<int>(SynthFilterModel::kSvf4Pole));
  I_TYPED(waveform, static_cast<int>(VaWaveform::kSine), static_cast<int>(VaWaveform::kNoise));
  F(detune_cents);
  F(drift_cents);
  F(drift_rate_hz);
  F(pitch_offset_cents);
  F(gain);
  F(cutoff_hz);
  F(hp_cutoff_hz);
  F(sample_hold_hz);
  F(bit_depth);
  F(resonance_q);
  F(drive);
  F(env_to_cutoff_cents);
  F(key_track);
  F(vel_to_cutoff_cents);
  F(lfo_rate_hz);
  F(lfo_to_pitch_cents);
  F(lfo2_rate_hz);
  F(glide_ms);
  F(body_mix);
  F(stereo_spread);
  apply_env(p.amp_env, f, "amp_env");
  apply_env(p.filter_env, f, "filter_env");
}

void apply_piano(NativeSynthPatch& p, const Fields& f) {
  // The choir size decides what `piano.detune_cents` has to detune.
  I(piano.strings);
  F(piano.detune_cents);
  F(piano.decay_fast_s);
  F(piano.decay_slow_s);
  F(piano.decay_stretch);
  F(piano.brightness);
  F(piano.dispersion);
  F(piano.strike_position);
  F(piano.hammer_exponent);
  F(piano.hammer_contact_ms);
  F(piano.hammer_dynamics);
  F(piano.attack_hf_dynamics);
  F(piano.soundboard);
  F(piano.release_damp_s);
}

void apply_pipe_organ(NativeSynthPatch& p, const Fields& f) {
  // The registration comes first because it decides which of the fields under
  // it are read at all: >0 sounds `ranks[0..rank_count)` and leaves the four
  // flat voicing fields unread, 0 sounds one implicit 8' built from them.
  I(pipe_organ.rank_count);
  I_TYPED(pipe_organ.stopped, 0, 1);
  F(pipe_organ.brightness);
  F(pipe_organ.tone_decay_s);
  F(pipe_organ.breath);
  F(pipe_organ.chiff);
  F(pipe_organ.chiff_ms);
  F(pipe_organ.release_damp_s);
  F(pipe_organ.reed);
  F(pipe_organ.radiation);
  F(pipe_organ.keytrack);
  F(pipe_organ.pipe_detune_cents);
  I_TYPED(pipe_organ.loop_phase_servo, 0, 1);
  F(pipe_organ.tremulant_rate_hz);
  F(pipe_organ.tremulant_depth);
  F(pipe_organ.wind_sag);
  I_TYPED(pipe_organ.wind_regulated, 0, 1);
  F(pipe_organ.swell);
  for (int i = 0; i < kMaxPipeRanks; ++i) {
    PipeOrganRank& r = p.pipe_organ.ranks[static_cast<size_t>(i)];
    const std::string base = "pipe_organ.ranks" + std::to_string(i) + '.';
    r.footage_mult =
        f((base + "footage_mult").c_str(), r.footage_mult, "pipe_organ.ranks.footage_mult");
    r.stopped = f.as_int((base + "stopped").c_str(), r.stopped ? 1 : 0, 0, 1,
                         "pipe_organ.ranks.stopped") != 0;
    r.brightness = f((base + "brightness").c_str(), r.brightness, "pipe_organ.ranks.brightness");
    r.level = f((base + "level").c_str(), r.level, "pipe_organ.ranks.level");
    r.reed = f((base + "reed").c_str(), r.reed, "pipe_organ.ranks.reed");
    r.radiation = f((base + "radiation").c_str(), r.radiation, "pipe_organ.ranks.radiation");
  }
}

void apply_bowed_string(NativeSynthPatch& p, const Fields& f) {
  // The friction models: off is the memoryless table, `stribeck` shapes the
  // bristle memory only when the elasto-plastic one is on, and the hyperbolic
  // junction reads the string's tension and length and the force ratio only
  // when it is on.
  I_TYPED(bowed_string.elasto_plastic, 0, 1);
  I_TYPED(bowed_string.hyperbolic_friction, 0, 1);
  F(bowed_string.string_tension_n);
  F(bowed_string.string_length_m);
  F(bowed_string.bow_width_m);
  F(bowed_string.force_ratio);
  F(bowed_string.bow_position);
  F(bowed_string.bow_position_ff);
  F(bowed_string.bow_position_spread);
  F(bowed_string.velocity_exponent);
  F(bowed_string.bow_force);
  F(bowed_string.bow_speed);
  F(bowed_string.vel_to_speed);
  F(bowed_string.brightness);
  F(bowed_string.damping);
  F(bowed_string.attack_ms);
  F(bowed_string.release_ms);
  F(bowed_string.rosin);
  F(bowed_string.stribeck);
  F(bowed_string.sympathetic);
  F(bowed_string.polarization);
  F(bowed_string.section_spread_cents);
  F(bowed_string.section_rate_hz);
  F(bowed_string.attack_noise);
  F(bowed_string.bow_accel_ms);
  F(bowed_string.corpus_scale);
  F(bowed_string.corpus_tilt_hz);
  I_TYPED(bowed_string.corpus_series, static_cast<int>(BowedCorpus::kNone),
          static_cast<int>(BowedCorpus::kContrabass));
  F(bowed_string.string_loss_harmonic);
  F(bowed_string.string_loss_per_octave);
}

void apply_reed(NativeSynthPatch& p, const Fields& f) {
  // The bore shape decides which harmonics exist at all, and the dynamic reed
  // is what `closing_pressure` biases; off, it is the memoryless table.
  I_TYPED(reed.conical, 0, 1);
  I_TYPED(reed.dynamic_reed, 0, 1);
  F(reed.breath_pressure);
  F(reed.vel_to_breath);
  F(reed.reed_stiffness);
  F(reed.reed_opening);
  F(reed.brightness);
  F(reed.damping);
  F(reed.bell_radiation_hz);
  F(reed.attack_ms);
  F(reed.release_ms);
  F(reed.breath_noise);
  F(reed.chiff);
  F(reed.chiff_ms);
  F(reed.reed_resonance);
  F(reed.register_vent);
  F(reed.growl);
  F(reed.cone_growth);
  F(reed.tonehole);
  F(reed.closing_pressure);
  F(reed.flow_gain);
  F(reed.pressure_scale);
  F(reed.velocity_exponent);
}

void apply_brass(NativeSynthPatch& p, const Fields& f) {
  // The body shape biases the bell reflection every brightness knob works into.
  I_TYPED(brass.conical, 0, 1);
  F(brass.breath_pressure);
  F(brass.vel_to_breath);
  F(brass.lip_tension);
  F(brass.lip_damping);
  F(brass.brightness);
  F(brass.damping);
  F(brass.attack_ms);
  F(brass.release_ms);
  F(brass.breath_noise);
  F(brass.chiff);
  F(brass.chiff_ms);
  F(brass.lip_aperture);
  F(brass.bell_radiation_hz);
  F(brass.bell_cutoff_hz);
  F(brass.brassiness);
  F(brass.cuivre_dynamics);
  F(brass.bore_nonlinearity);
  F(brass.mute);
  F(brass.half_valve);
  F(brass.dynamic_lip);
  F(brass.onset_breath_overshoot_db);
  F(brass.onset_breath_settle_ms);
  F(brass.onset_breath_overshoot_vel_db);
  F(brass.velocity_exponent);
}

void apply_flute(NativeSynthPatch& p, const Fields& f) {
  F(flute.breath_pressure);
  F(flute.vel_to_breath);
  F(flute.jet_ratio);
  F(flute.jet_reflection);
  F(flute.end_reflection);
  F(flute.brightness);
  F(flute.damping);
  F(flute.attack_ms);
  F(flute.release_ms);
  F(flute.breath_noise);
  F(flute.chiff);
  F(flute.chiff_ms);
  F(flute.vibrato_rate_hz);
  F(flute.vibrato_depth);
  F(flute.overblow);
  F(flute.jet_turbulence);
  F(flute.edge_hysteresis);
  F(flute.vortex);
  F(flute.vortex_loss);
  I_TYPED(flute.jet_drive, 0, 1);
  F(flute.jet_offset);
  F(flute.lattice_cutoff_hz);
  F(flute.jet_centring_per_octave);
  F(flute.jet_receptivity_window);
  I_TYPED(flute.jet_cycle_mean, 0, 1);
  I_TYPED(flute.loop_phase_servo, 0, 1);
  F(flute.jet_deflection_limit);
  F(flute.breath_radiation);
  F(flute.onset_breath_overshoot_db);
  F(flute.onset_breath_settle_ms);
  F(flute.onset_breath_overshoot_vel_db);
  F(flute.velocity_exponent);
}

void apply_ks(NativeSynthPatch& p, const Fields& f) {
  F(ks.brightness);
  F(ks.decay_s);
  F(ks.decay_stretch);
  F(ks.pick_position);
  F(ks.pick_position_per_octave);
  F(ks.exc_brightness);
  F(ks.exc_brightness_per_octave);
  F(ks.vel_to_brightness);
  F(ks.release_damp_s);
  F(ks.mute_harmonic);
  F(ks.slap);
  F(ks.polarization);
  F(ks.body_coupling);
  F(ks.pluck_style);
  F(ks.nail);
  I_TYPED(ks.sympathetic, 0, 1);
  F(ks.halo_damp_s);
  F(ks.pickup_pos);
  F(ks.dispersion);
  F(ks.tension_mod);
  F(ks.octave_mix);
  F(ks.harmonic_node);
  F(ks.keyoff_noise);
  F(ks.pick_noise);
  F(ks.body_air_hz);
  F(ks.body_air_t60_s);
  F(ks.body_knock);
  F(ks.hf_decay_s);
  F(ks.mid_decay_s);
  F(ks.velocity_exponent);
}

void apply_plucked_string(NativeSynthPatch& p, const Fields& f) {
  F(plucked_string.brightness);
  F(plucked_string.decay_s);
  F(plucked_string.decay_stretch);
  F(plucked_string.pick_position);
  F(plucked_string.pick_position_per_octave);
  F(plucked_string.exc_brightness);
  F(plucked_string.exc_brightness_per_octave);
  F(plucked_string.vel_to_brightness);
  F(plucked_string.release_damp_s);
  F(plucked_string.hf_decay_s);
  F(plucked_string.mid_decay_s);
  F(plucked_string.inharmonicity_b);
  F(plucked_string.pick_noise);
  F(plucked_string.buzz);
  F(plucked_string.velocity_exponent);
}

void apply_harpsichord(NativeSynthPatch& p, const Fields& f) {
  // The registration and the key's reach come first because each decides
  // whether the fields under it are read: a choir that is not drawn ignores its
  // own pluck point and detune, and `velocity_droop_db` shapes the response
  // only past `peak_velocity`, which at 127 there is no room beyond.
  I_TYPED(harpsichord.eight_a, 0, 1);
  I_TYPED(harpsichord.eight_b, 0, 1);
  I_TYPED(harpsichord.four, 0, 1);
  I_TYPED(harpsichord.peak_velocity, 0, 127);
  I_TYPED(harpsichord.undamped_from_note, 0, 128);
  F(harpsichord.pluck_8a);
  F(harpsichord.pluck_8b);
  F(harpsichord.pluck_4);
  F(harpsichord.plectrum_edge);
  F(harpsichord.end_reflection);
  F(harpsichord.velocity_range_db);
  F(harpsichord.velocity_droop_db);
  F(harpsichord.decay_s);
  F(harpsichord.decay_stretch);
  F(harpsichord.hf_damping);
  F(harpsichord.damping_ref_hz);
  F(harpsichord.unison_detune_cents);
  F(harpsichord.octave_detune_cents);
  F(harpsichord.ripple_loss_share);
  F(harpsichord.ripple_ratio);
  F(harpsichord.pol_coupling);
  F(harpsichord.pol_detune_cents);
  F(harpsichord.pol_t60_ratio);
  F(harpsichord.rear_segment_mm);
  F(harpsichord.rear_coupling);
  F(harpsichord.rear_decay_s);
  F(harpsichord.scale_c5_mm);
  F(harpsichord.bass_foreshortening);
  F(harpsichord.pluck_noise);
  F(harpsichord.jack_noise);
  F(harpsichord.damper_s);
  F(harpsichord.board_radiating_from_hz);
  F(harpsichord.board_tilt_db_oct);
  F(harpsichord.board_diffuse_db);
}

void apply_free_reed(NativeSynthPatch& p, const Fields& f) {
  F(free_reed.brightness);
  F(free_reed.reed_stiffness);
  F(free_reed.breath_pressure);
  F(free_reed.vel_to_breath);
  F(free_reed.detune);
  I_TYPED(free_reed.musette_flat, 0, 1);
  F(free_reed.musette_partner_level);
  F(free_reed.musette_taper);
  F(free_reed.attack_ms);
  F(free_reed.release_ms);
  F(free_reed.breath_noise);
  F(free_reed.slot_duty);
  F(free_reed.slot_return);
  F(free_reed.slot_gap);
  F(free_reed.slot_swing);
  F(free_reed.radiation);
  F(free_reed.radiation_keytrack);
  F(free_reed.onset_breath_overshoot_db);
  F(free_reed.onset_breath_settle_ms);
  F(free_reed.onset_breath_overshoot_vel_db);
  F(free_reed.onset_breath_overshoot_key_db);
  F(free_reed.velocity_exponent);
}

void apply_vocal(NativeSynthPatch& p, const Fields& f) {
  // Which formant table the whole voice is read out of.
  I(vocal.vowel);
  F(vocal.brightness);
  F(vocal.breath_noise);
  F(vocal.vibrato_rate_hz);
  F(vocal.vibrato_depth);
  F(vocal.attack_ms);
  F(vocal.release_ms);
  F(vocal.velocity_exponent);
  F(vocal.section_spread_cents);
  F(vocal.section_rate_hz);
}

void apply_modal(NativeSynthPatch& p, const Fields& f) {
  I(modal.num_modes);
  F(modal.decay_s);
  F(modal.decay_stretch);
  F(modal.strike_brightness);
  F(modal.vel_to_brightness);
  F(modal.release_damp_s);
  F(modal.mallet_ms);
  F(modal.mallet_vel_exp);
  F(modal.contact);
  F(modal.contact_ms);
  F(modal.velocity_exponent);
  for (int i = 0; i < kMaxModalModes; ++i) {
    ModalMode& m = p.modal.modes[static_cast<size_t>(i)];
    const std::string base = "modal.modes" + std::to_string(i) + '.';
    m.ratio = f((base + "ratio").c_str(), m.ratio, "modal.modes.ratio");
    m.gain = f((base + "gain").c_str(), m.gain, "modal.modes.gain");
    m.decay_scale = f((base + "decay_scale").c_str(), m.decay_scale, "modal.modes.decay_scale");
  }
}

void apply_additive(NativeSynthPatch& p, const Fields& f) {
  F(additive.key_click);
  F(additive.click_decay_ms);
  // The harmonic comes first because it decides whether the two under it do
  // anything: 0 is percussion switched off, and sweeping its decay there reads
  // as "the percussion cannot reach this measurement".
  I(additive.percussion_harmonic);
  F(additive.percussion_decay_ms);
  F(additive.percussion_level);
  // The morph comes first because it decides whether the second registration
  // does anything: sweeping `drawbars_b` at morph 0 reads as "the second
  // registration cannot reach this measurement" and means it is not drawn on.
  F(additive.morph);
  for (int i = 0; i < kAdditivePartials; ++i) {
    float& d = p.additive.drawbars[static_cast<size_t>(i)];
    d = f(("additive.drawbars" + std::to_string(i)).c_str(), d, "additive.drawbars");
    // `drawbars_b<i>`, not `drawbars2` — that key is already drawbars[2].
    float& b = p.additive.drawbars_b[static_cast<size_t>(i)];
    b = f(("additive.drawbars_b" + std::to_string(i)).c_str(), b, "additive.drawbars_b");
  }
}

void apply_percussion(NativeSynthPatch& p, const Fields& f) {
  // The counts and the kit switch come first because they decide whether the
  // fields under them do anything: sweeping the shell's frequencies with
  // `shell_num_modes` at 0 reads as "the shell cannot reach this measurement"
  // and means "the shell is switched off", and `gm_kit` ignores the rest of the
  // section outright — note-on resolves the struck note through the GM map.
  I_TYPED(percussion.gm_kit, 0, 1);
  I(percussion.num_modes);
  I(percussion.shell_num_modes);
  // Neither is narrowed by `clamp_synth_patch`, so each states the range its
  // own type defines. `exclusive_class` is the GM mute group — 0 is "none",
  // and it is here so a choke relationship can be tested without a rebuild.
  I_TYPED(percussion.exclusive_class, 0, 255);
  // Upper bound is a working range rather than a limit the code enforces: past
  // a handful the train stops reading as one gesture.
  I_TYPED(percussion.noise_burst_count, 0, 8);
  I_TYPED(percussion.noise_output, static_cast<int>(SynthFilterOutput::kLowpass),
          static_cast<int>(SynthFilterOutput::kHighpass));
  for (int i = 0; i < kMaxPercussionModes; ++i) {
    const std::string index = std::to_string(i);
    float& ratio = p.percussion.mode_ratios[static_cast<size_t>(i)];
    ratio = f(("percussion.mode_ratios" + index).c_str(), ratio, "percussion.mode_ratios");
    float& alpha = p.percussion.mode_alpha[static_cast<size_t>(i)];
    alpha = f(("percussion.mode_alpha" + index).c_str(), alpha, "percussion.mode_alpha");
  }
  F(percussion.mode_decay_s);
  F(percussion.mode_decay_exp);
  F(percussion.tone_gain);
  F(percussion.tone_direct);
  F(percussion.base_freq_hz);
  F(percussion.pitch_drop);
  F(percussion.pitch_drop_ms);
  F(percussion.strike_r);
  F(percussion.strike_theta);
  F(percussion.mallet_ms);
  F(percussion.mallet_vel_exp);
  F(percussion.air_spring);
  F(percussion.head_diameter_m);
  F(percussion.shell_depth_m);
  F(percussion.noise_gain);
  F(percussion.noise_decay_ms);
  F(percussion.noise_cutoff_hz);
  F(percussion.noise_q);
  F(percussion.noise_burst_interval_ms);
  F(percussion.noise_burst_decay_ms);
  F(percussion.shell_mix);
  for (int i = 0; i < kMaxShellModes; ++i) {
    const std::string index = std::to_string(i);
    float& freq = p.percussion.shell_freq_hz[static_cast<size_t>(i)];
    freq = f(("percussion.shell_freq_hz" + index).c_str(), freq, "percussion.shell_freq_hz");
    float& t60 = p.percussion.shell_t60_s[static_cast<size_t>(i)];
    t60 = f(("percussion.shell_t60_s" + index).c_str(), t60, "percussion.shell_t60_s");
    float& weight = p.percussion.shell_weight[static_cast<size_t>(i)];
    weight = f(("percussion.shell_weight" + index).c_str(), weight, "percussion.shell_weight");
  }
  F(percussion.noise_air_hz);
  F(percussion.wire_buzz);
  F(percussion.wire_threshold);
  F(percussion.wire_cutoff_hz);
  F(percussion.wire_decay_ms);
  F(percussion.shimmer);
  F(percussion.shimmer_attack_ms);
  F(percussion.shimmer_cutoff_hz);
  F(percussion.contact);
  F(percussion.contact_ms);
  F(percussion.plate_gain);
  F(percussion.plate_t60_s);
  F(percussion.plate_hf_ratio);
  F(percussion.plate_low_hz);
  F(percussion.plate_air_hz);
  F(percussion.plate_contact);
  F(percussion.plate_cascade);
  F(percussion.plate_cascade_hz);
  F(percussion.plate_cascade_drop_db);
  F(percussion.plate_floor_hz);
  F(percussion.phisem_beans);
  F(percussion.phisem_energy_ms);
  F(percussion.phisem_sound_ms);
  F(percussion.phisem_res_hz);
  F(percussion.phisem_res_q);
  F(percussion.phisem_body_hz);
  F(percussion.phisem_body_q);
  F(percussion.phisem_body_gain);
  F(percussion.phisem_scrape_hz);
  F(percussion.phisem_pitch_glide);
}

void apply_va(NativeSynthPatch& p, const Fields& f) {
  // The level first: at 0 the interval moves nothing.
  F(va.osc2_level);
  F(va.osc2_dry);
  F(va.osc2_interval_cents);
  F(va.noise_level);
  F(va.noise_am);
  // The level first: at 0 the band's corners move nothing.
  F(va.band_noise_level);
  F(va.band_noise_low_hz);
  F(va.band_noise_low_key_track);
  F(va.band_noise_high_hz);
  F(va.band_noise_high_key_track);
  F(va.velocity_exponent);
  F(va.pulse_width);
  F(va.unison_width);
}

void apply_fm(NativeSynthPatch& p, const Fields& f) {
  I_TYPED(fm.algorithm, static_cast<int>(FmAlgorithm::kStack2),
          static_cast<int>(FmAlgorithm::kAdd2));
  F(fm.velocity_exponent);
  for (int i = 0; i < kMaxFmOperators; ++i) {
    FmOperatorParams& op = p.fm.ops[static_cast<size_t>(i)];
    const std::string base = "fm.ops" + std::to_string(i);
    op.ratio = f((base + ".ratio").c_str(), op.ratio, "fm.ops.ratio");
    op.detune_cents = f((base + ".detune_cents").c_str(), op.detune_cents, "fm.ops.detune_cents");
    op.level = f((base + ".level").c_str(), op.level, "fm.ops.level");
    op.vel_to_level = f((base + ".vel_to_level").c_str(), op.vel_to_level, "fm.ops.vel_to_level");
    op.key_rate_scale =
        f((base + ".key_rate_scale").c_str(), op.key_rate_scale, "fm.ops.key_rate_scale");
    op.feedback = f((base + ".feedback").c_str(), op.feedback, "fm.ops.feedback");
    apply_env(op.env, f, base + ".env", "fm.ops.env");
  }
}

#undef F

/// Walk the selected engine's own section, doing whatever `f`'s pass says.
void walk_engine(NativeSynthPatch& p, const Fields& f) {
  switch (p.mode) {
    case SynthEngineMode::kPiano:
      apply_piano(p, f);
      break;
    case SynthEngineMode::kPipeOrgan:
      apply_pipe_organ(p, f);
      break;
    case SynthEngineMode::kBowedString:
      apply_bowed_string(p, f);
      break;
    case SynthEngineMode::kReed:
      apply_reed(p, f);
      break;
    case SynthEngineMode::kBrass:
      apply_brass(p, f);
      break;
    case SynthEngineMode::kFlute:
      apply_flute(p, f);
      break;
    case SynthEngineMode::kKarplusStrong:
      apply_ks(p, f);
      break;
    case SynthEngineMode::kPluckedString:
      apply_plucked_string(p, f);
      break;
    case SynthEngineMode::kFreeReed:
      apply_free_reed(p, f);
      break;
    case SynthEngineMode::kHarpsichord:
      apply_harpsichord(p, f);
      break;
    case SynthEngineMode::kVocal:
      apply_vocal(p, f);
      break;
    case SynthEngineMode::kModal:
      apply_modal(p, f);
      break;
    case SynthEngineMode::kAdditive:
      apply_additive(p, f);
      break;
    case SynthEngineMode::kPercussion:
      apply_percussion(p, f);
      break;
    case SynthEngineMode::kFm:
      apply_fm(p, f);
      break;
    case SynthEngineMode::kSubtractive:
      apply_va(p, f);
      break;
    // The sample engine's fields address host data rather than model
    // behaviour, so there is nothing here a reference could fit.
    case SynthEngineMode::kSample:
      break;
  }
}

/// Walk every field this patch's engine exposes: the common section, then the
/// engine's own.
void walk_fields(NativeSynthPatch& p, const Fields& f) {
  apply_common(p, f);
  walk_engine(p, f);
}

/// The engine's name for the catalogue. Written here rather than in `tunable.h`
/// so the util layer stays free of the synth enum, and as names rather than enum
/// values so a renumbering cannot silently relabel a voice.
const char* mode_name(SynthEngineMode mode) {
  switch (mode) {
    case SynthEngineMode::kSubtractive:
      return "subtractive";
    case SynthEngineMode::kFm:
      return "fm";
    case SynthEngineMode::kKarplusStrong:
      return "karplus_strong";
    case SynthEngineMode::kModal:
      return "modal";
    case SynthEngineMode::kAdditive:
      return "additive";
    case SynthEngineMode::kPercussion:
      return "percussion";
    case SynthEngineMode::kPiano:
      return "piano";
    case SynthEngineMode::kPipeOrgan:
      return "pipe_organ";
    case SynthEngineMode::kBowedString:
      return "bowed_string";
    case SynthEngineMode::kReed:
      return "reed";
    case SynthEngineMode::kBrass:
      return "brass";
    case SynthEngineMode::kFlute:
      return "flute";
    case SynthEngineMode::kPluckedString:
      return "plucked_string";
    case SynthEngineMode::kVocal:
      return "vocal";
    case SynthEngineMode::kFreeReed:
      return "free_reed";
    case SynthEngineMode::kHarpsichord:
      return "harpsichord";
    case SynthEngineMode::kSample:
      return "sample";
  }
  return "subtractive";
}

using patch_tuning_detail::find_unit;

/// The array index the walker put into @p site's path; 0 at a plain member.
int site_index(const FieldSite& site) {
  if (!site.indexed) return 0;
  size_t at = 0;
  while (at < site.member_path.size() && site.path[at] == site.member_path[at]) ++at;
  int index = 0;
  while (at < site.path.size() && site.path[at] >= '0' && site.path[at] <= '9') {
    index = index * 10 + (site.path[at++] - '0');
  }
  return index;
}

/// @p site's gate as alternatives; empty when the field always reaches the voice.
std::vector<ActiveAlternative> gates_of(const FieldSite& site, const std::string& own_key,
                                        const NativeSynthPatch& base) {
  return patch_tuning_detail::engine_field_gates(site.member_path, site_index(site), own_key, base);
}

/// Measure every field's admissible range for one engine mode.
///
/// The range is not written down anywhere a caller could read: it lives in
/// `clamp_synth_patch` as a `std::clamp` per field. Rather than mirror that
/// table — a mirror that would drift the first time a bound moved — the bounds
/// are measured through it: fill every field with a value far outside any
/// interval, clamp, and read back what survived. The field list is the walker's,
/// so a field added there is bounded here for free.
ModeBounds probe_bounds(SynthEngineMode mode) {
  std::vector<FieldSite> ends[2];
  for (const bool upper : {false, true}) {
    NativeSynthPatch probe;
    probe.mode = mode;
    Fields filler;
    filler.pass = FieldPass::kFill;
    filler.fill = upper ? kBoundProbe : -kBoundProbe;
    walk_fields(probe, filler);
    NativeSynthPatch clamped = clamp_synth_patch(probe);
    Fields reader;
    reader.sites = &ends[upper ? 1 : 0];
    walk_fields(clamped, reader);
  }
  ModeBounds bounds;
  bounds.reserve(ends[0].size());
  for (size_t i = 0; i < ends[0].size(); ++i) {
    const FieldSite& lo = ends[0][i];
    const FieldSite& hi = ends[1][i];
    const float open = lo.integer ? kIntBoundProbe : kBoundProbe;
    ProbedBound bound;
    bound.path = lo.path;
    bound.lo = lo.value;
    bound.hi = hi.value;
    bound.lo_open = std::fabs(lo.value) >= open;
    bound.hi_open = std::fabs(hi.value) >= open;
    bounds.push_back(std::move(bound));
  }
  std::sort(bounds.begin(), bounds.end(),
            [](const ProbedBound& a, const ProbedBound& b) { return a.path < b.path; });
  return bounds;
}

/// The probed ranges of @p mode's fields, measured once per mode on first use
/// and never on the audio thread.
const ModeBounds& mode_bounds(SynthEngineMode mode) {
  constexpr size_t kModeCount = static_cast<size_t>(kSynthEngineModeMax) + 1;
  static std::once_flag once[kModeCount];
  static ModeBounds table[kModeCount];
  static const ModeBounds kNone;
  const auto index = static_cast<size_t>(mode);
  if (index >= kModeCount) return kNone;
  std::call_once(once[index], [index, mode] {
    table[index] = probe_bounds(mode);
#if defined(SONARE_TUNING) && SONARE_TUNING
    // Either end still at the probe means the field is unbounded on that side,
    // which is not a range a fitter can search; the catalogue reports nothing
    // and the fitter falls back to its own heuristic rather than 1e30.
    for (const ProbedBound& b : table[index]) {
      if (std::abs(b.lo) >= kBoundProbe || std::abs(b.hi) >= kBoundProbe) continue;
      ::sonare::tuning::note_bound(b.path.c_str(), b.lo, b.hi);
    }
#endif
  });
  return table[index];
}

/// Every engine-section field of @p patch, as the read pass records it.
std::vector<FieldSite> engine_sites(const NativeSynthPatch& patch) {
  std::vector<FieldSite> sites;
  NativeSynthPatch copy = patch;
  Fields reader;
  reader.sites = &sites;
  walk_engine(copy, reader);
  return sites;
}

}  // namespace

void apply_patch_tuning(NativeSynthPatch& patch, const char* prefix) noexcept {
#if defined(SONARE_TUNING) && SONARE_TUNING
  const std::string owned(prefix == nullptr ? "" : prefix);
  if (owned.empty()) return;
  ::sonare::tuning::note_patch_mode(owned.c_str(), mode_name(patch.mode));
  mode_bounds(patch.mode);
  Fields apply;
  apply.pass = FieldPass::kApply;
  apply.prefix = &owned;
  walk_fields(patch, apply);
  // The override map is arbitrary text: it can name a non-finite value or one
  // outside the field's admissible range, and the callers clamp BEFORE calling
  // in. Re-clamp so a fitted value is evaluated exactly as the shipped build
  // would render it — otherwise a fit converges on a value the writeback path
  // truncates, and the result does not transfer.
  patch = clamp_synth_patch(patch);
#else
  (void)patch;
  (void)prefix;
#endif
}

bool apply_engine_params(NativeSynthPatch& patch, const EngineParam* params, size_t count,
                         std::string* error) {
  if (count == 0) return true;
  const auto refuse = [error](std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
  };
  for (size_t i = 0; i < count; ++i) {
    if (params[i].key == nullptr) return refuse("engine param '': key is null");
    for (size_t j = 0; j < i; ++j) {
      if (params[j].key != nullptr && std::strcmp(params[i].key, params[j].key) == 0) {
        return refuse(std::string("engine param '") + params[i].key + "': given more than once");
      }
    }
  }

  Assignment assignment;
  assignment.params = params;
  assignment.count = count;
  assignment.bounds = &mode_bounds(patch.mode);
  assignment.used.assign(count, false);
  NativeSynthPatch work = patch;
  Fields assign;
  assign.pass = FieldPass::kAssign;
  assign.assignment = &assignment;
  walk_engine(work, assign);
  if (!assignment.error.empty()) return refuse(std::move(assignment.error));
  for (size_t i = 0; i < count; ++i) {
    if (!assignment.used[i]) {
      return refuse(std::string("engine param '") + params[i].key + "': not a field of the " +
                    mode_name(patch.mode) + " engine");
    }
  }
  patch = work;
  return true;
}

const char* active_scope_name(ActiveCondition::Scope scope) noexcept {
  return scope == ActiveCondition::Scope::kPatch ? "patch" : "engine";
}

const char* active_relation_name(ActiveCondition::Relation relation) noexcept {
  switch (relation) {
    case ActiveCondition::Relation::kEq:
      return "eq";
    case ActiveCondition::Relation::kNe:
      return "ne";
    case ActiveCondition::Relation::kGt:
      return "gt";
    case ActiveCondition::Relation::kGe:
      return "ge";
    case ActiveCondition::Relation::kLt:
      return "lt";
    case ActiveCondition::Relation::kLe:
      return "le";
  }
  return "eq";
}

std::vector<EngineParamDescriptor> engine_param_descriptors(const NativeSynthPatch& base) {
  const std::vector<FieldSite> sites = engine_sites(base);
  const ModeBounds& bounds = mode_bounds(base.mode);
  std::vector<EngineParamDescriptor> out;
  out.reserve(sites.size());
  for (const FieldSite& site : sites) {
    EngineParamDescriptor d{};
    d.key = public_key(site.path);
    d.boolean = site.integer && site.type_lo == 0 && site.type_hi == 1;
    d.integer = site.integer && !d.boolean;
    const ProbedBound* bound = find_bound(bounds, site.path.c_str());
    d.bounded = bound != nullptr && !bound->lo_open && !bound->hi_open;
    d.lo = d.bounded ? bound->lo : 0.0f;
    d.hi = d.bounded ? bound->hi : 0.0f;
    d.unit = Unit::None;
    find_unit(site.member_path, &d.unit);
    d.value = site.value;
    d.active_when = gates_of(site, d.key, base);
    out.push_back(std::move(d));
  }
  return out;
}

namespace patch_tuning_detail {

std::vector<EngineFieldSite> engine_field_sites(const NativeSynthPatch& patch) {
  std::vector<EngineFieldSite> out;
  for (FieldSite& site : engine_sites(patch)) {
    Unit unit = Unit::None;
    const bool has_unit = find_unit(site.member_path, &unit);
    out.push_back({std::move(site.path), std::move(site.member_path), site.indexed, has_unit});
  }
  return out;
}

}  // namespace patch_tuning_detail

#if defined(SONARE_TUNING) && SONARE_TUNING
std::vector<std::string> patch_tuning_field_paths(const NativeSynthPatch& patch) {
  std::vector<FieldSite> sites;
  NativeSynthPatch copy = patch;
  Fields reader;
  reader.sites = &sites;
  walk_fields(copy, reader);
  std::vector<std::string> paths;
  paths.reserve(sites.size());
  for (FieldSite& site : sites) paths.push_back(std::move(site.path));
  return paths;
}
#endif

}  // namespace sonare::midi::synth
