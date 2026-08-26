#pragma once

/// @file harpsichord_voice.h
/// @brief Harpsichord core for the NativeSynth voice: a jack-and-plectrum
///        mechanism driving a registration of real string choirs.
///
/// Three consequences of that mechanism are what a plucked-string core with tone
/// knobs cannot produce:
///
///   - Key speed barely controls loudness — 3 to 6 dB across the instrument, and
///     not even monotonic, since past a peak speed the plectrum slips off sooner
///     and the note gets quieter. That is the plectrum's release law here, and
///     the engine opts out of the sampler velocity curve entirely.
///   - A loop closed through a tone-picked lowpass loses the treble long before
///     its nominal decay, so the loss filter is solved against decay targets at
///     the fundamental and at a named frequency (solve_string_loop_filter).
///   - The inharmonic content is the short undamped segment behind the bridge,
///     not string stiffness — the partials stay harmonic to a couple of cents.
///
/// Registration is separate string choirs: two 8' unisons and a 4' octave are
/// three delay lines at three periods, not one line with a mix knob.
///
/// The delay buffer is not owned here — the host attaches one slab per voice
/// slot before start(). RT contract: attach()/start()/render() are
/// allocation-free. Determinism: every noise source is the counter-based
/// (voice_index, note, age) stream, so identical events render bit-identically.

#include <cstddef>
#include <cstdint>

#include "midi/synth/string_loop.h"
#include "midi/synth/voice_random.h"
#include "rt/biquad_design.h"

namespace sonare::midi::synth {

/// Lowest fundamental the delay lines are sized for; notes below clamp to the
/// buffer (their pitch lands sharp instead of overflowing).
inline constexpr float kHarpsichordMinFundamentalHz = 20.0f;

/// Per-LINE delay capacity (samples) for a speaking string.
inline int harpsichord_buffer_capacity(double sample_rate) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  return static_cast<int>(sr / kHarpsichordMinFundamentalHz) + 8;
}

/// Per-voice delay-SLAB capacity (samples) the host must allocate: the two 8'
/// unison choirs at the full period, the 4' choir an octave up (half the
/// period), and the behind-the-bridge segment, which is a few percent of a
/// speaking length and gets an eighth of a span. Every span is reserved whether
/// or not the registration draws its stop, so attach() stays allocation-free.
inline int harpsichord_slab_capacity(double sample_rate) noexcept {
  const int full = harpsichord_buffer_capacity(sample_rate);
  return 2 * full + full / 2 + full / 8;
}

/// Harpsichord section of a NativeSynthPatch (used when mode == kHarpsichord).
///
/// The fields are the instrument's measurements, not normalized depths, so a
/// value can be checked against a published figure or a captured reference
/// rather than only against how it sounds.
struct HarpsichordPatchParams {
  // --- Registration -------------------------------------------------------
  /// The front 8' choir: one string per key at written pitch. The instrument's
  /// core voice, and the one stop that is always available.
  bool eight_a = true;
  /// The back 8' choir: a second unison, plucked further from the nut and so
  /// rounder. Drawing both 8' choirs is what makes the chorus a single string
  /// cannot — two strings tuned in unison by ear are never exactly together.
  bool eight_b = false;
  /// The 4' choir: physically separate strings an octave up, plucked by their
  /// own jacks on the same key. Not an octave doubling of the 8' signal — its
  /// own strings, its own decay, its own pluck point.
  bool four = false;

  // --- Plectrum and jack --------------------------------------------------
  /// Where each choir's jack plucks its string, as a fraction of the speaking
  /// length from the nut. The literature puts a harpsichord's between 1/8 and
  /// 1/4; the back 8' plucks further along than the front, which is what makes
  /// it the rounder of the two, and the 4' plucks nearer its nut still.
  float pluck_8a = 0.14f;
  float pluck_8b = 0.22f;
  float pluck_4 = 0.11f;
  /// The plectrum's edge, from a worn Delrin tongue (0, a slow rounded release)
  /// to a fresh-cut quill (1, a fast abrupt one). It sets how long the tongue
  /// takes to let the string go, which rounds the corners of the bridge-force
  /// wave and so rolls off the top of the series. It does NOT set the series'
  /// envelope — that is the pluck point's, and the two were one knob until the
  /// reference was measured wanting both a -6 dB partial balance in the bass and
  /// a -35 dB one at the top, which no single width reaches from either end.
  float plectrum_edge = 0.8f;
  /// How much of the plucking-point image the bridge sends back, in [0,1]. An
  /// ideally plucked string drives its bridge with a rectangle whose duty cycle
  /// is the plucking point, and the depth of that rectangle's return stroke is
  /// what the far end sends back: at 1 — a fixed end — a jack plucking at
  /// exactly 1/n erases every nth partial outright, while a real bridge drives
  /// the soundboard and cannot return quite everything, so the null fills in.
  ///
  /// The captured references DO have a comb, at their fourth or fifth partial;
  /// what differs is where it falls, not that it is there. Spending this knob to
  /// flatten one leaves an even harmonic series that reads as synthetic, and no
  /// dimension of the note grid reports the difference — so a fit is free to do
  /// exactly that. 1 is the harpsichord's setting; below it belongs to a voice
  /// whose reference genuinely has no null.
  float end_reflection = 1.0f;

