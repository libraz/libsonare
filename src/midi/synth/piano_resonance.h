#pragma once

/// @file piano_resonance.h
/// @brief Shared piano soundboard and sympathetic-resonance state.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "rt/biquad_design.h"

namespace sonare::midi::synth {

/// Share of the raw string signal a piano host keeps in the mix. The rest of
/// the note reaches the listener through PianoSoundboard::process(), whose
/// phase-diffusing radiation path breaks the waveform's periodicity — heard
/// directly, a phase-coherent string loop reads as a literally vibrating
/// string (a guitar), not as an instrument radiating through a board.
inline constexpr float kPianoDirectGain = 0.3f;

/// How long the shared piano body keeps radiating after the last string is
/// released (seconds). The modal soundboard and the pedal-gated sympathetic
/// bank ring well past the ~120 ms voice release, so both hosts fold this into
/// their tail estimate — and the SF2 host uses it to decide how long a part's
/// body still costs CPU — or a bounce cuts the bloom off the last chord.
///
/// The longest-ringing member is the frame bank (kFrameT60S in
/// piano_resonance.cpp) and not the soundboard, and this covers it with room
/// to spare. What matters at the cut is the LEVEL, not the t60: measured on a
/// released note the render sits at -85 dBFS six seconds after note-off, -98 at
/// nine and -110 at twelve, so six seconds truncates below fourteen bits. A
/// body ringing past the estimate is a body whose last chord is clipped in the
/// rendered file and nowhere else, so nothing fails except that file — which is
/// also why raising this buys inaudibility that is already bought, at the cost
/// of lengthening every bounce.
inline constexpr float kPianoBodyRingS = 6.0f;

/// Pedal-gated sympathetic resonance: a small shared bank of string-mode
/// resonators driven by the bridge/voice mix while the dampers are lifted
/// (sustain pedal down). A reduced model of the undamped strings re-radiating
/// when the dampers are off (Lehtonen, Penttinen, Rauhala & Valimaki 2007).
/// One bank is shared by the whole instrument (not per voice); the host feeds
/// it the summed dry mix and adds the returned resonance back.
///
/// RT contract: prepare() is the only allocation site (it owns no heap, so it
/// is in fact allocation-free too); process()/reset() are allocation-free and
/// deterministic. The excitation is gated by a smoothed damper-open envelope,
/// and the resonators ring out with extra damping as the dampers fall.
class PianoResonanceBank {
 public:
  /// Tunes the mode bank for @p sample_rate and clears the state.
  void prepare(double sample_rate) noexcept;
  /// Tunes the bank from an explicit set of @p count mode frequencies (Hz;
  /// clamped to kResonanceModes, the surplus zeroed) instead of the built-in
  /// piano register grid, with a @p ring_t60_s resonator decay and a @p out_gain
  /// coupling level. Used to share this bank as a plucked-string sympathetic
  /// resonator (the open guitar/harp strings ringing behind the played note):
  /// every caller drives process() with the same sustain-pedal state the piano
  /// board uses, so the gate opens and closes with it rather than holding
  /// open. The piano prepare(double) path is untouched. RT contract identical
  /// to prepare().
  void prepare_custom(double sample_rate, const float* freqs, int count, float ring_t60_s,
                      float out_gain) noexcept;
  /// prepare_custom() preset for the plucked-string "sound halo": the bank is
  /// tuned to the standard-tuning open guitar strings (E2 A2 D3 G3 B3 E4) plus
  /// their low harmonics, ringing ~1.5 s at a weak coupling. Shared by every
  /// host that voices a ks.sympathetic patch so the tuning table lives in one
  /// place.
  void prepare_guitar_sympathetic(double sample_rate) noexcept;
  /// Clears the resonator state and the damper gate.
  void reset() noexcept;
  /// Adds the sympathetic resonance for one input sample. @p damper_open
  /// (sustain pedal down) gates the excitation through a smoothed envelope;
  /// returns the resonance to mix into the output.
  float process(float bridge_in, bool damper_open) noexcept;

