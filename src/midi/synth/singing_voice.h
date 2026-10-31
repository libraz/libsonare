#pragma once

/// @file singing_voice.h
/// @brief Self-oscillating singing voice for the NativeSynth (mode kSinging): a
///        two-mass fold valve solved implicitly against a Kelly-Lochbaum tract
///        and an open-end lip termination. Pitch, loudness and timbre are coupled
///        the way a voice couples them, because nothing here synthesises them apart.
///
/// The four excitation axes all reach the model. Force is lung pressure paired
/// with adduction; position and brightness draw the tract's constriction place
/// and degree (the vowel); morph leans the cover fraction (the register). The
/// fold's own F0 rises with pressure, so the tension a note asks for is read
/// from a measured inverse map (singing_pitch_compensation.inc) and weighted by
/// `pitch_compensation`: 1 holds the written pitch, 0 lets the physics through.
///
/// Every pressure is held in units of the fold's base stiffness, so a note, a
/// bend or a slur changes the fold's time scale and not its operating point.
/// Deliberately shares nothing with the reed: the reed's breath colours a tone it
/// never silences, while here force carries the note across the phonation threshold.
///
/// RT contract: start()/render() are allocation-free. Determinism: the aspiration
/// noise is the counter-based (voice_index, note, age) stream.

#include <cstdint>

#include "midi/control_value.h"
#include "midi/synth/acoustic_tube.h"
#include "midi/synth/excitation_axes.h"
#include "midi/synth/fold_valve.h"
#include "midi/synth/passive_coupling.h"
#include "midi/synth/player_trajectory.h"
#include "midi/synth/voice_random.h"
#include "rt/open_end_radiation.h"
#include "util/constants.h"

namespace sonare::midi::synth {

/// Tract sections: the longest tract (2 x 17.5 cm) at 48 kHz, `c / (2 fs)` each.
inline constexpr int kSingingTractSections = 100;

/// Reference adult tract length the `tract_length` multiplier scales, in metres.
inline constexpr float kSingingReferenceTractM = 0.175f;

/// Singing section of a NativeSynthPatch (used when mode == kSinging).
/// The anatomy and the per-note shape; the playing is ExcitationAxes.
///
/// This section lives only in the C++ patch. The C ABI carries deep per-engine
/// parameters inside named presets, so the way to reach a voice from a binding
/// is to name a preset, not to fill a struct.
struct SingingPatchParams {
  /// Tract length multiplier in [0.5, 2.0]; 1.0 = the reference 17.5 cm adult
  /// tract. Anatomy, so a patch field rather than a live axis.
  float tract_length = 1.0f;
  /// Fold mass multiplier in [0.25, 4.0]. With tract_length this is the
  /// "which body is singing" pair, and it is continuous.
  float fold_mass = 1.0f;
  /// Lip rounding in [0,1]. The third of Fant's three parameters: without it
  /// /u/ and /i/ collapse onto the same constriction position and degree.
  /// A patch field because ExcitationAxes carries four axes and the other three
  /// earn their place by being the ones a player moves within a phrase.
  float rounding = 0.0f;
  /// Rest of the position axis in [0,1] until its controller arrives: the
  /// constriction place, glottis 0 to lips 1 -- the vowel's front-back placement.
  float position = 0.3f;
  /// Rest of the brightness axis in [0,1] until its controller arrives: the
  /// constriction degree, 1 = open and bright, 0 = closed down to the area floor,
  /// logarithmic in area -- the vowel's openness.
  float brightness = 0.97f;
  /// Rest of the morph axis in [0,1] until its controller arrives: the lean of
  /// the cover fraction, 0 = thin cover (falsetto), 1 = thick cover (chest).
  float morph = 0.6f;
  /// Aspiration noise floor in [0,1] at zero adduction.
  float breath_noise = 0.1f;
  /// Voice-local vibrato rate (Hz). Read at note-on.
  float vibrato_rate_hz = 5.5f;
  /// Vibrato depth in [0,1]. Applied to fold tension, never to output pitch, so
  /// the amplitude and timbre modulation that accompany a real vibrato follow by
  /// themselves rather than being synthesised alongside.
  float vibrato_depth = 0.3f;
  /// Weight on the breath-to-F0 compensation table in [0,1]; 1 = fully
  /// compensated. The default is 1 because a note must sound at its note
  /// number; 0 stays reachable as the uncompensated voice, whose F0 rises with
  /// the breath as the fold's does.
  float pitch_compensation = 1.0f;
  /// Adduction onset (ms): the folds close from the breathing position into
  /// phonation at a note that is not slurred.
  float attack_ms = 30.0f;
  /// Release (ms): breath and adduction return to rest after note-off.
  float release_ms = 120.0f;