  // --- What the key can do ------------------------------------------------
  /// The whole loudness range the key commands, in dB from the softest playable
  /// key to the plectrum's peak. Stated in dB because that is the form the
  /// measurement is published in: 3 to 6 dB across the instrument, against a
  /// piano's 40. A captured 8' reference measured 6.1 dB, flat across the
  /// compass to within 0.7.
  float velocity_range_db = 5.0f;
  /// The MIDI velocity at which the plectrum releases the string at its largest
  /// displacement — the measured peak sits near 2 m/s of key speed. 127 puts the
  /// peak at the top of the range, which makes the response monotonic.
  uint8_t peak_velocity = 127;
  /// How far the note falls back BEYOND that peak, in dB at full velocity. Past
  /// the peak a faster key makes the plectrum slip off the string sooner, so the
  /// note gets quieter rather than louder — the non-monotonic dynamic the
  /// instrument is known for. 0 leaves the response monotonic, which is what a
  /// sampled reference will show whether or not the instrument does.
  float velocity_droop_db = 0.0f;

  // --- Strings ------------------------------------------------------------
  /// t60 of the fundamental at A4, in seconds. It means what it says at every
  /// pitch: the loss filter is solved for its gain AT the fundamental.
  float decay_s = 3.0f;
  /// How much longer the bass rings, as octaves of t60 per keyboard octave below
  /// A4. Both captured references decay about 2^0.7 faster per octave up.
  float decay_stretch = 0.7f;
  /// t60 of the partial at @ref damping_ref_hz as a fraction of the
  /// fundamental's. 1 = every partial decays alike; smaller = the top goes
  /// first. Stated as a decay ratio rather than a filter coefficient so it keeps
  /// its meaning across the keyboard.
  float hf_damping = 0.45f;
  /// The frequency that ratio is quoted at. Absolute, because that is how string
  /// damping is measured — and because a ratio quoted at the octave asks a
  /// one-pole for a tilt between two frequencies it can barely tell apart, which
  /// it can only supply with a pole steep enough to erase the note's tenth
  /// partial. The solver clamps it clear of the fundamental and of Nyquist.
  float damping_ref_hz = 2000.0f;
  /// Residual mistuning between the two 8' choirs, in cents. A unison tuned by
  /// ear is never exact, and an exact one has no chorus at all.
  float unison_detune_cents = 3.5f;
  /// Residual mistuning of the 4' choir against the octave, in cents.
  float octave_detune_cents = 2.0f;

  // --- Behind the bridge --------------------------------------------------
  /// The string between the bridge and the hitch pin, in millimetres. It is
  /// never damped, and because its length is set by the case rather than by the
  /// note, its modes bear no fixed ratio to the fundamental — which is what puts
  /// inharmonic content into a sound whose partials are harmonic to a couple of
  /// cents. 0 removes the segment.
  float rear_segment_mm = 0.0f;
  /// How much of the bridge's motion crosses into that segment, in [0,1].
  float rear_coupling = 0.35f;
  /// t60 of that segment, in seconds. It is NOT the speaking string's: the
  /// bridge loads a short length heavily and most instruments weave listing
  /// cloth through the rear lengths to stop them. Taking the speaking t60 left a
  /// fixed-pitch ring 28 dB over the reference's late residue at F2 — the whole
  /// of a 41 dB excess there. 0 takes the speaking string's.
  float rear_decay_s = 0.0f;
  /// The speaking length of c'' in millimetres, and how far the bass departs
  /// from doubling that length every octave (0 = a Pythagorean scale, which no
  /// case is long enough for). Together they set the string length the rear
  /// segment is measured against; they do not affect pitch.
  float scale_c5_mm = 280.0f;
  float bass_foreshortening = 0.35f;