 private:
  /// Two populations share this bank, and only one of them answers to the
  /// pedal. A grand's dampers stop partway up the treble; every string above
  /// that point is free at all times, so its ring is not a pedal effect and
  /// must not be gated by one. The modes below `ungated_count_` are that top,
  /// the rest are the damped register the pedal lifts.
  static constexpr int kResonanceModes = 44;
  struct Mode {
    float a1 = 0.0f;
    float a2 = 0.0f;
    float gain = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
  };
  std::array<Mode, kResonanceModes> modes_{};
  int ungated_count_ = 0;
  float gate_ = 0.0f;
  float gate_open_coeff_ = 1.0f;
  float gate_close_coeff_ = 1.0f;
  float ringout_ = 1.0f;
  float out_gain_ = 0.0f;
};

/// Shared modal soundboard: one fixed bank of second-order resonators, spread
/// across the soundboard's radiating range with a frequency-graded damping and
/// a low-mid radiation envelope, driven by the summed instrument output and
/// added back in parallel as a body coloration (the cheap, data-free end of
/// commuted synthesis). One board is shared by the whole instrument (not per
/// voice), so chords couple into a common body the way a real grand's strings
/// all drive one soundboard. The synthetic modal data is designed from plate
/// heuristics, so it needs no measured impulse response; the architecture is
/// the same if the coefficients are later refit to a measured admittance.
///
/// RT contract: prepare() is the only configuration site (it owns no heap);
/// process()/reset() are allocation-free and deterministic. Each resonator is
/// unity-peak normalized so the bank colours rather than rings away.
class PianoSoundboard {
 public:
  /// Tunes the mode bank for @p sample_rate and stores the patch @p mix in
  /// [0,1] (the soundboard return level); clears the resonator state.
  void prepare(double sample_rate, float mix) noexcept;
  /// Re-states the return level @p mix in [0,1] without touching the resonator
  /// state, so notes still sounding through the board keep their ring.
  void set_mix(float mix) noexcept { out_gain_ = std::clamp(mix, 0.0f, 1.0f); }
  /// Clears the resonator state.
  void reset() noexcept;
  /// Radiates one summed input sample: returns the phase-diffused complement
  /// of the host's direct share plus the (mix-scaled) modal colour.
  float process(float in) noexcept;
  /// A blow's worth of energy into the case network, from a note that has just
  /// started. Accumulated and spent at the network's own decimated rate, so
  /// several notes struck in one host block each contribute and none is lost
  /// between ticks. Costs nothing when the path is off, since the whole case
  /// block is already skipped at a zero level.
  void strike(float amount) noexcept { case_strike_ += amount; }
  /// The same blow into the board bank instead of the case network, spent on
  /// the next sample rather than at the decimated tick.
  ///
  /// Two structures answer one hammer over two timescales: the rim rings out
  /// in a fraction of a second and the low field it feeds rings for four, and
  /// one injection point cannot be both. Measured on three concert grands, the
  /// blow the top octave needs is the first of those -- C8's own note is over
  /// in half a second, so a blow spent into the four-second network is what is
  /// left sounding, and the same blow large enough to fill C8's attack pours
  /// four seconds of non-harmonic energy under every note in the middle of the
  /// keyboard, which needed none. This bank is 40 modes rather than the frame
  /// bank's eight, so a blow into it is a thud instead of a chord, and its
  /// decay is already the fraction of a second the measurement asks for.
  void strike_board(float amount) noexcept { board_strike_ += amount; }
  /// The side component of the last process() call: add it to the left leg
  /// and subtract it from the right. A grand is not a point source -- its
  /// board radiates to two listening positions through different paths -- and
  /// a mono return renders one however wide the per-voice pan scatter is.
  /// Zero at a zero width, where the whole layer folds out.
  float last_side() const noexcept { return side_; }
  /// The phase-diffused sample computed by the last process() call. Feed
  /// resonance banks from this (not the raw dry) so their returns share the
  /// radiated path's phase field instead of partially cancelling it.
  float last_diffused() const noexcept { return in1_; }

