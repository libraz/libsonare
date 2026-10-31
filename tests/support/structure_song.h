#pragma once

/// @file structure_song.h
/// @brief A 132 s synthetic pop song with a known section layout, rendered through the GM synth.
///
/// 120 BPM, 4/4, C major, 66 bars of 2 s. Sections that share a group repeat the same
/// progression, instruments and velocity, so the truth table names which sections are the same
/// material and what role each plays. The render happens once per process; the 48 kHz and
/// modulated variants derive from the same score.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/resample.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/midi_sequence_render.h"
#include "util/constants.h"
#include "util/types.h"

namespace sonare::test::structure_song {

inline constexpr int kRenderRate = 44100;
inline constexpr int kResampledRate = 48000;
inline constexpr float kBeatSec = 0.5f;
inline constexpr float kBarSec = 2.0f;
inline constexpr int kBeatsPerBar = 4;
inline constexpr int kNumBars = 66;
inline constexpr float kDurationSec = 132.0f;

/// Fraction of a note's nominal length that it sounds before its note-off.
inline constexpr double kGate = 0.95;

/// Melodic channels (0-based UMP channel index) and their GM programs (0-based).
inline constexpr uint8_t kChVibraphone = 0;     // program 11
inline constexpr uint8_t kChBass = 1;           // program 33
inline constexpr uint8_t kChElectricPiano = 2;  // program 4
inline constexpr uint8_t kChFlute = 3;          // program 73
inline constexpr uint8_t kChPad = 4;            // program 89
inline constexpr uint8_t kChStrings = 5;        // program 48
inline constexpr uint8_t kChDistGuitar = 6;     // program 30
inline constexpr uint8_t kChTrumpet = 7;        // program 56
inline constexpr uint8_t kChVoice = 8;          // program 53
inline constexpr uint8_t kChDrums = 9;

inline constexpr uint8_t kKick = 36;
inline constexpr uint8_t kSnare = 38;
inline constexpr uint8_t kClosedHat = 42;
inline constexpr uint8_t kCrash = 49;

/// One section of the song as the truth table states it.
struct TruthSection {
  const char* name;  ///< Layout name ("Intro", "Verse", ...).
  int bars;
  int group;  ///< Same group = same material.
  SectionType label;
  float start_sec;
  float end_sec;
};

namespace detail {

enum class Kind { kIntro, kVerse, kPre, kChorus, kBridge, kOutro };

struct Chord {
  int root_pc;
  bool minor;
};

inline constexpr Chord kC{0, false};
inline constexpr Chord kAm{9, true};
inline constexpr Chord kF{5, false};
inline constexpr Chord kG{7, false};
inline constexpr Chord kEm{4, true};
inline constexpr Chord kDm{2, true};

struct SectionSpec {
  const char* name;
  Kind kind;
  int group;
  SectionType label;
  int velocity;
  std::vector<Chord> chords;  ///< One chord per bar; the size is the bar count.
};

inline const std::vector<SectionSpec>& layout() {
  static const std::vector<Chord> intro = {kC, kAm, kF, kG};
  static const std::vector<Chord> verse = {kC, kG, kAm, kEm, kF, kC, kF, kG};
  static const std::vector<Chord> pre = {kDm, kEm, kF, kG};
  static const std::vector<Chord> chorus = {kF, kG, kEm, kAm, kDm, kG, kC, kC};
  static const std::vector<Chord> bridge = {kAm, kF, kC, kG, kAm, kF, kG, kG};
  static const std::vector<Chord> outro = {kC, kC};
  static const std::vector<SectionSpec> specs = {
      {"Intro", Kind::kIntro, 0, SectionType::Intro, 70, intro},
      {"Verse", Kind::kVerse, 1, SectionType::Verse, 50, verse},
      {"Pre", Kind::kPre, 2, SectionType::PreChorus, 85, pre},
      {"Chorus", Kind::kChorus, 3, SectionType::Chorus, 127, chorus},
      {"Interlude", Kind::kIntro, 0, SectionType::Instrumental, 70, intro},
      {"Verse", Kind::kVerse, 1, SectionType::Verse, 50, verse},
      {"Pre", Kind::kPre, 2, SectionType::PreChorus, 85, pre},
      {"Chorus", Kind::kChorus, 3, SectionType::Chorus, 127, chorus},
      {"Bridge", Kind::kBridge, 4, SectionType::Bridge, 90, bridge},
      {"Chorus", Kind::kChorus, 3, SectionType::Chorus, 127, chorus},
      {"Outro", Kind::kOutro, 0, SectionType::Outro, 60, outro},
  };
  return specs;
}

inline int64_t to_sample(double sec, int rate) {
  return static_cast<int64_t>(std::llround(sec * rate));
}

/// Pitch of triad degree @p degree (0 root, 1 third, 2 fifth) folded into the octave at @p base.
inline int chord_tone(const Chord& chord, int degree, int base) {
  const int intervals[3] = {0, chord.minor ? 3 : 4, 7};
  return base + (chord.root_pc + intervals[degree]) % 12;
}

/// Appends score events at a given sample rate; @c transpose shifts melodic notes only.
class Writer {
 public:
  Writer(std::vector<TimedUmp>& events, int rate) : events_(events), rate_(rate) {}

