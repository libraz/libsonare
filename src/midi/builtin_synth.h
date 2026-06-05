#pragma once

/// @file builtin_synth.h
/// @brief Minimal polyphonic oscillator synth — a deliberately plain electronic
///        sound source so MIDI arrangements bounce to audible output instead of
///        silence. A richer literature-backed instrument bank (FM, Karplus-Strong,
///        modal) is planned separately.
///
/// This fallback intentionally implements only note on/off, sustain, channel
/// mode, volume, expression and pan. Pitch bend, RPN and NRPN are supported by
/// NativeSynth/Sf2Player, not by this minimal oscillator fallback.
///
/// RT contract (inherited from MidiInstrument):
///   - prepare() runs on the control thread and is the ONLY place that allocates
///     (it sizes the fixed voice pool).
///   - on_event() / process() run on the audio thread and are allocation-free,
///     lock-free and IO-free.
///
/// Determinism: no RNG, no wall-clock. The same project + config bounces to
/// bit-identical audio within one build (voice stealing is deterministic:
/// prefer a free voice, else the oldest voice).

#include <array>
#include <cstdint>
#include <vector>

#include "midi/instrument.h"

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
  void reset() override;
  int tail_samples() const noexcept override { return static_cast<int>(tail_samples_); }
  void on_event(uint32_t destination_id, const MidiEvent& event) noexcept override;

 private:
  enum class Stage : uint8_t { kIdle = 0, kAttack, kDecay, kSustain, kRelease };

  struct Voice {
    bool active = false;
    uint8_t note = 0;
    uint8_t channel = 0;
    double phase = 0.0;      // [0,1)
    double phase_inc = 0.0;  // cycles per sample
    float velocity = 0.0f;   // [0,1]
    float env = 0.0f;        // current envelope level
    Stage stage = Stage::kIdle;
    bool key_down = false;
    uint64_t age = 0;  // start order, for deterministic voice stealing
  };

  void note_on(uint8_t channel, uint8_t note, float velocity) noexcept;
  void note_off(uint8_t channel, uint8_t note) noexcept;
  void sustain_pedal(uint8_t channel, bool down) noexcept;
  // Channel-mode "All Notes Off" (CC#123): release every sounding voice on the
  // channel (graceful, honours the release tail). `all_sound_off` (CC#120)
  // silences them immediately, bypassing the release stage.
  void all_notes_off(uint8_t channel) noexcept;
  void all_sound_off(uint8_t channel) noexcept;
  float render_voice_sample(Voice& v) noexcept;

  BuiltinSynthConfig config_{};
  double sample_rate_ = 0.0;
  bool prepared_ = false;
  uint64_t next_age_ = 1;
  int64_t tail_samples_ = 0;

  // Per-stage per-sample envelope increments derived in prepare().
  float attack_inc_ = 1.0f;
  float decay_inc_ = 1.0f;
  float release_inc_ = 1.0f;

  std::array<bool, 16> sustain_down_{};
  std::vector<Voice> voices_;
};

}  // namespace sonare::midi