  // --- Mechanism ----------------------------------------------------------
  /// The plectrum scraping off the string as it releases: the chiff in front of
  /// the tone, in [0,1]. 0 = silent, and the render is the plain pluck.
  float pluck_noise = 0.0f;
  /// The jack dropping back and the damper landing at note-off, in [0,1]. Two
  /// events a few milliseconds apart, not one thump: the tongue pivots past the
  /// string first and the felt arrives after.
  float jack_noise = 0.0f;
  /// How long the felt damper takes to stop a string, as a t60 in seconds. Fast,
  /// but a damper and not a mute — a captured reference stops a bass string at
  /// about -40 dB/s.
  float damper_s = 0.09f;

  // --- Soundboard ---------------------------------------------------------
  /// The lowest frequency the board radiates, in Hz, at 12 dB per octave below
  /// it: a plate small against the wavelength goes stiffness-controlled AND
  /// cancels between its two faces, and neither loss is the other's. A
  /// harpsichord is heard through its partials at the bottom of its compass, and
  /// radiating the bridge force flat instead leaves the lowest octave led by a
  /// fundamental no real instrument produces — the register sweep puts the model
  /// 23.7 dB over the baroque slot at 30 Hz where its two siblings sit at 1.7 and
  /// 1.1. The general-MIDI slot shares the excess, so it cannot adjudicate this.
  /// 0 radiates the whole compass flat.
  float board_radiating_from_hz = 0.0f;
  /// How much brighter the board radiates than it is driven, in dB per octave.
  ///
  /// A string hands its bridge a force whose partials fall as sin(n.pi.beta)/n;
  /// what reaches a listener is that force through the board's radiation
  /// efficiency, which rises with frequency until the plate's critical frequency
  /// and levels off above it. Without the stage the model radiates the bridge
  /// force itself, and the bare force is far darker than any captured reference:
  /// at FF they hold partials 3-10 level with the fundamental where it puts them
  /// 13 dB under. 0 is that bare force, and it renders as a voice with no board.
  float board_tilt_db_oct = 0.0f;
  /// The board's diffuse radiation, in dB below the strings' own. Above the
  /// board's Schroeder frequency its modes are too dense to resolve one at a
  /// time, so what radiates there is a broadband field following the strings'
  /// energy rather than a bank of resonances — which is why it stops when they
  /// do and leaves nothing behind the note. @ref body carries the separable
  /// modes below that frequency. -120 removes it.
  float board_diffuse_db = -120.0f;
  /// The top of the 4' choir carries no dampers on a real instrument, so those
  /// strings go on sounding after the key is released. The MIDI note above which
  /// the 4' choir is left undamped; 128 dampers the whole choir.
  uint8_t undamped_from_note = 128;
};

/// Per-voice harpsichord state, embedded in NativeSynthVoice. The voice's
/// amplitude envelope / filter / mod matrix wrap around this core; render()
/// returns the raw radiated sample.
class HarpsichordVoiceCore {
 public:
  /// CONTROL-thread wiring (or audio-thread pointer assignment before start()):
  /// hands the core its delay slab. @p per_line_capacity is the speaking-string
  /// span length; the slab holds the spans harpsichord_slab_capacity() sizes.
  void attach(float* slab, int per_line_capacity) noexcept {
    slab_ = slab;
    capacity_ = per_line_capacity;
  }

  /// Voices the note: works out the plectrum's release displacement for
  /// @p velocity, solves each drawn choir's loss filter, and loads the pluck.
  void start(const HarpsichordPatchParams& params, double sample_rate, uint8_t note,
             uint8_t velocity, uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / vibrato / drift / glide), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Key release: the jack falls, the tongue pivots past the string without
  /// plucking it again, and the damper lands. Undamped choirs keep ringing.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

 private:
  /// A choir of strings and the jack that plucks it.
  struct Choir {
    StringLoop loop;
    /// Output level (0 = the stop is not drawn and the loop is skipped).
    float level = 0.0f;
    /// The loop's actual period in samples — the delay line plus the feedback
    /// path and the loss filter's phase delay — and where along it this choir's
    /// jack plucks. One period of bridge force is injected over exactly this
    /// span, so it must be the period the loop will circulate it at rather than
    /// the ideal one, or the wave meets itself a sample or two early.
    float n_eff = 0.0f;
    float duty = 0.0f;
    /// How many whole samples of it are injected, and the mean of exactly those
    /// samples. The rectangle is zero-mean as a continuous shape, but the samples
    /// taken from it are not, and a string cannot hold a DC displacement — its
    /// two ends are fixed. The loop can: a lowpass closed at unity DC gain is
    /// LEAST damped there, so the leftover offset outlives every partial, answers
    /// at the same frequency whatever is struck, and survives the damper.
    int inject_len = 0;
    float dc_trim = 0.0f;
    /// Whether the damper reaches this choir at note-off.
    bool damped = true;
  };

