#pragma once

/// @file piano_resonance.h
/// @brief Shared piano soundboard and sympathetic-resonance state.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "rt/biquad_design.h"

namespace sonare::midi::synth {

/// Share of the raw string signal a piano host keeps in the mix; the rest
/// reaches the listener through PianoSoundboard's phase-diffusing radiation,
/// since a phase-coherent string loop heard directly reads as a guitar.
inline constexpr float kPianoDirectGain = 0.3f;

/// How long the shared piano body keeps radiating after the last string is
/// released (seconds); hosts fold it into their tail estimate so a bounce does
/// not cut the last chord's bloom. Measured on a released note: -85 dBFS at
/// 6 s after note-off (-98 at 9 s), i.e. below fourteen bits at the cut.
inline constexpr float kPianoBodyRingS = 6.0f;

/// Pedal-gated sympathetic resonance: a small shared bank of string-mode
/// resonators driven by the bridge/voice mix while the dampers are lifted
/// (sustain pedal down). A reduced model of the undamped strings re-radiating
/// when the dampers are off (Lehtonen, Penttinen, Rauhala & Valimaki 2007).
/// One bank is shared by the whole instrument (not per voice); the host feeds
/// it the summed dry mix and adds the returned resonance back.
///
/// RT contract: owns no heap; prepare()/process()/reset() are allocation-free
/// and deterministic. Resonators ring out with extra damping as dampers fall.
class PianoResonanceBank {
 public:
  /// Tunes the mode bank for @p sample_rate and clears the state.
  void prepare(double sample_rate) noexcept;
  /// Tunes the bank from an explicit set of @p count mode frequencies (Hz;
  /// clamped to kResonanceModes, the surplus zeroed) instead of the built-in
  /// piano register grid, with a @p ring_t60_s resonator decay and a @p out_gain
  /// coupling level. Used as a plucked-string sympathetic resonator (open
  /// guitar/harp strings), gated by the same sustain-pedal state as the piano.
  void prepare_custom(double sample_rate, const float* freqs, int count, float ring_t60_s,
                      float out_gain) noexcept;
  /// prepare_custom() preset for the plucked-string "sound halo": the bank is
  /// tuned to the standard-tuning open guitar strings (E2 A2 D3 G3 B3 E4) plus
  /// their low harmonics, ringing ~1.5 s at a weak coupling. Shared by every
  /// host that voices a ks.sympathetic patch.
  void prepare_guitar_sympathetic(double sample_rate) noexcept;
  /// Clears the resonator state and the damper gate.
  void reset() noexcept;
  /// Adds the sympathetic resonance for one input sample. @p damper_open
  /// (sustain pedal down) gates the excitation through a smoothed envelope;
  /// returns the resonance to mix into the output.
  float process(float bridge_in, bool damper_open) noexcept;

 private:
  /// A grand's dampers stop partway up the treble, so the modes below
  /// `ungated_count_` (the undamped top) ring regardless of the pedal; the
  /// rest are the damped register the pedal lifts.
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

/// Shared modal soundboard: a fixed bank of second-order resonators across
/// the board's radiating range (frequency-graded damping, low-mid radiation
/// envelope), driven by the summed instrument output and added back in
/// parallel. One board per instrument, so chords couple into a common body.
/// Modal data is designed from plate heuristics, not a measured response.
///
/// RT contract: owns no heap; process()/reset() are allocation-free and
/// deterministic. Each resonator is unity-peak normalized.
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
  /// A blow's worth of energy into the case network from a note that has just
  /// started; accumulated and spent at the network's decimated rate.
  void strike(float amount) noexcept { case_strike_ += amount; }
  /// The same blow into the 40-mode board bank, spent on the next sample. The
  /// top octave needs a sub-second knock (C8's note is over in half a second);
  /// spent into the four-second case network it would linger under every note.
  void strike_board(float amount) noexcept { board_strike_ += amount; }
  /// The side component of the last process() call: add it to the left leg
  /// and subtract it from the right. Zero at a zero width.
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
  /// Pending strike_board() energy, cleared by the sample that spends it.
  float board_strike_ = 0.0f;
  /// One-pole spreading the blow over the contact; unity DC gain, so level and
  /// spread fit independently.
  float board_strike_lp_ = 0.0f;
  float board_strike_a_ = 1.0f;
  // Frame: the cast-iron plate and rim. A dry concert grand keeps radiating
  // 60-250 Hz at a pitch-independent level (-39.6 dB at C4 vs -37.7 at C7,
  // 0.4-1.4 s), which only a fixed struck mass gives. Eight modes form four
  // near-degenerate pairs across kFrameFLow..kFrameFHigh.
  static constexpr int kFrameModes = 8;
  std::array<Mode, kFrameModes> frame_{};
  // Case and rim: a dense late field, as an eight-line feedback delay network.
  // The reference's 40-300 Hz sustain measures ~0.28 resolvable modes/Hz at
  // ~30 dB peak-to-floor on three grands; an FDN's density equals its total
  // delay (0.28 s) and that overlap needs t60 ~4 s. Delays exceed the case's
  // real paths on purpose: this is a perceptual stand-in, not a geometry.
  static constexpr int kCaseLines = 8;
  // Decimated by eight: 0.28 s of delay at full rate is 13416 samples per board
  // across sixteen boards per player. The drive's two-pole band limit is the
  // anti-alias filter, hence its corner clamp to a tenth of the internal rate.
  static constexpr uint32_t kCaseDecim = 8;
  // Shared pool for every line; above the supported rates the lengths scale
  // down together, costing density rather than correctness.
  static constexpr size_t kCaseCapacity = 2048;
  std::array<float, kCaseCapacity> case_buf_{};
  std::array<uint32_t, kCaseLines> case_off_{};
  std::array<uint32_t, kCaseLines> case_len_{};
  std::array<uint32_t, kCaseLines> case_idx_{};
  std::array<float, kCaseLines> case_g_{};
  std::array<float, kCaseLines> case_lp_{};
  float case_lp_a_ = 1.0f;
  // Injection filter: two cascaded one-poles (12 dB/oct) so only the low band
  // drives the network; a full-band drive raised the already-correct floor
  // above 300 Hz and cost 3 dB of note-invariant residue.
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
  // An allpass diffuser inside each line, against eight-echo flutter. Short
  // against its line so it does not move the mode, and skipped at a zero
  // coefficient: an allpass at zero is a plain delay that would detune the loop.
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
  // The drive's own copy of the air band: the shared board cannot be told which
  // note it radiates, so the layer is excited from its own band. See kAirDriveBandMix.
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
