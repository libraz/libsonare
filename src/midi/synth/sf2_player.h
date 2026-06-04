#pragma once

/// @file sf2_player.h
/// @brief Multitimbral (16-part) SoundFont 2 player implementing
///        MidiInstrument: (bank, program) -> preset resolution, layered
///        preset/instrument zone matching, the shared voice pool with
///        deterministic stealing, and BuiltinSynth-compatible channel-mode CC
///        semantics (CC64 sustain, CC120/121/123).
///
/// One Sf2Player instance receives all 16 MIDI channels (GS multitimbral
/// convention); channel 10 (index 9) resolves percussion via bank 128.
///
/// RT contract (MidiInstrument): set_soundfont() and prepare() run on the
/// CONTROL thread and are the only allocating calls; on_event()/process()
/// are allocation-free, lock-free and IO-free. The Sf2File is shared
/// read-only with the audio thread.
///
/// Determinism: no RNG, no wall clock; voice stealing and rendering are
/// bit-identical for identical event streams within one build.

#include <array>
#include <cstdint>
#include <memory>

#include "midi/instrument.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_voice.h"
#include "midi/synth/voice_pool.h"

namespace sonare::midi::synth {

struct Sf2PlayerConfig {
  /// Master output gain applied to the summed voices (linear).
  float gain = 0.5f;
  /// Voice pool size. GS playback layers zones across 16 parts + drums, so the
  /// default is far above BuiltinSynth's 16 (clamped to [1, kMaxSynthVoices]).
  int polyphony = 48;
};

class Sf2Player final : public MidiInstrument {
 public:
  explicit Sf2Player(const Sf2PlayerConfig& config = {}) noexcept;

  /// CONTROL thread: attach a parsed SoundFont. May be called before or after
  /// prepare(), but never concurrently with the audio thread.
  void set_soundfont(std::shared_ptr<const Sf2File> soundfont);

  const Sf2File* soundfont() const noexcept { return soundfont_.get(); }

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int tail_samples() const noexcept override { return static_cast<int>(tail_samples_); }
  void on_event(uint32_t destination_id, const MidiEvent& event) noexcept override;

  /// Currently sounding voices (test/diagnostic).
  int active_voice_count() const noexcept { return pool_.active_count(); }

 private:
  struct ChannelState {
    uint8_t program = 0;
    uint8_t bank_msb = 0;  // CC0; GS variation bank select
    uint8_t bank_lsb = 0;  // CC32
    bool sustain = false;
    // Default-modulator controller state.
    uint8_t volume = 100;      // CC7
    uint8_t expression = 127;  // CC11
    uint8_t pan = 64;          // CC10
    uint8_t mod_wheel = 0;     // CC1
    uint8_t reverb_send = 40;  // CC91 (GS power-on default)
    uint8_t chorus_send = 0;   // CC93
    uint16_t pitch_bend = 8192;
    // RPN state (bend range via RPN 0; default 2 semitones).
    uint8_t rpn_msb = 127;
    uint8_t rpn_lsb = 127;
    float bend_range_cents = 200.0f;
  };

  void note_on(uint8_t channel, uint8_t note, uint8_t velocity) noexcept;
  void note_off(uint8_t channel, uint8_t note) noexcept;
  void control_change(uint8_t channel, uint8_t controller, uint8_t value) noexcept;
  void sustain_pedal(uint8_t channel, bool down) noexcept;
  void all_notes_off(uint8_t channel) noexcept;
  void all_sound_off(uint8_t channel) noexcept;
  void reset_controllers(uint8_t channel) noexcept;
  /// Recompute the cached Sf2ChannelMod for @p channel after a CC/bend change.
  void refresh_channel_mod(uint8_t channel) noexcept;
  /// Effective SF2 bank for a channel (drum channel -> 128).
  uint16_t effective_bank(uint8_t channel) const noexcept;
  /// Preset index for (bank, program) with GS-style fallbacks, or -1.
  int resolve_preset(uint16_t bank, uint8_t program) const noexcept;

  Sf2PlayerConfig config_{};
  std::shared_ptr<const Sf2File> soundfont_;
  double sample_rate_ = 0.0;
  bool prepared_ = false;
  int64_t tail_samples_ = 0;
  /// Longest release timecents found in the soundfont (set_soundfont scan).
  int32_t max_release_timecents_ = -12000;

  std::array<ChannelState, 16> channels_{};
  std::array<Sf2ChannelMod, 16> channel_mods_{};
  VoicePool<Sf2Voice> pool_;
};

}  // namespace sonare::midi::synth