  float* slab_ = nullptr;
  int capacity_ = 0;

  Choir eight_a_;
  Choir eight_b_;
  Choir four_;

  /// The undamped string behind the bridge. rear_level_ == 0 -> the segment is
  /// skipped and the render is the plain choirs.
  StringLoop rear_;
  float rear_level_ = 0.0f;
  float rear_drive_ = 0.0f;

  /// The plectrum's release, injected as one period of the bridge-force wave an
  /// ideally plucked string produces: a rectangle whose duty cycle is the
  /// plucking point, which is the waveform whose partials are sin(n.pi.beta)/n.
  /// One period of it loads the loop, so the loop then circulates exactly that
  /// series — the comb and its 1/n envelope come out of the same shape instead
  /// of an envelope being chosen separately from the comb.
  ///
  /// It is a closed-form function of position rather than a filtered burst, and
  /// deterministic, because a plectrum is not a noise source. That is why a
  /// harpsichord's attack is the same every time and a noise-excited plucked
  /// string's is not.
  int pluck_pos_ = 0;
  int pluck_span_ = 0;
  float pluck_amp_ = 0.0f;
  /// The rectangle's corner radius in samples: how long the tongue takes to let
  /// the string go. Absolute, not a share of the period — the release is the
  /// plectrum's and the tension's, so the same release cuts the series at a
  /// lower partial the shorter the string, which is the keyboard-wide darkening
  /// the references show and a pitch-relative width cannot produce.
  float edge_w_ = 1.0f;
  /// How much of the plucking-point image comes back — 1 cuts an ideal comb.
  float pluck_image_ = 1.0f;
  /// Last sample's raw excitation, so the rear segment can be driven by the
  /// bridge force's transient rather than by the force itself.
  float exc_prev_ = 0.0f;

  /// Mechanism noise. Both are silent unless their patch amount is non-zero, and
  /// a silent one is skipped rather than multiplied by zero.
  VoiceRandomSequence noise_;
  float chiff_amount_ = 0.0f;
  int chiff_pos_ = 0;
  int chiff_len_ = 0;
  float chiff_lp_ = 0.0f;
  float chiff_alpha_ = 1.0f;
  float jack_amount_ = 0.0f;
  int jack_pos_ = 0;
  int jack_len_ = 0;
  /// The board's radiation efficiency, as a fractional-slope tilt: three
  /// one-pole/one-zero sections spread geometrically across the plate's band, so
  /// the cascade holds a slope of a few dB per octave that no single first-order
  /// section can (one is 6 dB/oct or nothing). Skipped entirely at zero tilt.
  struct TiltSection {
    float g = 1.0f;
    float zero = 0.0f;
    float pole = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
  };
  static constexpr int kTiltSections = 3;
  TiltSection tilt_[kTiltSections]{};
  bool tilt_on_ = false;

  /// The board's low-frequency radiation limit: a fourth-order Butterworth, as
  /// two staggered-Q sections. The order is what the references measure — 27 dB
  /// between FF and its octave — and the flat passband is what a cascade of
  /// identical one-poles cannot give: that knee spans two octaves, so reaching
  /// the fundamental at all costs 7 dB of the very partials the instrument leads
  /// with, and the note-29 band shares go from 7.50 to 9.20 while the picture
  /// itself improves. Skipped entirely when the patch does not ask for one.
  static constexpr int kRadiationStages = 2;
  rt::BiquadState hp_[kRadiationStages]{};
  bool hp_on_ = false;

  /// The soundboard's diffuse field, tracking the strings. Silent at level 0,
  /// and skipped rather than added at zero gain.
  float diffuse_level_ = 0.0f;
  float diffuse_env_ = 0.0f;
  float diffuse_follow_ = 0.0f;
  float diffuse_lp_ = 0.0f;
  float diffuse_tilt_ = 1.0f;
  float diffuse_top_ = 0.0f;
  float diffuse_top_a_ = 1.0f;
  uint64_t diffuse_age_ = 0;
  /// Samples from the jack's fall to the damper's arrival — the two mechanism
  /// events are not simultaneous and they do not sound alike.
  int damper_delay_ = 0;
  float jack_lp_ = 0.0f;
  float jack_alpha_ = 1.0f;

  /// Level trim bringing the raw string sample up to a musical voice level.
  float output_scale_ = 1.0f;
};

}  // namespace sonare::midi::synth
