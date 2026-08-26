#pragma once

/// @file builtin_synth.h
/// @brief Minimal polyphonic oscillator synth — a deliberately plain electronic
///        sound source so MIDI arrangements bounce to audible output instead of
///        silence. A richer literature-backed instrument bank (FM, Karplus-Strong,
///        modal) is planned separately.
///
/// Covers note on/off, sustain, channel mode, CC7/CC10/CC11 and MPE-style
/// expression (per-channel bend and pressure, which act per-note because MPE
/// gives each note its own member channel). RPN / NRPN, including a configurable
/// bend range, stay with NativeSynth / Sf2Player.
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

#include "midi/instrument.h"
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

 private:
  enum class Stage : uint8_t { kIdle = 0, kAttack, kDecay, kSustain, kRelease };

  struct Voice {
    bool active = false;
    uint8_t note = 0;
    uint8_t channel = 0;
    uint32_t source_track_id = 0;
    double phase = 0.0;           // [0,1)
    double base_phase_inc = 0.0;  // cycles per sample at the note pitch (no bend)
    double phase_inc = 0.0;       // effective increment incl. channel pitch bend
    float velocity = 0.0f;        // [0,1]
    float poly_pressure = 0.0f;   // per-note (poly) pressure in [0,1]
    float env = 0.0f;             // current envelope level
    Stage stage = Stage::kIdle;
    bool key_down = false;
    uint64_t age = 0;  // start order, for deterministic voice stealing
  };

  void note_on(uint8_t channel, uint8_t note, float velocity, uint32_t source_track_id) noexcept;
  void note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept;
  void sustain_pedal(uint8_t channel, bool down) noexcept;
  // MPE-style expression. Pitch bend is a per-channel 14-bit value (center 8192)
  // mapped through a fixed +/-2 semitone range; channel pressure applies to every
  // voice on the channel, poly pressure to the single matching note.
  void pitch_bend(uint8_t channel, uint16_t bend14) noexcept;
  void channel_pressure(uint8_t channel, uint8_t pressure7) noexcept;
  void poly_pressure(uint8_t channel, uint8_t note, uint8_t pressure7) noexcept;
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
    uint8_t volume = 100;      // CC7
    uint8_t pan = 64;          // CC10 (centre)
    uint8_t expression = 127;  // CC11
    float gain = 1.0f;
    rt::PanGains pan_gains{};
  };

  std::array<ChannelControls, 16> channel_controls_{};
  std::array<bool, 16> sustain_down_{};
  // Per-channel MPE expression state. Bend is stored in semitones (0 == centered);
  // pressure in [0,1]. Both default to neutral so a project that sends no
  // expression bounces bit-identically to the pre-MPE synth.
  std::array<float, 16> channel_bend_semitones_{};
  std::array<float, 16> channel_pressure_{};
  std::vector<Voice> voices_;
};

}  // namespace sonare::midi