  // --- Syllable onset: the alveolar tap, the one consonant this voice has ---

  /// Closure depth of the onset tap in [0,1]; 1 = as closed as kMinArea allows,
  /// 0 = no onset at all (the voice sings a bare vowel). Not an axis: four is
  /// what ExcitationAxes carries, and the three it already spends are the ones
  /// a player moves WITHIN a phrase rather than at each note.
  float tap_depth = 0.7f;
  /// Closure duration in ms. A Japanese alveolar tap runs 20-30 ms; past about
  /// 60 ms it stops reading as a tap and starts reading as a stop.
  float tap_ms = 25.0f;
  /// Where along the tract the tap closes, in the same units as the position
  /// axis. Alveolar by default; this is what makes it a tap rather than a
  /// generic interruption.
  float tap_position = 0.85f;

  // --- Default articulation: what a bare MIDI track gets with no CC at all ---

  /// Velocity-to-subglottal-pressure depth in [0,1]. 0 makes velocity silent
  /// and the voice depends entirely on an incoming force axis.
  float velocity_to_force = 0.6f;
  /// Breath onset ramp in ms, applied to force at a note that is not slurred.
  float breath_onset_ms = 45.0f;
};

/// The fold, junction, tract and lips the voice renders, with the force pairing
/// (lung pressure and adduction) applied inside. Exposed so the compensation
/// fit and its drift test sweep exactly this assembly at an explicit tension.
class SingingPhonation {
 public:
  /// @param note_hz   Pitch the fold mass is scaled to; tension 1 rings there.
  /// @param fold_mass Anatomy multiplier (SingingPatchParams::fold_mass).
  void configure(double sample_rate, float note_hz, float fold_mass, float cover_fraction) noexcept;
  /// configure() and set_tract() as @p params voices @p note_hz, with every
  /// axis at its rest: the state a note starts from and the compensation fit measures.
  void configure_patch(double sample_rate, float note_hz,
                       const SingingPatchParams& params) noexcept;
  /// Tract shape: length in metres, lip rounding, and the position and
  /// brightness axes (constriction place and degree), each in [0,1].
  void set_tract(float length_m, float rounding, float position, float brightness) noexcept;
  /// Constriction only, for a moving vowel; the profile stays.
  /// Brightness is logarithmic in area: 1 open, 0 closed to the tube's area floor.
  void set_constriction(float position, float brightness) noexcept;
  /// The tongue tip's closure at @p position, a second articulator on top of
  /// the vowel: @p closure in [0,1] is logarithmic in area like brightness, so
  /// 1 closes to the area floor and 0 removes it.
  void set_tap(float position, float closure) noexcept;
  void set_cover_fraction(float fraction) noexcept { fold_.set_cover_fraction(fraction); }
  /// Pitch factor over the configured note (bend, slur): rescales the pressure
  /// unit with the fold's stiffness so the operating point stays put.
  void set_pitch_scale(float ratio) noexcept;
  /// @param force     Breath axis in [0,1]: lung pressure and adduction together.
  /// @param abduction Extra opening in [0,1] (1 = breathing position).
  /// @param tension   Fold tension over the note's own, 1 = ring at note_hz.
  /// @param noise     Aspiration draw, scaled here by the square of the flow.
  /// @return The radiated pressure, in the note-independent unit.
  float process(float force, float abduction, float tension, float noise) noexcept;
  void reset() noexcept;
  /// The tract and lips alone, driven by @p input behind a closed glottis: what
  /// returns to the source reflects back in. For reading the vowel's resonances.
  float probe_tract(float input) noexcept;

