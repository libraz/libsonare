#include "midi/synth/patch_tuning.h"

#include "midi/synth/native_synth.h"
#include "util/tunable.h"

// The field table below is compiled only into a tuning build. This translation
// unit is linked into every target including the WebAssembly module, where a
// couple of hundred string literals and an <string> dependency would be real
// weight in a binary under a size gate — for a table a shipped build can never
// reach, since nothing there ever populates the override map.
#if defined(SONARE_TUNING) && SONARE_TUNING

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace sonare::midi::synth {

namespace {

/// What a walk over the field table does with each field it visits.
enum class FieldPass {
  kApply,   ///< normal: replace the value with its override, if one is set
  kFill,    ///< write a probe value into every field, to be clamped afterwards
  kRead,    ///< read each field back out (the clamped probe = one of its bounds)
  kReport,  ///< collect each field's path, leaving the value alone
};

/// Collects the two ends of each field's admissible range across two kRead
/// walks. Keyed by path rather than by full key: a bound belongs to the field,
/// so every patch carrying that field shares it.
struct BoundsCollector {
  std::map<std::string, float> lo;
  bool upper = false;
};

/// The magnitude written into every field before clamping. Finite (so
/// `clamp_synth_patch`'s non-finite fallback does not fire and substitute a
/// default) and far outside every interval the engines accept, so what comes
/// back is the clamp bound itself. A field that comes back unchanged is one
/// `clamp_synth_patch` does not bound at all.
constexpr float kBoundProbe = 1e30f;

/// One field: apply an override, fill a probe, or read a bound back out.
struct Fields {
  const std::string& prefix;
  FieldPass pass = FieldPass::kApply;
  float fill = 0.0f;
  BoundsCollector* bounds = nullptr;
  std::vector<std::string>* paths = nullptr;

  float operator()(const char* path, float current) const {
    if (pass == FieldPass::kFill) return fill;
    if (pass == FieldPass::kRead) {
      read_bound(path, current);
      return current;
    }
    if (pass == FieldPass::kReport) {
      if (paths != nullptr) paths->emplace_back(path);
      return current;
    }
    return ::sonare::tuning::tunable_keyed((prefix + '.' + path).c_str(), current);
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
  int as_int(const char* path, int current, int lo, int hi) const {
    if (pass == FieldPass::kFill) {
      return std::clamp(static_cast<int>(std::lround(fill_int())), lo, hi);
    }
    if (pass == FieldPass::kRead) {
      read_bound(path, static_cast<float>(current));
      return current;
    }
    if (pass == FieldPass::kReport) {
      if (paths != nullptr) paths->emplace_back(path);
      return current;
    }
    const float value =
        ::sonare::tuning::tunable_keyed((prefix + '.' + path).c_str(), static_cast<float>(current));
    if (!std::isfinite(value)) return current;
    return std::clamp(static_cast<int>(std::lround(value)), lo, hi);
  }

 private:
  /// The probe an integer field is filled with. The float probe is 1e30, which
  /// no integer type can hold, so the sign is taken from it and the magnitude
  /// from what an `int` can represent. `clamp_synth_patch` narrows it to the
  /// real bound exactly as it does for a float, so the bound stays measured
  /// rather than mirrored from the clamp.
  float fill_int() const {
    const float sign = fill < 0.0f ? -1.0f : 1.0f;
    return sign * static_cast<float>(std::numeric_limits<int>::max() / 2);
  }

  void read_bound(const char* path, float value) const {
    if (bounds == nullptr) return;
    if (!bounds->upper) {
      bounds->lo[path] = value;
      return;
    }
    const auto it = bounds->lo.find(path);
    if (it == bounds->lo.end()) return;
    // Either end still at the probe means the field is unbounded on that side,
    // which is not a range anything can search; report nothing and let the
    // caller fall back to its own heuristic rather than hand out 1e30.
    if (std::abs(it->second) >= kBoundProbe || std::abs(value) >= kBoundProbe) return;
    ::sonare::tuning::note_bound(path, it->second, value);
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

/// A DAHDSR section (`amp_env`, `filter_env`, an FM operator's `env`).
void apply_env(DahdsrConfig& e, const Fields& f, const std::string& path) {
  const auto at = [&](const char* leaf, float current) {
    return f((path + '.' + leaf).c_str(), current);
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
  // The oscillator count and the body voicing: the two shared fields that
  // switch a mechanism rather than trim one, and the two whose absence made a
  // sweep of `detune_cents` or `body_mix` read as a structural answer.
  I(unison);
  I_TYPED(body, static_cast<int>(BodyType::kNone), static_cast<int>(BodyType::kVocal));
  F(detune_cents);
  F(drift_cents);
  F(drift_rate_hz);
  F(pitch_offset_cents);
  F(gain);
  F(cutoff_hz);
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
  F(piano.soundboard);
  F(piano.release_damp_s);
}

void apply_pipe_organ(NativeSynthPatch& p, const Fields& f) {
  F(pipe_organ.brightness);
  F(pipe_organ.tone_decay_s);
  F(pipe_organ.breath);
  F(pipe_organ.chiff);
  F(pipe_organ.chiff_ms);
  F(pipe_organ.release_damp_s);
  F(pipe_organ.reed);
  F(pipe_organ.radiation);
  F(pipe_organ.keytrack);
  F(pipe_organ.tremulant_rate_hz);
  F(pipe_organ.tremulant_depth);
  F(pipe_organ.wind_sag);
  F(pipe_organ.swell);
  for (int i = 0; i < kMaxPipeRanks; ++i) {
    PipeOrganRank& r = p.pipe_organ.ranks[static_cast<size_t>(i)];
    const std::string base = "pipe_organ.ranks" + std::to_string(i) + '.';
    r.footage_mult = f((base + "footage_mult").c_str(), r.footage_mult);
    r.brightness = f((base + "brightness").c_str(), r.brightness);
    r.level = f((base + "level").c_str(), r.level);
    r.reed = f((base + "reed").c_str(), r.reed);
    r.radiation = f((base + "radiation").c_str(), r.radiation);
  }
}

void apply_bowed_string(NativeSynthPatch& p, const Fields& f) {
  F(bowed_string.bow_position);
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
}

void apply_reed(NativeSynthPatch& p, const Fields& f) {
  F(reed.breath_pressure);
  F(reed.vel_to_breath);
  F(reed.reed_stiffness);
  F(reed.reed_opening);
  F(reed.brightness);
  F(reed.damping);
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
}

void apply_brass(NativeSynthPatch& p, const Fields& f) {
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
  F(brass.brassiness);
  F(brass.cuivre_dynamics);
  F(brass.mute);
  F(brass.half_valve);
  F(brass.dynamic_lip);
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
}

void apply_ks(NativeSynthPatch& p, const Fields& f) {
  F(ks.brightness);
  F(ks.decay_s);
  F(ks.decay_stretch);
  F(ks.pick_position);
  F(ks.exc_brightness);
  F(ks.vel_to_brightness);
  F(ks.release_damp_s);
  F(ks.slap);
  F(ks.polarization);
  F(ks.body_coupling);
  F(ks.pluck_style);
  F(ks.nail);
  F(ks.pickup_pos);
  F(ks.dispersion);
  F(ks.tension_mod);
  F(ks.octave_mix);
  F(ks.keyoff_noise);
}

void apply_plucked_string(NativeSynthPatch& p, const Fields& f) {
  F(plucked_string.brightness);
  F(plucked_string.decay_s);
  F(plucked_string.decay_stretch);
  F(plucked_string.pick_position);
  F(plucked_string.exc_brightness);
  F(plucked_string.vel_to_brightness);
  F(plucked_string.release_damp_s);
  F(plucked_string.buzz);
}

void apply_harpsichord(NativeSynthPatch& p, const Fields& f) {
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
  F(free_reed.attack_ms);
  F(free_reed.release_ms);
  F(free_reed.breath_noise);
}

void apply_vocal(NativeSynthPatch& p, const Fields& f) {
  F(vocal.brightness);
  F(vocal.breath_noise);
  F(vocal.vibrato_rate_hz);
  F(vocal.vibrato_depth);
  F(vocal.attack_ms);
  F(vocal.release_ms);
}

void apply_modal(NativeSynthPatch& p, const Fields& f) {
  I(modal.num_modes);
  F(modal.decay_s);
  F(modal.decay_stretch);
  F(modal.strike_brightness);
  F(modal.vel_to_brightness);
  F(modal.release_damp_s);
  for (int i = 0; i < kMaxModalModes; ++i) {
    ModalMode& m = p.modal.modes[static_cast<size_t>(i)];
    const std::string base = "modal.modes" + std::to_string(i) + '.';
    m.ratio = f((base + "ratio").c_str(), m.ratio);
    m.gain = f((base + "gain").c_str(), m.gain);
    m.decay_scale = f((base + "decay_scale").c_str(), m.decay_scale);
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
  for (int i = 0; i < kAdditivePartials; ++i) {
    float& d = p.additive.drawbars[static_cast<size_t>(i)];
    d = f(("additive.drawbars" + std::to_string(i)).c_str(), d);
  }
}

void apply_percussion(NativeSynthPatch& p, const Fields& f) {
  // The counts come first because they decide whether the fields under them do
  // anything: sweeping the shell's frequencies with `shell_num_modes` at 0
  // reads as "the shell cannot reach this measurement" and means "the shell is
  // switched off".
  I(percussion.num_modes);
  I(percussion.shell_num_modes);
  // Neither is narrowed by `clamp_synth_patch`, so each states the range its
  // own type defines. `exclusive_class` is the GM mute group — 0 is "none",
  // and it is here so a choke relationship can be tested without a rebuild.
  I_TYPED(percussion.exclusive_class, 0, 255);
  I_TYPED(percussion.noise_output, static_cast<int>(SynthFilterOutput::kLowpass),
          static_cast<int>(SynthFilterOutput::kHighpass));
  for (int i = 0; i < kMaxPercussionModes; ++i) {
    const std::string index = std::to_string(i);
    float& ratio = p.percussion.mode_ratios[static_cast<size_t>(i)];
    ratio = f(("percussion.mode_ratios" + index).c_str(), ratio);
    float& alpha = p.percussion.mode_alpha[static_cast<size_t>(i)];
    alpha = f(("percussion.mode_alpha" + index).c_str(), alpha);
  }
  F(percussion.mode_decay_s);
  F(percussion.tone_gain);
  F(percussion.tone_direct);
  F(percussion.base_freq_hz);
  F(percussion.pitch_drop);
  F(percussion.pitch_drop_ms);
  F(percussion.strike_r);
  F(percussion.strike_theta);
  F(percussion.noise_gain);
  F(percussion.noise_decay_ms);
  F(percussion.noise_cutoff_hz);
  F(percussion.noise_q);
  F(percussion.shell_mix);
  for (int i = 0; i < kMaxShellModes; ++i) {
    const std::string index = std::to_string(i);
    float& freq = p.percussion.shell_freq_hz[static_cast<size_t>(i)];
    freq = f(("percussion.shell_freq_hz" + index).c_str(), freq);
    float& t60 = p.percussion.shell_t60_s[static_cast<size_t>(i)];
    t60 = f(("percussion.shell_t60_s" + index).c_str(), t60);
    float& weight = p.percussion.shell_weight[static_cast<size_t>(i)];
    weight = f(("percussion.shell_weight" + index).c_str(), weight);
  }
  F(percussion.noise_air_hz);
  F(percussion.wire_buzz);
  F(percussion.wire_threshold);
  F(percussion.wire_cutoff_hz);
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

void apply_fm(NativeSynthPatch& p, const Fields& f) {
  for (int i = 0; i < kMaxFmOperators; ++i) {
    FmOperatorParams& op = p.fm.ops[static_cast<size_t>(i)];
    const std::string base = "fm.ops" + std::to_string(i);
    op.ratio = f((base + ".ratio").c_str(), op.ratio);
    op.detune_cents = f((base + ".detune_cents").c_str(), op.detune_cents);
    op.level = f((base + ".level").c_str(), op.level);
    op.vel_to_level = f((base + ".vel_to_level").c_str(), op.vel_to_level);
    op.key_rate_scale = f((base + ".key_rate_scale").c_str(), op.key_rate_scale);
    op.feedback = f((base + ".feedback").c_str(), op.feedback);
    apply_env(op.env, f, base + ".env");
  }
}

#undef F

/// Walk every field this patch's engine exposes, doing whatever `f`'s pass says.
void walk_fields(NativeSynthPatch& p, const Fields& f) {
  apply_common(p, f);
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
      break;
  }
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
  }
  return "subtractive";
}

/// Report each of this engine's fields' admissible range to the knob catalogue.
///
/// The range is not written down anywhere a fitter could read: it lives in
/// `clamp_synth_patch` as a `std::clamp` per field. Rather than mirror that
/// table — a mirror that would drift the first time a bound moved — the bounds
/// are measured through it: fill every field with a value far outside any
/// interval, clamp, and read back what survived. The field list is the same one
/// the override layer already walks, so a field added there is bounded here for
/// free.
///
/// Once per engine mode. The walk runs while the fallback tables are being
/// built, never on the audio thread.
void record_engine_bounds(const NativeSynthPatch& patch) {
  static bool done[16] = {};
  const auto mode = static_cast<size_t>(patch.mode);
  if (mode >= sizeof(done) / sizeof(done[0]) || done[mode]) return;
  done[mode] = true;

  const std::string unused;
  BoundsCollector collector;
  for (const bool upper : {false, true}) {
    NativeSynthPatch probe = patch;
    walk_fields(probe, Fields{unused, FieldPass::kFill, upper ? kBoundProbe : -kBoundProbe});
    NativeSynthPatch clamped = clamp_synth_patch(probe);
    collector.upper = upper;
    walk_fields(clamped, Fields{unused, FieldPass::kRead, 0.0f, &collector});
  }
}

}  // namespace

void apply_patch_tuning(NativeSynthPatch& patch, const char* prefix) noexcept {
  const std::string owned(prefix == nullptr ? "" : prefix);
  if (owned.empty()) return;
  ::sonare::tuning::note_patch_mode(owned.c_str(), mode_name(patch.mode));
  record_engine_bounds(patch);
  walk_fields(patch, Fields{owned});
  // The override map is arbitrary text: it can name a non-finite value or one
  // outside the field's admissible range, and the callers clamp BEFORE calling
  // in. Re-clamp so a fitted value is evaluated exactly as the shipped build
  // would render it — otherwise a fit converges on a value the writeback path
  // truncates, and the result does not transfer.
  patch = clamp_synth_patch(patch);
}

std::vector<std::string> patch_tuning_field_paths(const NativeSynthPatch& patch) {
  const std::string unused;
  std::vector<std::string> paths;
  NativeSynthPatch copy = patch;
  Fields report{unused, FieldPass::kReport, 0.0f, nullptr, &paths};
  walk_fields(copy, report);
  return paths;
}

}  // namespace sonare::midi::synth

#else  // !SONARE_TUNING

namespace sonare::midi::synth {

void apply_patch_tuning(NativeSynthPatch&, const char*) noexcept {}

}  // namespace sonare::midi::synth

#endif  // SONARE_TUNING