 private:
  static constexpr int kSoundboardModes = 40;
  struct Mode {
    float a1 = 0.0f;
    float a2 = 0.0f;
    float gain = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
  };
  std::array<Mode, kSoundboardModes> modes_{};
  /// A blow waiting to be handed to the bank above, cleared by the sample that
  /// spends it. Accumulated, so several notes struck in one host block each
  /// contribute; zero costs nothing, since the drive is then `bp` exactly.
  float board_strike_ = 0.0f;
  /// The one-pole that spreads it over the contact instead of delivering it in
  /// a single sample, and its coefficient. Unity gain at DC, so the blow's area
  /// does not move with the time constant and the level and the spread can be
  /// fitted apart. Both fold out of a shipped build at a zero strike gain.
  float board_strike_lp_ = 0.0f;
  float board_strike_a_ = 1.0f;
  // Frame: the plate and the rim, which the board above is not.
  //
  // Every member of the instrument that the model has is made of wood, and
  // wood cannot hold a note for seconds. A resonator's decay is set by its
  // loss factor, t60 ~ 2.2 / (f * eta), and spruce's eta of one to three
  // percent puts a 100 Hz soundboard mode at about a second. A grand also
  // carries a 150 kg cast-iron plate and a laminated rim, and iron's eta is
  // one to three PER MILLE -- an order of magnitude lower, which is seconds
  // to tens of seconds at the same frequency. Nothing else in the instrument
  // has that number.
  //
  // It matters because the reference has exactly that signature and the model
  // had none of it: struck anywhere on the keyboard, a dry concert grand goes
  // on radiating 60-250 Hz at a level its pitch barely moves -- over 0.4-1.4 s,
  // where the note's own fundamental is out of the band, -39.6 dB at C4 against
  // -37.7 at C7, flat to two decibels across three octaves -- and holds
  // 125-1000 Hz for seconds after the string is gone. That constant is a
  // property of the SUSTAIN; the attack window is not flat and never was, since
  // it still carries a strike transient the contact time shapes with pitch.
  // Nor does the bank's own decay follow the material figure quoted above --
  // see kFrameT60S, where the number that does land on iron measures wrong once
  // the damper is in the metric. What the material argument establishes is that
  // the member is there, not how long the bank should ring.
  // A pitch-independent level is what a fixed mass
  // struck by a fixed blow gives; it is not something the strings can produce,
  // and it is not something a room can produce either, since a linear space
  // cannot make 125 Hz out of a 4 kHz C8. Measured across three different
  // concert grands the attack level agrees to within 0.6 dB, which is what a
  // structure common to all of them looks like.
  // Eight, which with the cents split below is four near-degenerate pair
  // centres spread across kFrameFLow..kFrameFHigh. The count trades against
  // the split: fewer centres cover the band less finely, and a bank with too
  // few of them has to choose between covering the band and beating.
  static constexpr int kFrameModes = 8;
  std::array<Mode, kFrameModes> frame_{};
  // Case and rim: the DENSE late field, which a bank of eight resonators is not.
  //
  // The frame bank above answers every note at its own eight pitches, and that
  // is audible as exactly what it is. It is also the only member the model has
  // with a long decay, so the two are inseparable by calibration: switching it
  // off takes the note-invariant ringing from 5.5 to 1.5 on the shape metric
  // and the tail's band balance from 8.1 to 9.1 in the same move. They are the
  // same eight resonators.
  //
  // What the field has to be is not a matter of taste, and it is not diffusion
  // either. Read the reference's 40-300 Hz sustain under a note whose own
  // partials are all above the band -- so nothing the string radiates directly
  // is being counted -- and two numbers come back on all three concert grands:
  // about 0.28 resolvable resonances per hertz, each standing some THIRTY
  // decibels over the floor between them. Band-limited noise through the
  // identical transform gives 0.17 per hertz at eight decibels, and the frame
  // bank gives half the count at fifty to a hundred. The instrument is neither.
  // It is a dense set of resonances that are still individually resolvable, and
  // both numbers have to be quoted because either one alone is reachable by
  // something audibly wrong -- a short decay hits thirty decibels by having no
  // tail left to be peaky with.
  //
  // Those two numbers determine the network instead of being fitted into it. A
  // feedback delay network's modal density in modes per hertz is its TOTAL delay
  // in seconds, so 0.28 per hertz IS 0.28 seconds of delay spread over the
  // lines. Peak-to-floor follows from modal overlap -- a mode's bandwidth
  // 2.2/t60 over the spacing 1/T -- and thirty decibels is an overlap near 0.15,
  // which at this density is a t60 of about four seconds. Both were then
  // confirmed by measuring the built network exactly as the reference was
  // measured rather than by trusting the arithmetic.
  //
  // Also worth stating because the earlier reasoning here got it backwards: the
  // instrument's low field does NOT satisfy Schroeder's criterion and is not
  // supposed to. Modes per hertz and modal overlap are different quantities, and
  // a response with 0.28 modes per hertz whose modes are a fraction of a hertz
  // wide is a long way from being heard as a continuum -- which is exactly why
  // the reference measures thirty decibels peak-to-floor and not eight.
  //
  // No parameter of a bank reaches that. Packing the same eight modes into a
  // narrower band measured WORSE, because their bandwidth does not change with
  // their spacing, and shortening the decay to widen them removes the long tail
  // the bank exists to provide. The count is the quantity, and a count is not a
  // knob: matching the measured density with resonators takes over a thousand
  // biquads per part, against eight delay lines here.
  //
  // Its delays are longer than the case's own path lengths, and that is a
  // deliberate departure from geometry rather than an oversight. A grand's
  // reflections are one to nine milliseconds; a network built on those alone
  // has a small fraction of the measured density and rings as audibly as the
  // bank it replaces. The instrument reaches its density through the degrees of
  // freedom of a plate, a cavity, a rim and a lid, which a delay network does
  // not have -- so the density is bought with delay instead, and this member is
  // a perceptual stand-in for that structure and not a model of it.
  static constexpr int kCaseLines = 8;
  // The network runs decimated, at an eighth of the host rate.
  //
  // Density is bought with total delay and nothing else, so at the full rate the
  // measured 0.28 modes per hertz is 13416 samples of delay line per board --
  // and a player carries sixteen boards whether or not a piano is ever loaded on
  // one. That is a megabyte and a half of buffer, per player, for a member
  // deliberately driven at three hundred hertz and below; it is also enough to
  // overflow the stack of anything that holds two players as locals, which the
  // suite does.
  //
  // Decimating by eight buys the same 0.28 seconds of delay for an eighth of the
  // buffer and an eighth of the arithmetic, and costs nothing this member
  // needs: the internal Nyquist is 3 kHz at a 48 kHz host, and the two-pole
  // filter that band-limits the drive IS the anti-alias filter, forty decibels
  // down there at its default corner -- which is why the corner is clamped to a
  // tenth of the internal rate rather than left free. On the way out the held
  // sample is smoothed by a one-pole and then by a fourth-order lowpass that
  // removes the hold's images at multiples of the internal rate.
  static constexpr uint32_t kCaseDecim = 8;
  // Shared pool rather than a buffer per line, sized for the whole set at the
  // rates this synth runs at. Above them the lengths scale down together, which
  // costs density rather than correctness -- and density is the one property
  // this member is built to a measured figure.
  static constexpr size_t kCaseCapacity = 2048;
  std::array<float, kCaseCapacity> case_buf_{};
  std::array<uint32_t, kCaseLines> case_off_{};
  std::array<uint32_t, kCaseLines> case_len_{};
  std::array<uint32_t, kCaseLines> case_idx_{};
  std::array<float, kCaseLines> case_g_{};
  std::array<float, kCaseLines> case_lp_{};
  float case_lp_a_ = 1.0f;
  // Injection filter: two cascaded one-poles, so what the network is driven
  // with is the low band and not the whole spectrum.
  //
  // The band is the point of the member rather than a refinement of it. What
  // the density measurement establishes exists below three hundred hertz, and
  // the inter-partial floor ABOVE that is already where the reference puts it --
  // so a network driven full-band raises a floor that was correct in order to
  // fill one that was not, and measures exactly that way: at a level too low to
  // move the low band at all it still costs three decibels of note-invariant
  // residue keyboard-wide. Twelve decibels an octave rather than six because a
  // single pole is still only twenty decibels down two octaves up, which is
  // inside the range those terms can see.
  float case_in_a_ = 1.0f;
  float case_in_1_ = 0.0f;
  float case_in_2_ = 0.0f;
  float case_strike_ = 0.0f;
  // Decimation state: which sample of the current block we are on, the network
  // output being held across it, and the one-pole that smooths the hold.
  uint32_t case_phase_ = 0;
  float case_hold_ = 0.0f;
  float case_out_lp_ = 0.0f;
  float case_out_a_ = 1.0f;
  // Rejects the hold's images at k * sr/8 +- f, which sit 35-55 dB under the member
  // with the one-pole alone and are the whole 12-24 kHz content of a C4 attack.
  static constexpr int kCaseImageStages = 2;
  rt::BiquadState case_image_lp_[kCaseImageStages]{};
  // An allpass diffuser inside each line. Eight lines put eight echoes into
  // every circulation of the network, and eight is few enough that the pattern
  // is audible as flutter before the mixing matrix has had time to fill it in.
  // Density in TIME is what a delay length buys; density WITHIN one traversal
  // is what this buys, and they are not the same quantity -- lengthening the
  // lines to get the second costs modal spacing, which is why the standard cure
  // is a diffuser in the loop rather than a longer loop.
  //
  // Each stays short against its own line so it disperses the echo without
  // moving the mode it sits on, and mutually prime against every other length
  // in the network for the same reason those are prime against each other. At
  // a zero coefficient the stage is skipped rather than passed through: an
  // allpass at zero is a plain delay, which would lengthen the loop and
  // detune the network instead of leaving it alone.
  static constexpr size_t kCaseApCapacity = 256;
  std::array<float, kCaseApCapacity> case_ap_buf_{};
  std::array<uint32_t, kCaseLines> case_ap_off_{};
  std::array<uint32_t, kCaseLines> case_ap_len_{};
  std::array<uint32_t, kCaseLines> case_ap_idx_{};
  // Schroeder allpass diffusers (fixed capacity, no allocation: the lazy
  // fallback prepare runs on the audio thread; prepare() sets the active
  // lengths, clamped to the capacity at very high sample rates).
  static constexpr size_t kDiffuserCapacity = 2048;
  std::array<float, kDiffuserCapacity> diff_buf_[2]{};
  size_t diff_len_[2] = {0, 0};
  size_t diff_idx_[2] = {0, 0};
  float in1_ = 0.0f;
  float in2_ = 0.0f;
  float out_gain_ = 0.0f;
  // Radiation-path decorrelators: one allpass per leg, incommensurate with
  // each other and with the phase diffusers above.
  std::array<float, kDiffuserCapacity> side_buf_[2]{};
  size_t side_len_[2] = {0, 0};
  size_t side_idx_[2] = {0, 0};
  float side_ = 0.0f;
  // Sustain-air texture state (level-tracked bandpassed noise).
  float air_env_ = 0.0f;
  float air_lp_ = 0.0f;
  float air_lp2_ = 0.0f;
  float air_hp_ = 0.0f;
  // The drive's own copy of the air band. The board is shared by every note
  // sounding on the part, so this layer cannot be told which note it is
  // radiating; taking its excitation from the band it radiates IN is how the
  // note reaches it anyway. See kAirDriveBandMix.
  float air_drv_lp_ = 0.0f;
  float air_drv_lp2_ = 0.0f;
  float air_drv_hp_ = 0.0f;
  float air_attack_ = 0.0f;
  float air_release_ = 0.0f;
  float air_lp_a_ = 0.0f;
  float air_hp_a_ = 0.0f;
  uint32_t air_rng_ = 0x9E3779B9u;
};

}  // namespace sonare::midi::synth