  void set_transpose(int semitones) { transpose_ = semitones; }
  void set_velocity(int velocity) { velocity_ = static_cast<uint8_t>(velocity); }

  void note(uint8_t channel, int pitch, double start_sec, double length_sec) {
    const uint8_t shifted = static_cast<uint8_t>(pitch + transpose_);
    events_.push_back({to_sample(start_sec, rate_),
                       sonare::midi::make_midi1_note_on(0, channel, shifted, velocity_)});
    events_.push_back({to_sample(start_sec + length_sec * kGate, rate_),
                       sonare::midi::make_midi1_note_off(0, channel, shifted, 0)});
  }

  void drum(uint8_t pitch, double start_sec) {
    const int saved = transpose_;
    transpose_ = 0;
    note(kChDrums, pitch, start_sec, 0.1);
    transpose_ = saved;
  }

  void triad(uint8_t channel, const Chord& chord, int base, double start_sec, double length_sec) {
    for (int degree = 0; degree < 3; ++degree) {
      note(channel, chord_tone(chord, degree, base), start_sec, length_sec);
    }
  }

  void program(uint8_t channel, uint8_t program) {
    events_.push_back({0, sonare::midi::make_midi1_program_change(0, channel, program)});
  }

 private:
  std::vector<TimedUmp>& events_;
  int rate_;
  int transpose_ = 0;
  uint8_t velocity_ = 80;
};

inline void write_bar(Writer& w, Kind kind, int bar, int bars, const Chord& chord, double t) {
  const int bass = 36 + chord.root_pc;
  const double beat = kBeatSec;
  if (kind == Kind::kOutro && bar == bars - 1) {
    w.note(kChBass, bass, t, 2.0);
  } else {
    w.note(kChBass, bass, t, 1.0);
    w.note(kChBass, bass, t + 1.0, 1.0);
  }
  switch (kind) {
    case Kind::kIntro:
    case Kind::kOutro: {
      if (kind == Kind::kOutro && bar == bars - 1) {
        w.triad(kChVibraphone, chord, 60, t, 2.0);
        break;
      }
      const int degrees[8] = {0, 1, 2, 0, 2, 1, 2, 1};
      for (int i = 0; i < 8; ++i) {
        const int pitch = chord_tone(chord, degrees[i], 60) + (i == 3 ? 12 : 0);
        w.note(kChVibraphone, pitch, t + i * beat / 2, beat / 2);
      }
      break;
    }
    case Kind::kVerse: {
      const int melody[4] = {1, 2, 1, 0};
      for (int i = 0; i < 4; ++i) {
        w.triad(kChElectricPiano, chord, 60, t + i * beat, beat);
        w.note(kChFlute, chord_tone(chord, melody[i], 72), t + i * beat, beat);
      }
      w.drum(kKick, t);
      w.drum(kKick, t + 2 * beat);
      for (int i = 0; i < 8; ++i) w.drum(kClosedHat, t + i * beat / 2);
      break;
    }
    case Kind::kPre: {
      w.triad(kChPad, chord, 60, t, 2.0);
      for (int i = 0; i < 16; ++i) w.drum(kClosedHat, t + i * beat / 4);
      break;
    }
    case Kind::kChorus: {
      const int melody[4] = {2, 1, 2, 0};
      w.triad(kChStrings, chord, 60, t, 2.0);
      for (int i = 0; i < 4; ++i) {
        w.triad(kChDistGuitar, chord, 48, t + i * beat, beat);
        w.note(kChTrumpet, chord_tone(chord, melody[i], 72), t + i * beat, beat);
      }
      w.drum(kKick, t);
      w.drum(kKick, t + 2 * beat);
      w.drum(kSnare, t + beat);
      w.drum(kSnare, t + 3 * beat);
      for (int i = 0; i < 8; ++i) w.drum(kClosedHat, t + i * beat / 2);
      w.drum(kCrash, t);
      break;
    }
    case Kind::kBridge: {
      const int melody[4] = {0, 1, 2, 1};
      w.triad(kChPad, chord, 60, t, 2.0);
      for (int i = 0; i < 4; ++i) {
        w.note(kChVoice, chord_tone(chord, melody[i], 60), t + i * beat, beat);
        w.drum(kKick, t + i * beat);
      }
      break;
    }
  }
}

}  // namespace detail

/// The truth table: one entry per section, in time order.
inline const std::vector<TruthSection>& truth() {
  static const std::vector<TruthSection> table = [] {
    std::vector<TruthSection> out;
    float start = 0.0f;
    for (const auto& spec : detail::layout()) {
      const int bars = static_cast<int>(spec.chords.size());
      const float end = start + static_cast<float>(bars) * kBarSec;
      out.push_back({spec.name, bars, spec.group, spec.label, start, end});
      start = end;
    }
    return out;
  }();
  return table;
}

/// True internal boundaries in seconds (every section start except the first).
inline std::vector<float> true_boundaries() {
  std::vector<float> out;
  const auto& table = truth();
  for (std::size_t i = 1; i < table.size(); ++i) out.push_back(table[i].start_sec);
  return out;
}

/// Builds the score as timed UMP events at @p rate.
/// @param modulate_last_chorus_semitones Shift of the last Chorus' melodic notes (drums stay).
inline std::vector<TimedUmp> build_events(int rate = kRenderRate,
                                          int modulate_last_chorus_semitones = 0) {
  std::vector<TimedUmp> events;
  detail::Writer w(events, rate);
  const uint8_t programs[9] = {11, 33, 4, 73, 89, 48, 30, 56, 53};
  for (uint8_t channel = 0; channel < 9; ++channel) w.program(channel, programs[channel]);

  const auto& specs = detail::layout();
  std::size_t last_chorus = 0;
  for (std::size_t i = 0; i < specs.size(); ++i) {
    if (specs[i].kind == detail::Kind::kChorus) last_chorus = i;
  }
  const auto& table = truth();
  for (std::size_t i = 0; i < specs.size(); ++i) {
    w.set_velocity(specs[i].velocity);
    w.set_transpose(i == last_chorus ? modulate_last_chorus_semitones : 0);
    const int bars = static_cast<int>(specs[i].chords.size());
    for (int bar = 0; bar < bars; ++bar) {
      const double t = table[i].start_sec + bar * static_cast<double>(kBarSec);
      detail::write_bar(w, specs[i].kind, bar, bars, specs[i].chords[static_cast<std::size_t>(bar)],
                        t);
    }
  }
  return events;
}

namespace detail {

inline Audio render(int semitones) {
  sonare::midi::synth::NativeSynthConfig config;
  config.use_gm_programs = true;
  const int samples = static_cast<int>(kDurationSec * kRenderRate);
  return Audio::from_vector(
      render_sequence_mono(build_events(kRenderRate, semitones), kRenderRate, samples, config),
      kRenderRate);
}

}  // namespace detail

/// The song at 44100 Hz, rendered once per process.
inline const Audio& rendered_audio() {
  static const Audio audio = detail::render(0);
  return audio;
}

/// The same render resampled to 48000 Hz (not rendered separately).
inline const Audio& resampled_audio() {
  static const Audio audio = sonare::resample(rendered_audio(), kResampledRate);
  return audio;
}

/// The song with the last Chorus a semitone up, at 44100 Hz, rendered once per process.
inline const Audio& modulated_audio() {
  static const Audio audio = detail::render(1);
  return audio;
}

/// A constant 220 Hz tone with four short percussive attacks at 4.0, 4.5, 5.0 and 5.5 s.
inline Audio steady_tone_four_onsets(int rate = kRenderRate) {
  constexpr double kSeconds = 16.0;
  constexpr double kToneHz = 220.0;
  constexpr double kBurstHz = 1000.0;
  constexpr double kBurstDecaySec = 0.02;
  constexpr double kBurstSec = 0.1;
  const double onsets[4] = {4.0, 4.5, 5.0, 5.5};
  std::vector<float> samples(static_cast<std::size_t>(kSeconds * rate));
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double t = static_cast<double>(i) / rate;
    double v = 0.1 * std::sin(constants::kTwoPiD * kToneHz * t);
    for (const double onset : onsets) {
      const double dt = t - onset;
      if (dt >= 0.0 && dt < kBurstSec) {
        v += 0.6 * std::exp(-dt / kBurstDecaySec) * std::sin(constants::kTwoPiD * kBurstHz * dt);
      }
    }
    samples[i] = static_cast<float>(v);
  }
  return Audio::from_vector(std::move(samples), rate);
}

}  // namespace sonare::test::structure_song
