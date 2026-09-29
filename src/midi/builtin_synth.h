#pragma once

/// @file builtin_synth.h
/// @brief Minimal polyphonic oscillator synth — a deliberately plain electronic
///        sound source so MIDI arrangements bounce to audible output instead of
///        silence. A richer literature-backed instrument bank (FM, Karplus-Strong,
///        modal) is planned separately.
///
/// Covers note on/off, sustain, channel mode, CC7/CC10/CC11, per-channel bend
/// and pressure, and MPE through the shared zone model (midi/mpe.h) — so a
/// controller that configures a zone gets the same channel layout, the same
/// bend ranges and the same manager fold here as from the full instrument. The
/// only RPNs parsed are the two a zone needs, 00 06 and 00 00; everything else
/// in RPN / NRPN stays with NativeSynth / Sf2Player, and outside a zone the
/// bend range is the fixed one below rather than a configurable one.
///
/// Both protocols go through decode_channel_voice(), and controller values stay
/// at MIDI 2.0 width up to the float that consumes them, so a MIDI 2.0 velocity,
/// CC or bend is heard between the 7-bit (14-bit) steps while MIDI 1.0 input
/// renders exactly as before. MIDI 2.0 per-note pitch (Per-Note Pitch Bend,
/// RPNC #3, Note On attribute #3, Per-Note Management, RC 0/7) retunes the
/// addressed key only; the other per-note controllers are counted as skipped.
///
/// Volume, pan and expression follow the same laws as those two, so swapping the
/// instrument keeps an arrangement's balance and image: volume x expression as a
/// (v/127)^2 gain, and CC10 through the project's constant-power pan law, which
/// puts a centred voice at 1/sqrt(2) per channel.
///
/// RT contract: prepare() runs on the control thread and is the only allocation
/// site; on_event() and process() are allocation-, lock- and IO-free.
/// Determinism: no RNG, no clock, and voice stealing prefers a free voice then
/// the oldest, so a project bounces bit-identically within one build.

#include <array>
#include <cstdint>
#include <vector>

#include "midi/channel_voice_decode.h"
#include "midi/control_value.h"
#include "midi/instrument.h"
#include "midi/mpe.h"
#include "midi/per_note_state.h"
#include "midi/synth/channel_param_state.h"
#include "rt/pan_law.h"

namespace sonare::midi {

/// Oscillator waveform for the minimal synth.
enum class SynthWaveform : int {
  kSine = 0,
  kSaw = 1,
  kSquare = 2,
  kTriangle = 3,
};

/// Patch parameters. A zero-initialized config is sanitized into a usable sine
/// patch by BuiltinSynth (see clamp_config), so callers can fill only what they
/// care about.
struct BuiltinSynthConfig {
  SynthWaveform waveform = SynthWaveform::kSine;
  /// Master output gain applied to the summed voices (linear).
  float gain = 0.2f;
  /// ADSR in milliseconds / sustain in [0,1].
  float attack_ms = 5.0f;
  float decay_ms = 60.0f;
  float sustain = 0.7f;
  float release_ms = 120.0f;
  /// Maximum simultaneous voices (clamped to [1, kMaxVoices]).
  int polyphony = 16;
};

/// Largest voice pool the synth will allocate, regardless of config.
inline constexpr int kMaxSynthVoices = 64;

/// Returns a copy of @p cfg with every field clamped to a safe, audible range.
BuiltinSynthConfig clamp_synth_config(const BuiltinSynthConfig& cfg) noexcept;

/// Longest possible note tail (release) in samples for @p cfg at @p sample_rate.
/// Hosts use this to extend an offline bounce so the final note's release is not
/// truncated.
int64_t synth_tail_samples(const BuiltinSynthConfig& cfg, double sample_rate) noexcept;

class BuiltinSynth final : public MidiInstrument {
 public:
  explicit BuiltinSynth(const BuiltinSynthConfig& config) noexcept;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  bool process_source_tracks(const MidiInstrumentSourceOutput* outputs, size_t output_count,
                             int num_channels, int num_samples) noexcept override;
  bool supports_source_track_rendering() const noexcept override { return true; }
  void reset() override;
  int tail_samples() const noexcept override { return static_cast<int>(tail_samples_); }
  void on_event(uint32_t destination_id, const MidiEvent& event) noexcept override;

  /// Channel-voice messages received but not acted on: reserved statuses, per-note controllers
  /// other than pitch, and relative controllers on a parameter this synth does not hold. Cleared
  /// by reset().
  uint64_t skipped_event_count() const noexcept { return skipped_events_; }

 private:
  enum class Stage : uint8_t { kIdle = 0, kAttack, kDecay, kSustain, kRelease };

  struct Voice {
    bool active = false;
    uint8_t note = 0;
    uint8_t channel = 0;
    uint32_t source_track_id = 0;
    double phase = 0.0;           // [0,1)
    double base_phase_inc = 0.0;  // cycles per sample at the note pitch (no bend)
    double phase_inc = 0.0;       // effective increment incl. channel and per-note pitch
    float velocity = 0.0f;        // [0,1]
    float poly_pressure = 0.0f;   // per-note (poly) pressure in [0,1]
    float env = 0.0f;             // current envelope level
    Stage stage = Stage::kIdle;
    bool key_down = false;
    uint64_t age = 0;  // start order, for deterministic voice stealing
    PerNoteBinding per_note;
    // 2^(per-note semitones / 12); exactly 1.0 while the key carries no per-note pitch.
    double per_note_ratio = 1.0;
    bool has_attribute_pitch = false;  // Note On attribute #3, captured at note-on.
    uint16_t attribute_pitch_q7_9 = 0;
  };