  /// Energy held by the fold, the tract and the lip termination together, in
  /// the tract's area-normalized units: the coupled system's share for a passivity test.
  double stored_energy() const noexcept;
  const PassiveCoupling& junction() const noexcept { return junction_; }
  const AcousticTube<kSingingTractSections>& tract() const noexcept { return tract_; }

 private:
  FoldValve fold_;
  PassiveCoupling junction_;
  AcousticTube<kSingingTractSections> tract_;
  rt::OpenEndRadiation lips_;
  float lips_reflected_ = 0.0f;
  /// Area profile of the rounding alone, which a tap is drawn over.
  float profile_[kSingingTractSections] = {};
  float length_m_ = kSingingReferenceTractM;
  float fold_mass_ = 1.0f;
  float pitch_ratio_ = 1.0f;
  float pitch_scale_ = 1.0f;
};

/// Cover fraction the morph axis reaches at @p morph in [0,1].
float singing_cover_fraction(float morph) noexcept;

/// Tension over the note's own that sounds @p note_hz at @p force with the
/// compensation weighted by @p weight (0 = the threshold tension at every force).
float singing_compensated_tension(float note_hz, float force, float weight) noexcept;

/// Per-voice singing state, embedded in NativeSynthVoice. render() returns the
/// raw voice sample; the wrapper's envelope and filter stand around it.
class SingingVoiceCore {
 public:
  /// Note-on at a note that is not slurred: the tap fires and the breath ramps.
  void start(const SingingPatchParams& params, double sample_rate, uint8_t note,
             Velocity16 velocity, uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / drift / glide / slur), 1 = on the started note.
  float render(float pitch_ratio) noexcept;
  /// Note-off: breath and adduction return to rest over release_ms.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

  /// Base axis position in [0,1] from the patch or a CC. @p present names the
  /// fields the caller filled; an axis it does not name keeps its value. Force
  /// is accepted here where the source-filter vocal declines it: the fold's
  /// pressure-driven F0 rise is answered by the compensation table.
  void set_excitation_base(const ExcitationAxes& base, uint32_t present) noexcept;
  void restore_excitation_base() noexcept;
  /// Mod-matrix offset on the same axes; composed with the base, never written over it.
  void set_excitation_mod(const ExcitationAxes& offsets) noexcept;
  /// Jump the smoothed axes to their targets.
  void snap_excitation() noexcept;

 private:
  /// Base, home (the start() value) and mod offset of the four axes.
  struct Axes {
    ExcitationAxes base;
    ExcitationAxes home;
    ExcitationAxes mod;
  };

  void refresh_targets() noexcept;
  /// Re-draws the constriction and cover from the live axes.
  void apply_shape() noexcept;
  /// Re-draws the tongue-tip closure from the tap's phase.
  void apply_tap() noexcept;

  double sample_rate_ = ::sonare::constants::kDefaultDawSampleRate;
  float note_hz_ = 220.0f;
  SingingPhonation phonation_;
  float compensation_ = 1.0f;

  Axes axes_;
  ExcitationAxes target_;
  ExcitationAxes live_;
  float ctrl_coeff_ = 0.0f;
  bool moving_ = false;
  /// Pitch factor the fold is tuned to, following the asked one; < 0 before the first sample.
  float ratio_ = -1.0f;
  float ratio_coeff_ = 0.0f;
  int shape_countdown_ = 0;

  // Breath onset (force) and adduction contours, both released together.
  OnsetContour breath_;
  OnsetContour adduction_;

  // Tap: raised-sine tongue-tip closure over 2 * tap_ms from note-on.
  float tap_depth_ = 0.0f;
  float tap_position_ = 0.85f;
  float tap_phase_ = 1.0f;
  float tap_inc_ = 0.0f;

  // Vibrato on tension; depth 0 skips the LFO.
  float vib_depth_ = 0.0f;
  float vib_phase_ = 0.0f;
  float vib_inc_ = 0.0f;

  float breath_noise_ = 0.0f;
  VoiceRandomSequence noise_;
  uint64_t drive_index_ = 0;
};

}  // namespace sonare::midi::synth
