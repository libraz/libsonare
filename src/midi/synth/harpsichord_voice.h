#pragma once

/// @file harpsichord_voice.h
/// @brief Harpsichord core for the NativeSynth voice: a jack-and-plectrum
///        mechanism driving a registration of real string choirs.
///
/// A harpsichord is not a bright guitar. What separates it from every other
/// plucked string is the mechanism between the key and the string, and three of
/// its consequences are things a plucked-string core parameterised by tone knobs
/// cannot produce:
///
///   - THE KEY BARELY CONTROLS THE LOUDNESS. The plectrum lifts the string to
///     the same place however fast the key falls, so the whole dynamic range of
///     the instrument is a few dB — the literature measures 3 to 6, against a
///     piano's 40 — and it is not even monotonic: past a peak key speed the
///     plectrum slips off the string sooner and the note gets QUIETER. Here that
///     is the plectrum's release law rather than a velocity knob turned down,
///     and the engine opts out of the sampler velocity curve entirely.
///   - THE TREBLE HAS TO KEEP RINGING. A loop closed through a lowpass picked
///     for its tone attenuates a treble fundamental on every traversal, and at a
///     thousand traversals a second the string is gone long before its nominal
///     decay. The loss filter here is solved against decay targets at the
///     fundamental and at a named frequency (solve_string_loop_filter), so the
///     top octave sustains for the time it was asked to.
///   - THE SOUND IS NOT CLEAN. Behind the bridge every string carries a short
///     undamped segment whose modes have nothing to do with the note being
///     played; the literature names that, not string stiffness, as where a
///     harpsichord's inharmonic content comes from. A harpsichord's partials
///     are harmonic to within a couple of cents, and it still does not sound
///     like a synthesizer.
///
/// Registration is modelled as what it is: separate choirs of strings, each with
/// its own jacks. Two 8' unisons and a 4' octave are three real delay lines at
/// three periods, not one line with a mix knob.
///
/// The delay buffer is NOT owned by the core: the host instrument allocates one
/// slab per voice slot in prepare() (the only allocation site) and attach()es it
/// before start(). RT contract: attach()/start()/render() are allocation-free.
/// Determinism: every noise source is the counter-based (voice_index, note, age)
/// stream, so identical event streams render bit-identically.

#include <cstddef>
#include <cstdint>

#include "midi/synth/string_loop.h"
#include "midi/synth/voice_random.h"

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
  /// The plectrum's edge, from a worn Delrin tongue (0, a wide rounded release)
  /// to a fresh-cut quill (1, a narrow abrupt one). A harder plectrum releases
  /// the string through a shorter contact and so excites more of the top.
  float plectrum_edge = 0.8f;

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
    /// Where along the string this choir's jack plucks, in samples of the loop
    /// period — the comb delay applied to its share of the excitation.
    int pluck_delay = 0;
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

  /// The plectrum's release pulse: a raised-cosine lobe whose width IS the
  /// contact patch (a harder plectrum releases through a narrower one) and whose
  /// height is the release displacement. It is a closed-form function of its
  /// position, not a filtered burst, which is what lets each choir's jack comb
  /// the same pulse at its own plucking point — and it is deterministic, because
  /// a plectrum is not a noise source. That is why a harpsichord's attack is the
  /// same every time and a noise-excited plucked string's is not.
  int pluck_pos_ = 0;
  int pluck_len_ = 0;
  int pluck_span_ = 0;
  float pluck_amp_ = 0.0f;

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
  /// Samples from the jack's fall to the damper's arrival — the two mechanism
  /// events are not simultaneous and they do not sound alike.
  int damper_delay_ = 0;
  float jack_lp_ = 0.0f;
  float jack_alpha_ = 1.0f;

  /// Level trim bringing the raw string sample up to a musical voice level.
  float output_scale_ = 1.0f;
};

}  // namespace sonare::midi::synth