  void note_on(uint8_t channel, uint8_t note, Velocity16 velocity, uint8_t attribute_type,
               uint16_t attribute_data, uint32_t source_track_id) noexcept;
  void note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept;
  void sustain_pedal(uint8_t channel, bool down) noexcept;
  // Per-channel expression. Pitch bend is a 14-bit value (center 8192) mapped
  // through a fixed +/-2 semitone range, or through the zone's own range when
  // the channel is in one; channel pressure applies to every voice on the
  // channel, poly pressure to the single matching note.
  void pitch_bend(uint8_t channel, Bend32 bend) noexcept;
  void channel_pressure(uint8_t channel, Control32 pressure) noexcept;
  void poly_pressure(uint8_t channel, uint8_t note, Control32 pressure) noexcept;
  void control_change(uint8_t channel, uint8_t controller, Control32 value) noexcept;
  // MIDI 2.0 Registered / Assignable Controllers, absolute and relative. RC 0/0 and 0/7 are read
  // from the message; every other one is lowered onto the MIDI 1.0 parameter-number path.
  void registered_controller(const Ump& ump, const ChannelVoiceEvent& ev) noexcept;
  void relative_controller(const ChannelVoiceEvent& ev) noexcept;
  // Recomputes the per-note share of @p v's pitch from its binding and attribute.
  void refresh_per_note_pitch(Voice& v) noexcept;
  // Re-evaluates every sounding voice on (channel, note), or on the whole channel when
  // @p all_notes is set (a sensitivity change reaches every key of it).
  void refresh_per_note_voices(uint8_t channel, uint8_t note, bool all_notes) noexcept;
  // Recomputes a channel's cached bend and pressure from the zone model, and
  // every channel of the zone when @p channel is its manager, whose values
  // reach all of them.
  void refresh_channel_expression(uint8_t channel) noexcept;
  // Accepts an MPE Configuration Message: the channels that entered or left the
  // zone lose their sounding notes and their controllers, so re-zoning
  // mid-performance cannot leave a note hanging on a channel with no owner
  // (M1-100-UM v1.1 section 2.2.3).
  void apply_mcm(uint8_t manager_channel, uint8_t member_count) noexcept;
  // Channel-mode "All Notes Off" (CC#123): release every sounding voice on the
  // channel (graceful, honours the release tail). `all_sound_off` (CC#120)
  // silences them immediately, bypassing the release stage.
  void all_notes_off(uint8_t channel) noexcept;
  void all_sound_off(uint8_t channel) noexcept;
  // Channel-mode "Reset All Controllers" (CC#121): lift the damper and return
  // every continuous controller this synth honours to its neutral value --
  // pitch bend to center (restoring active voices' pitch), channel/poly
  // pressure to zero and expression to full. Volume and pan survive, as MIDI
  // RP-015 requires: they are mix settings, not performance gestures. Without
  // the bend/pressure reset a prior MPE expression would bleed into subsequent
  // notes (detuned pitch, residual +pressure gain).
  void reset_all_controllers(uint8_t channel) noexcept;
  // Recomputes the cached gain / pan pair after a CC7, CC10 or CC11 change, so
  // the per-sample render only multiplies.
  void refresh_channel_controls(uint8_t channel) noexcept;
  float render_voice_sample(Voice& v) noexcept;
  // Adds one already-panned frame to a render target, folding to mono for a
  // single-channel host and fanning the mono fold to any channel beyond stereo.
  void add_frame(float* const* target, int num_channels, int sample, float left,
                 float right) const noexcept;

  BuiltinSynthConfig config_{};
  double sample_rate_ = 0.0;
  bool prepared_ = false;
  uint64_t next_age_ = 1;
  int64_t tail_samples_ = 0;

  // Per-stage per-sample envelope increments derived in prepare().
  float attack_inc_ = 1.0f;
  float decay_inc_ = 1.0f;
  float release_inc_ = 1.0f;

  // Per-channel mix controllers, at their GM power-on values. `gain` and
  // `pan_gains` are derived from the three CC values by
  // refresh_channel_controls(); the raw values are kept so a change to one
  // controller does not lose the others.
  struct ChannelControls {
    Control32 volume = Control32::from7(100);      // CC7
    Control32 pan = Control32::from7(64);          // CC10 (centre)
    Control32 expression = Control32::from7(127);  // CC11
    float gain = 1.0f;
    rt::PanGains pan_gains{};
  };

  std::array<ChannelControls, 16> channel_controls_{};
  std::array<bool, 16> sustain_down_{};
  // Per-channel expression as the render reads it. Bend is stored in semitones
  // (0 == centered); pressure in [0,1]. Both default to neutral so a project
  // that sends no expression bounces bit-identically to the synth before either
  // existed, and inside a zone both carry the manager's contribution folded in.
  std::array<float, 16> channel_bend_semitones_{};
  std::array<float, 16> channel_pressure_{};
  // Empty until an MPE Configuration Message arrives, and while it is empty
  // every channel reads as unassigned and nothing above behaves differently.
  MpeState mpe_{};
  // Only ever asked about RPN 00 06 and RPN 00 00, the two a zone is configured
  // with. The full parameter-number machinery stays with the bigger synths.
  std::array<synth::ChannelParamState, 16> params_{};
  // Per-note pitch rows (per key, surviving note-off) and RC 0/7 per channel.
  PerNotePitchTable per_note_pitch_{};
  std::array<Control32, 16> per_note_bend_sensitivity_{};
  uint64_t skipped_events_ = 0;
  std::vector<Voice> voices_;
};

}  // namespace sonare::midi
