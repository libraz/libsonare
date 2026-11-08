/// @file note_transcriber_test.cpp
/// @brief Tests for the audio-to-note bridge: the sequence a synthesized take
///        transcribes back to, and the two conversions the bridge owns on top of
///        the chains it reads -- Hz to a MIDI note number, and a measured level
///        to a velocity.
///
/// The round-trip case is the headline, and it is stated against material whose
/// note numbers are known by construction: equal-tempered sines at exact
/// frequencies, separated by silence so the segmenter has an onset to cut on. A
/// second sequence one whole tone away runs beside it, so the file records that
/// the expectation discriminates rather than that it merely held once.

#include "editing/note_model/note_transcriber.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "editing/note_model/monophonic_onset_evidence.h"
#include "editing/pitch_editor/f0_provider.h"
#include "editing/polyphony/f0_salience.h"
#include "feature/pitch.h"
#include "support/note_match.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/json_reader.h"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace sonare::editing::note_model;

namespace {

constexpr int kSampleRate = 22050;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/// Fixed tolerance for the sequence's measured onset. Keep this independent of
/// tracker cadence so rate-specific settings do not change the expectation.
constexpr int64_t kOnsetToleranceSamples = 1024;

/// The default floor, and one shallow enough to move every measured velocity.
constexpr float kDeepFloorDb = -48.0f;
constexpr float kShallowFloorDb = -12.0f;

constexpr int kToneSamples = kSampleRate / 4;       // 0.25 s
constexpr int kGapSamples = kSampleRate * 6 / 100;  // 0.06 s
constexpr float kEdgeSamples = 0.005f * kSampleRate;

float hz_for_midi(int note) {
  return sonare::constants::kA4Hz *
         std::pow(2.0f, static_cast<float>(note - 69) / sonare::constants::kSemitonesPerOctave);
}

/// @brief A tone with raised-cosine edges, so the level rises into the note
///        rather than stepping into it.
void append_tone(std::vector<float>& into, float hz, float amplitude, int samples) {
  for (int i = 0; i < samples; ++i) {
    const float from_start = static_cast<float>(i);
    const float from_end = static_cast<float>(samples - 1 - i);
    const float edge = std::min(1.0f, std::min(from_start, from_end) / kEdgeSamples);
    const float envelope = 0.5f - 0.5f * std::cos(sonare::constants::kTwoPi * 0.5f * edge);
    into.push_back(amplitude * envelope *
                   static_cast<float>(std::sin(sonare::constants::kTwoPiD * hz *
                                               static_cast<double>(i) / kSampleRate)));
  }
}

void append_silence(std::vector<float>& into, int samples) {
  into.insert(into.end(), static_cast<size_t>(samples), 0.0f);
}

/// @brief A take whose note numbers and onsets are known by construction.
struct Sequence {
  std::vector<float> samples;
  std::vector<int> notes;
  std::vector<int64_t> onsets;

  sonare::Audio audio() const {
    return sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate);
  }
};

/// @brief @p midi_notes as separated tones, after a lead-in silence so an onset
///        of 0 is wrong for every note rather than right for the first.
Sequence sequence_of(const std::vector<int>& midi_notes, float amplitude = 0.5f) {
  Sequence sequence;
  append_silence(sequence.samples, kGapSamples);
  for (const int note : midi_notes) {
    sequence.onsets.push_back(static_cast<int64_t>(sequence.samples.size()));
    sequence.notes.push_back(note);
    append_tone(sequence.samples, hz_for_midi(note), amplitude, kToneSamples);
    append_silence(sequence.samples, kGapSamples);
  }
  return sequence;
}

/// @brief One sustained tone, for the cases that are about a config field rather
///        than about segmentation.
sonare::Audio sustained(int midi_note, float amplitude = 0.5f) {
  std::vector<float> samples;
  append_tone(samples, hz_for_midi(midi_note), amplitude, kSampleRate / 2);
  return sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate);
}

struct RateSequence {
  std::vector<float> samples;
  std::vector<int> notes;
  std::vector<int64_t> onsets;

  sonare::Audio audio(int sample_rate) const {
    return sonare::Audio::from_buffer(samples.data(), samples.size(), sample_rate);
  }
};

/// A short, legato arpeggio rendered at the requested input rate.
RateSequence fast_arpeggio(int sample_rate) {
  constexpr double kLeadSeconds = 0.2;
  constexpr double kToneSeconds = 0.125;
  constexpr double kAttackSeconds = 0.004;
  constexpr double kReleaseSeconds = 0.008;
  const std::vector<int> intended{60, 64, 67, 72, 69, 65, 62, 71};
  RateSequence sequence;
  sequence.notes = intended;
  sequence.samples.resize(static_cast<size_t>(std::lround(kLeadSeconds * sample_rate)), 0.0f);
  for (const int note : intended) {
    const int tone_samples = static_cast<int>(std::lround(kToneSeconds * sample_rate));
    const float hz = hz_for_midi(note);
    sequence.onsets.push_back(static_cast<int64_t>(sequence.samples.size()));
    for (int i = 0; i < tone_samples; ++i) {
      const double t = static_cast<double>(i) / sample_rate;
      const double envelope =
          std::min({1.0, t / kAttackSeconds, std::max(0.0, kToneSeconds - t) / kReleaseSeconds});
      double value = 0.0;
      for (int harmonic = 1; harmonic <= 5; ++harmonic) {
        value += std::sin(sonare::constants::kTwoPiD * hz * harmonic * t) / harmonic;
      }
      sequence.samples.push_back(static_cast<float>(0.18 * envelope * value));
    }
  }
  return sequence;
}

RateSequence contiguous_harmonic_notes(int sample_rate, const std::vector<int>& notes) {
  constexpr double kLeadSeconds = 0.2;
  constexpr double kToneSeconds = 0.125;
  constexpr double kTailSeconds = 0.2;
  constexpr double kAttackSeconds = 0.004;
  constexpr double kReleaseSeconds = 0.008;
  const int64_t lead = static_cast<int64_t>(std::llround(kLeadSeconds * sample_rate));
  const int64_t tone = static_cast<int64_t>(std::llround(kToneSeconds * sample_rate));
  const int64_t tail = static_cast<int64_t>(std::llround(kTailSeconds * sample_rate));
  RateSequence sequence;
  sequence.notes = notes;
  sequence.samples.assign(
      static_cast<size_t>(lead + tone * static_cast<int64_t>(notes.size()) + tail), 0.0f);
  for (size_t note_index = 0; note_index < notes.size(); ++note_index) {
    const int64_t start = lead + static_cast<int64_t>(note_index) * tone;
    sequence.onsets.push_back(start);
    const double fundamental_hz = hz_for_midi(notes[note_index]);
    double phase = 0.0;
    for (int64_t offset = 0; offset < tone; ++offset) {
      const double t = static_cast<double>(offset) / sample_rate;
      const double duration = static_cast<double>(tone) / sample_rate;
      const double envelope =
          std::min({1.0, t / kAttackSeconds, std::max(0.0, duration - t) / kReleaseSeconds});
      phase += sonare::constants::kTwoPiD * fundamental_hz / sample_rate;
      double value = 0.0;
      for (int harmonic = 1; harmonic <= 5; ++harmonic) {
        value += std::sin(harmonic * phase) / harmonic;
      }
      sequence.samples[static_cast<size_t>(start + offset)] =
          static_cast<float>(0.18 * envelope * value);
    }
  }
  return sequence;
}

sonare::Audio gliding_harmonic_tone(int sample_rate) {
  constexpr double kLeadSeconds = 0.2;
  constexpr double kGlideSeconds = 1.6;
  constexpr double kTailSeconds = 0.2;
  constexpr double kAttackSeconds = 0.004;
  constexpr double kReleaseSeconds = 0.008;
  const int64_t start = static_cast<int64_t>(std::llround(kLeadSeconds * sample_rate));
  const int64_t end =
      static_cast<int64_t>(std::llround((kLeadSeconds + kGlideSeconds) * sample_rate));
  const int64_t total = static_cast<int64_t>(
      std::llround((kLeadSeconds + kGlideSeconds + kTailSeconds) * sample_rate));
  std::vector<float> samples(static_cast<size_t>(total), 0.0f);
  const double initial_hz = hz_for_midi(57);
  double phase = 0.0;
  for (int64_t sample = start; sample < end; ++sample) {
    const double t = static_cast<double>(sample - start) / sample_rate;
    const double envelope =
        std::min({1.0, t / kAttackSeconds, std::max(0.0, kGlideSeconds - t) / kReleaseSeconds});
    const double frequency = initial_hz * std::pow(2.0, 300.0 * t / (kGlideSeconds * 1200.0));
    phase += sonare::constants::kTwoPiD * frequency / sample_rate;
    double value = 0.0;
    for (int harmonic = 1; harmonic <= 5; ++harmonic) {
      value += std::sin(harmonic * phase) / harmonic;
    }
    samples[static_cast<size_t>(sample)] = static_cast<float>(0.18 * envelope * value);
  }
  return sonare::Audio::from_buffer(samples.data(), samples.size(), sample_rate);
}

/// A contiguous leap phrase that exposes pYIN octave errors after abrupt pitch
/// changes without changing the known note onsets.
RateSequence alias_arpeggio(int sample_rate) {
  constexpr double kLeadSeconds = 0.2;
  constexpr double kToneSeconds = 0.125;
  constexpr double kAttackSeconds = 0.004;
  constexpr double kReleaseSeconds = 0.008;
  const std::vector<int> intended{57, 62, 67, 69, 71, 67, 74, 67, 76, 67, 77, 67};
  RateSequence sequence;
  sequence.notes = intended;
  sequence.samples.resize(
      static_cast<size_t>(std::llround((0.4 + intended.size() * kToneSeconds) * sample_rate)),
      0.0f);
  for (size_t index = 0; index < intended.size(); ++index) {
    const int64_t start =
        static_cast<int64_t>(std::llround((kLeadSeconds + index * kToneSeconds) * sample_rate));
    const int64_t end = static_cast<int64_t>(
        std::llround((kLeadSeconds + (index + 1) * kToneSeconds) * sample_rate));
    sequence.onsets.push_back(start);
    const double fundamental_hz = hz_for_midi(intended[index]);
    double phase = 0.0;
    for (int64_t sample = start; sample < end; ++sample) {
      const double t = static_cast<double>(sample - start) / sample_rate;
      const double envelope =
          std::min({1.0, t / kAttackSeconds,
                    (static_cast<double>(end - sample) / sample_rate) / kReleaseSeconds});
      phase += sonare::constants::kTwoPiD * fundamental_hz / sample_rate;
      double value = 0.0;
      for (int harmonic = 1; harmonic <= 5; ++harmonic) {
        value += std::sin(harmonic * phase) / harmonic;
      }
      sequence.samples[static_cast<size_t>(sample)] = static_cast<float>(0.18 * envelope * value);
    }
  }
  return sequence;
}

sonare::Audio low_fundamental_harmonic_tone(int sample_rate, double fundamental_gain,
                                            double second_harmonic_gain,
                                            double third_harmonic_gain = 0.0) {
  constexpr double kLeadSeconds = 0.2;
  constexpr double kHeldSeconds = 1.6;
  constexpr double kTailSeconds = 0.2;
  constexpr double kAttackSeconds = 0.004;
  constexpr double kReleaseSeconds = 0.008;
  const int64_t start = static_cast<int64_t>(std::llround(kLeadSeconds * sample_rate));
  const int64_t end =
      static_cast<int64_t>(std::llround((kLeadSeconds + kHeldSeconds) * sample_rate));
  const int64_t total = static_cast<int64_t>(
      std::llround((kLeadSeconds + kHeldSeconds + kTailSeconds) * sample_rate));
  std::vector<float> samples(static_cast<size_t>(total), 0.0f);
  const double fundamental_hz = hz_for_midi(45);
  double phase = 0.0;
  for (int64_t sample = start; sample < end; ++sample) {
    const double t = static_cast<double>(sample - start) / sample_rate;
    const double envelope =
        std::min({1.0, t / kAttackSeconds,
                  (static_cast<double>(end - sample) / sample_rate) / kReleaseSeconds});
    phase += sonare::constants::kTwoPiD * fundamental_hz / sample_rate;
    const double value = fundamental_gain * std::sin(phase) +
                         second_harmonic_gain * std::sin(2.0 * phase) +
                         third_harmonic_gain * std::sin(3.0 * phase);
    samples[static_cast<size_t>(sample)] = static_cast<float>(0.18 * envelope * value);
  }
  return sonare::Audio::from_buffer(samples.data(), samples.size(), sample_rate);
}

RateSequence repeated_harmonic_tone(int sample_rate, int midi_note,
                                    const std::vector<float>& gains) {
  constexpr double kLeadSeconds = 0.2;
  constexpr double kPeriodSeconds = 0.125;
  constexpr double kTailSeconds = 0.2;
  constexpr double kAttackSeconds = 0.004;
  constexpr double kReleaseSeconds = 0.008;
  RateSequence sequence;
  sequence.notes.assign(gains.size(), midi_note);
  const int64_t sample_count = static_cast<int64_t>(
      std::llround((kLeadSeconds + gains.size() * kPeriodSeconds + kTailSeconds) * sample_rate));
  sequence.samples.resize(static_cast<size_t>(sample_count), 0.0f);
  const double fundamental_hz = hz_for_midi(midi_note);
  for (size_t attack = 0; attack < gains.size(); ++attack) {
    const int64_t start =
        static_cast<int64_t>(std::llround((kLeadSeconds + attack * kPeriodSeconds) * sample_rate));
    const int64_t end = static_cast<int64_t>(
        std::llround((kLeadSeconds + (attack + 1) * kPeriodSeconds) * sample_rate));
    const double duration_seconds = static_cast<double>(end - start) / sample_rate;
    sequence.onsets.push_back(start);
    for (int64_t sample = start; sample < end; ++sample) {
      const double t = static_cast<double>(sample - start) / sample_rate;
      const double envelope = std::min(
          {1.0, t / kAttackSeconds, std::max(0.0, duration_seconds - t) / kReleaseSeconds});
      double value = 0.0;
      for (int harmonic = 1; harmonic <= 5; ++harmonic) {
        value += std::sin(sonare::constants::kTwoPiD * fundamental_hz * harmonic * t) / harmonic;
      }
      sequence.samples[static_cast<size_t>(sample)] +=
          static_cast<float>(gains[attack] * envelope * value);
    }
  }
  return sequence;
}

RateSequence repeated_harmonic_tone(int sample_rate, int midi_note, float gain) {
  return repeated_harmonic_tone(sample_rate, midi_note, std::vector<float>(4, gain));
}

sonare::Audio modulated_harmonic_tone(int sample_rate, int midi_note, double amplitude_rate_hz,
                                      double amplitude_depth, double vibrato_rate_hz = 0.0,
                                      double vibrato_depth_cents = 0.0) {
  constexpr double kLeadSeconds = 0.2;
  constexpr double kHeldSeconds = 1.6;
  constexpr double kTailSeconds = 0.2;
  constexpr double kAttackSeconds = 0.004;
  constexpr double kReleaseSeconds = 0.008;
  constexpr float kGain = 0.18f;
  const int64_t start = static_cast<int64_t>(std::llround(kLeadSeconds * sample_rate));
  const int64_t end =
      static_cast<int64_t>(std::llround((kLeadSeconds + kHeldSeconds) * sample_rate));
  const int64_t total = static_cast<int64_t>(
      std::llround((kLeadSeconds + kHeldSeconds + kTailSeconds) * sample_rate));
  std::vector<float> samples(static_cast<size_t>(total), 0.0f);
  const double duration_seconds = static_cast<double>(end - start) / sample_rate;
  const double fundamental_hz = hz_for_midi(midi_note);
  double phase = 0.0;
  for (int64_t sample = start; sample < end; ++sample) {
    const double t = static_cast<double>(sample - start) / sample_rate;
    const double edge =
        std::min({1.0, t / kAttackSeconds, std::max(0.0, duration_seconds - t) / kReleaseSeconds});
    const double amplitude =
        1.0 - amplitude_depth * 0.5 *
                  (1.0 + std::cos(sonare::constants::kTwoPiD * amplitude_rate_hz * t));
    const double frequency =
        fundamental_hz *
        std::pow(2.0, vibrato_depth_cents *
                          std::sin(sonare::constants::kTwoPiD * vibrato_rate_hz * t) /
                          sonare::constants::kCentsPerOctave);
    phase += sonare::constants::kTwoPiD * frequency / sample_rate;
    double value = 0.0;
    for (int harmonic = 1; harmonic <= 5; ++harmonic) {
      value += std::sin(harmonic * phase) / harmonic;
    }
    samples[static_cast<size_t>(sample)] = kGain * static_cast<float>(edge * amplitude * value);
  }
  return sonare::Audio::from_buffer(samples.data(), samples.size(), sample_rate);
}

sonare::Audio noisy_harmonic_tone(int sample_rate, int midi_note, float noise_peak) {
  constexpr double kLeadSeconds = 0.2;
  constexpr double kHeldSeconds = 1.6;
  constexpr double kTailSeconds = 0.2;
  constexpr double kAttackSeconds = 0.004;
  constexpr double kReleaseSeconds = 0.008;
  constexpr float kGain = 0.18f;
  const int64_t start = static_cast<int64_t>(std::llround(kLeadSeconds * sample_rate));
  const int64_t end =
      static_cast<int64_t>(std::llround((kLeadSeconds + kHeldSeconds) * sample_rate));
  const int64_t total = static_cast<int64_t>(
      std::llround((kLeadSeconds + kHeldSeconds + kTailSeconds) * sample_rate));
  std::vector<float> samples(static_cast<size_t>(total), 0.0f);
  const double duration_seconds = static_cast<double>(end - start) / sample_rate;
  const double fundamental_hz = hz_for_midi(midi_note);
  double phase = 0.0;
  uint32_t state = 0x6d2b79f5u;
  for (int64_t sample = start; sample < end; ++sample) {
    const double t = static_cast<double>(sample - start) / sample_rate;
    const double envelope =
        std::min({1.0, t / kAttackSeconds, std::max(0.0, duration_seconds - t) / kReleaseSeconds});
    phase += sonare::constants::kTwoPiD * fundamental_hz / sample_rate;
    double value = 0.0;
    for (int harmonic = 1; harmonic <= 5; ++harmonic) {
      value += std::sin(harmonic * phase) / harmonic;
    }
    state = 1664525u * state + 1013904223u;
    const float unit = static_cast<float>(state >> 8) / 16777215.0f;
    const float noise = (2.0f * unit - 1.0f) * noise_peak;
    samples[static_cast<size_t>(sample)] = envelope * (kGain * static_cast<float>(value) + noise);
  }
  return sonare::Audio::from_buffer(samples.data(), samples.size(), sample_rate);
}

struct BachReferenceNote {
  int pitch = 0;
  int64_t onset_sample = 0;
  int64_t offset_sample = 0;
};

struct BachExcerpt {
  const char* work;
  double first_quarter;
  double end_quarter;
  int tempo_bpm;
};

std::vector<BachReferenceNote> bach_excerpt(const sonare::test::JsonValue& root,
                                            const BachExcerpt& excerpt, int sample_rate) {
  REQUIRE(root.contains("tracks"));
  const auto& tracks = root["tracks"].as_array();
  REQUIRE(!tracks.empty());
  REQUIRE(tracks[0].contains("notes"));
  const auto& notes = tracks[0]["notes"].as_array();
  std::vector<BachReferenceNote> selected;
  constexpr double kLeadSeconds = 0.2;
  for (const auto& note : notes) {
    const double onset_quarters = note["onset"].as_number();
    if (onset_quarters < excerpt.first_quarter || onset_quarters >= excerpt.end_quarter) continue;
    const double duration_quarters = note["duration"].as_number();
    const double onset_seconds =
        kLeadSeconds + (onset_quarters - excerpt.first_quarter) * 60.0 / excerpt.tempo_bpm;
    const double offset_seconds =
        kLeadSeconds +
        (onset_quarters - excerpt.first_quarter + duration_quarters) * 60.0 / excerpt.tempo_bpm;
    const int64_t onset_sample = static_cast<int64_t>(std::llround(onset_seconds * sample_rate));
    const int64_t offset_sample = static_cast<int64_t>(std::llround(offset_seconds * sample_rate));
    selected.push_back({note["pitch"].as_int(), onset_sample, offset_sample});
  }
  return selected;
}

sonare::Audio render_bach_excerpt(const std::vector<BachReferenceNote>& notes, int sample_rate) {
  constexpr double kAttackSeconds = 0.004;
  constexpr double kReleaseSeconds = 0.008;
  constexpr double kTailSeconds = 0.2;
  REQUIRE(!notes.empty());
  int64_t sample_count = notes.front().offset_sample;
  for (const auto& note : notes) sample_count = std::max(sample_count, note.offset_sample);
  sample_count += static_cast<int64_t>(std::llround(kTailSeconds * sample_rate));
  std::vector<float> samples(static_cast<size_t>(sample_count), 0.0f);
  for (const auto& note : notes) {
    const double duration_seconds =
        static_cast<double>(note.offset_sample - note.onset_sample) / sample_rate;
    const double fundamental_hz = hz_for_midi(note.pitch);
    for (int64_t sample = note.onset_sample; sample < note.offset_sample; ++sample) {
      const double t = static_cast<double>(sample - note.onset_sample) / sample_rate;
      const double envelope = std::min(
          {1.0, t / kAttackSeconds, std::max(0.0, duration_seconds - t) / kReleaseSeconds});
      double value = 0.0;
      for (int harmonic = 1; harmonic <= 5; ++harmonic) {
        value += std::sin(sonare::constants::kTwoPiD * fundamental_hz * harmonic * t) / harmonic;
      }
      samples[static_cast<size_t>(sample)] += static_cast<float>(0.18 * envelope * value);
    }
  }
  return sonare::Audio::from_buffer(samples.data(), samples.size(), sample_rate);
}

sonare::test::NoteMatchCounts score_bach_notes(const std::vector<BachReferenceNote>& truth,
                                               const std::vector<TranscribedNote>& estimates,
                                               int sample_rate) {
  std::vector<sonare::test::EvalNote> reference;
  reference.reserve(truth.size());
  for (const BachReferenceNote& note : truth) {
    reference.push_back({static_cast<double>(note.onset_sample) / sample_rate,
                         static_cast<double>(note.offset_sample) / sample_rate,
                         static_cast<double>(note.pitch)});
  }
  // Integer note numbers on both sides, so the 50-cent criterion requires the same MIDI note.
  std::vector<sonare::test::EvalNote> estimated;
  estimated.reserve(estimates.size());
  for (const TranscribedNote& note : estimates) {
    estimated.push_back({static_cast<double>(note.onset_sample) / sample_rate,
                         static_cast<double>(note.offset_sample) / sample_rate,
                         static_cast<double>(note.note)});
  }
  return sonare::test::match_notes(reference, estimated, sonare::test::NoteMatchCriteria{});
}

/// @brief The note numbers, as ints so a failed comparison prints numbers.
std::vector<int> note_numbers(const std::vector<TranscribedNote>& notes) {
  std::vector<int> numbers;
  numbers.reserve(notes.size());
  for (const TranscribedNote& note : notes) numbers.push_back(note.note);
  return numbers;
}

/// @brief Every field of a transcribed note, so "the same notes" is one call.
void require_same_notes(const std::vector<TranscribedNote>& actual,
                        const std::vector<TranscribedNote>& expected) {
  REQUIRE(actual.size() == expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    INFO("note " << i);
    REQUIRE(actual[i].onset_sample == expected[i].onset_sample);
    REQUIRE(actual[i].offset_sample == expected[i].offset_sample);
    REQUIRE(actual[i].note == expected[i].note);
    REQUIRE(actual[i].velocity == expected[i].velocity);
    REQUIRE(actual[i].median_hz == expected[i].median_hz);
  }
}

/// @brief The linear peak RMS a decibel level reads as.
float linear_of(float db) { return std::pow(10.0f, db / 20.0f); }

}  // namespace

// --- Round trip on synthesized material -----------------------------------

TEST_CASE(
    "transcribe_notes returns the sequence a take was built from, and says so of one take only",
    "[note_transcriber]") {
  const Sequence intended = sequence_of({60, 64, 67, 72});
  const std::vector<TranscribedNote> notes = transcribe_notes(intended.audio());

  INFO("transcribed " << notes.size() << " notes");
  REQUIRE(notes.size() == intended.notes.size());
  REQUIRE(note_numbers(notes) == intended.notes);

  for (size_t i = 0; i < notes.size(); ++i) {
    INFO("note " << i << ": onset " << notes[i].onset_sample << ", intended " << intended.onsets[i]
                 << ", tolerance " << kOnsetToleranceSamples << " samples at " << kSampleRate
                 << " Hz");
    CHECK(std::llabs(notes[i].onset_sample - intended.onsets[i]) <= kOnsetToleranceSamples);
    CHECK(notes[i].offset_sample > notes[i].onset_sample);
    // The pitch the note number was rounded from, so a right number reached by a
    // wrong measurement is still a failure.
    CHECK_THAT(notes[i].median_hz, WithinRel(hz_for_midi(intended.notes[i]), 0.02f));
    // -9 dBFS of peak RMS against the -48 dB floor is roughly velocity 103; a
    // measurement that collapsed to the floor would land at 1.
    INFO("velocity " << static_cast<int>(notes[i].velocity));
    CHECK(notes[i].velocity > 64);
    CHECK(notes[i].velocity <= 127);
  }

  SECTION("a different sequence is a different answer") {
    // The control. Without it the expectation above is compatible with a
    // transcriber that answers with a fixed sequence, or with one whose note
    // numbers come from the fixture rather than from the audio.
    const Sequence other = sequence_of({62, 65, 69, 74});
    const std::vector<TranscribedNote> other_notes = transcribe_notes(other.audio());
    REQUIRE(note_numbers(other_notes) == other.notes);
    REQUIRE(note_numbers(other_notes) != intended.notes);
  }
}

TEST_CASE("default monophonic transcription follows a fast arpeggio across input rates",
          "[note_transcriber][pitch_cadence]") {
  const std::vector<int> expected{60, 64, 67, 72, 69, 65, 62, 71};
  for (const int sample_rate : {16000, 22050, 44100}) {
    const RateSequence sequence = fast_arpeggio(sample_rate);
    const std::vector<TranscribedNote> notes = transcribe_notes(sequence.audio(sample_rate));
    INFO("sample rate " << sample_rate << ", detected notes " << note_numbers(notes).size()
                        << ": ");
    for (size_t i = 0; i < notes.size(); ++i) {
      INFO("note " << i << ": midi " << static_cast<int>(notes[i].note) << ", onset "
                   << notes[i].onset_sample << ", intended "
                   << (i < sequence.onsets.size() ? sequence.onsets[i] : -1));
    }
    CHECK(note_numbers(notes) == expected);
    if (notes.size() == sequence.onsets.size()) {
      for (size_t i = 0; i < notes.size(); ++i) {
        INFO("sample rate " << sample_rate << ", note " << i << ", onset " << notes[i].onset_sample
                            << ", intended " << sequence.onsets[i]);
        CHECK(std::llabs(notes[i].onset_sample - sequence.onsets[i]) <=
              static_cast<int64_t>(std::llround(0.050 * sample_rate)));
      }
    }
  }
}

TEST_CASE("monophonic transcription repairs octave aliases without raising true low notes",
          "[note_transcriber][pitch_alias]") {
  constexpr int64_t kOnsetToleranceMs = 50;
  for (const int sample_rate : {16000, 22050, 44100, 48000}) {
    const RateSequence sequence = alias_arpeggio(sample_rate);
    const std::vector<TranscribedNote> notes = transcribe_notes(sequence.audio(sample_rate));
    INFO("fast leap phrase at " << sample_rate << " Hz; detected " << notes.size() << " notes");
    REQUIRE(notes.size() == sequence.notes.size());
    CHECK(note_numbers(notes) == sequence.notes);
    for (size_t i = 0; i < notes.size(); ++i) {
      INFO("sample rate " << sample_rate << ", note " << i << ", actual onset "
                          << notes[i].onset_sample << ", intended " << sequence.onsets[i]);
      CHECK(std::llabs(notes[i].onset_sample - sequence.onsets[i]) * 1000 <=
            kOnsetToleranceMs * sample_rate);
    }
  }

  for (const int sample_rate : {16000, 22050, 44100, 48000}) {
    const std::vector<TranscribedNote> notes =
        transcribe_notes(low_fundamental_harmonic_tone(sample_rate, 0.20, 1.0));
    INFO("110 Hz fundamental with stronger second harmonic at " << sample_rate << " Hz");
    REQUIRE(notes.size() == 1);
    CHECK(notes.front().note == 45);
  }

  SECTION("a missing fundamental and a dominant third harmonic remain low") {
    const std::vector<TranscribedNote> missing_fundamental =
        transcribe_notes(low_fundamental_harmonic_tone(22050, 0.0, 1.0, 0.5));
    REQUIRE(missing_fundamental.size() == 1);
    CHECK(missing_fundamental.front().note == 45);

    const std::vector<TranscribedNote> dominant_third =
        transcribe_notes(low_fundamental_harmonic_tone(22050, 0.4, 0.0, 1.0));
    REQUIRE(dominant_third.size() == 1);
    CHECK(dominant_third.front().note == 45);
  }

  for (const int sample_rate : {16000, 22050, 44100, 48000}) {
    const std::vector<TranscribedNote> amplitude_modulated =
        transcribe_notes(modulated_harmonic_tone(sample_rate, 45, 8.0, 0.95));
    INFO("110 Hz amplitude modulation at " << sample_rate << " Hz");
    REQUIRE(amplitude_modulated.size() == 1);
    CHECK(amplitude_modulated.front().note == 45);

    const std::vector<TranscribedNote> vibrato =
        transcribe_notes(modulated_harmonic_tone(sample_rate, 45, 0.0, 0.0, 6.0, 50.0));
    INFO("110 Hz 50-cent vibrato at " << sample_rate << " Hz");
    REQUIRE(vibrato.size() == 1);
    CHECK(vibrato.front().note == 45);
  }
}

TEST_CASE("editing pYIN defaults scale the frame and cap the hop", "[note_transcriber]") {
  struct ExpectedGeometry {
    int sample_rate;
    int frame_length;
    int hop_length;
  };
  const std::vector<ExpectedGeometry> expected{
      {16000, 1486, 186}, {22050, 2048, 256}, {44100, 4096, 512}, {48000, 4458, 512}};
  for (const ExpectedGeometry& geometry : expected) {
    const sonare::PitchConfig config =
        sonare::editing::pitch_editor::default_editing_pitch_config(geometry.sample_rate);
    INFO("sample rate " << geometry.sample_rate);
    CHECK(config.frame_length == geometry.frame_length);
    CHECK(config.hop_length == geometry.hop_length);
  }
}

TEST_CASE("monophonic transcription separates short same-pitch attacks",
          "[note_transcriber][reattack]") {
  constexpr int64_t kOnsetToleranceMs = 50;
  const std::vector<int> sample_rates{16000, 22050, 44100, 48000};
  const std::vector<int> midi_notes{57, 69, 76};
  const std::vector<float> gains{0.03f, 0.3f};
  for (const int sample_rate : sample_rates) {
    for (const int midi_note : midi_notes) {
      for (const float gain : gains) {
        const RateSequence sequence = repeated_harmonic_tone(sample_rate, midi_note, gain);
        const std::vector<TranscribedNote> notes = transcribe_notes(sequence.audio(sample_rate));
        INFO("sample rate " << sample_rate << ", MIDI " << midi_note << ", gain " << gain
                            << ", detected " << notes.size() << " attacks");
        CHECK(notes.size() == sequence.onsets.size());
        if (notes.size() != sequence.onsets.size()) continue;
        for (size_t i = 0; i < notes.size(); ++i) {
          INFO("attack " << i << ", actual onset " << notes[i].onset_sample << ", expected "
                         << sequence.onsets[i]);
          CHECK(notes[i].note == midi_note);
          CHECK(std::llabs(notes[i].onset_sample - sequence.onsets[i]) * 1000 <=
                kOnsetToleranceMs * sample_rate);
          CHECK(notes[i].offset_sample - notes[i].onset_sample >=
                static_cast<int64_t>(std::lround(30.0 * sample_rate / 1000.0)));
        }
      }
    }
  }

  const RateSequence alternating_gain =
      repeated_harmonic_tone(22050, 69, std::vector<float>{0.3f, 0.15f, 0.3f, 0.15f});
  const std::vector<TranscribedNote> alternating_notes =
      transcribe_notes(alternating_gain.audio(22050));
  REQUIRE(alternating_notes.size() == alternating_gain.onsets.size());
  for (size_t i = 0; i < alternating_notes.size(); ++i) {
    INFO("alternating-gain attack " << i);
    CHECK(alternating_notes[i].note == 69);
    CHECK(std::llabs(alternating_notes[i].onset_sample - alternating_gain.onsets[i]) * 1000 <=
          kOnsetToleranceMs * 22050);
  }
}

TEST_CASE("reattack splitting leaves smooth motion, silence, and noise intact",
          "[note_transcriber][reattack]") {
  const auto require_one_note = [](const sonare::Audio& audio, int midi_note) {
    const std::vector<TranscribedNote> notes = transcribe_notes(audio);
    std::string note_spans;
    for (const TranscribedNote& note : notes) {
      note_spans += "[" + std::to_string(note.onset_sample) + "," +
                    std::to_string(note.offset_sample) + "," + std::to_string(note.note) + "]";
    }
    INFO("expected MIDI " << midi_note << ", detected " << notes.size() << " notes");
    INFO("note spans " << note_spans);
    for (size_t i = 0; i < notes.size(); ++i) {
      INFO("note " << i << ": onset " << notes[i].onset_sample << ", offset "
                   << notes[i].offset_sample << ", MIDI " << static_cast<int>(notes[i].note));
    }
    REQUIRE(notes.size() == 1);
    CHECK(notes.front().note == midi_note);
  };

  for (const int sample_rate : {16000, 22050, 44100, 48000}) {
    INFO("held tone at " << sample_rate << " Hz");
    require_one_note(modulated_harmonic_tone(sample_rate, 69, 0.0, 0.0), 69);
  }

  const std::vector<std::tuple<int, int, double>> amplitude_cases{
      {16000, 57, 4.0}, {22050, 69, 8.0}, {48000, 76, 12.0}};
  for (const auto& [sample_rate, midi_note, modulation_hz] : amplitude_cases) {
    INFO("smooth amplitude modulation " << modulation_hz << " Hz at " << sample_rate << " Hz");
    require_one_note(modulated_harmonic_tone(sample_rate, midi_note, modulation_hz, 0.95),
                     midi_note);
  }

  for (const auto& [sample_rate, midi_note] :
       {std::pair<int, int>{22050, 69}, std::pair<int, int>{48000, 57}}) {
    INFO("50-cent vibrato at " << sample_rate << " Hz");
    require_one_note(modulated_harmonic_tone(sample_rate, midi_note, 0.0, 0.0, 6.0, 50.0),
                     midi_note);
  }

  INFO("110 Hz smooth amplitude modulation at 16 kHz");
  require_one_note(modulated_harmonic_tone(16000, 45, 8.0, 0.95), 45);
  INFO("held tone with moderate white noise");
  require_one_note(noisy_harmonic_tone(22050, 69, 0.04f), 69);

  const RateSequence repeated = repeated_harmonic_tone(22050, 69, 0.18f);
  TranscribeConfig long_minimum;
  long_minimum.min_note_ms = 300.0f;
  const std::vector<TranscribedNote> constrained =
      transcribe_notes(repeated.audio(22050), long_minimum);
  REQUIRE(constrained.size() == 1);
  CHECK(constrained.front().note == 69);
  CHECK(constrained.front().offset_sample - constrained.front().onset_sample >= 0.300 * 22050);

  const int64_t quiet_samples = static_cast<int64_t>(2 * 22050);
  std::vector<float> silence(static_cast<size_t>(quiet_samples), 0.0f);
  REQUIRE(
      transcribe_notes(sonare::Audio::from_buffer(silence.data(), silence.size(), 22050)).empty());

  uint32_t state = 0x12345678u;
  std::vector<float> low_noise(static_cast<size_t>(quiet_samples));
  for (float& sample : low_noise) {
    state = 1664525u * state + 1013904223u;
    const float unit = static_cast<float>(state >> 8) / 16777215.0f;
    sample = (2.0f * unit - 1.0f) * 1.0e-5f;
  }
  REQUIRE(transcribe_notes(sonare::Audio::from_buffer(low_noise.data(), low_noise.size(), 22050))
              .empty());
}

TEST_CASE("acoustic onsets preserve a held tone with wide vibrato",
          "[note_transcriber][acoustic_boundary]") {
  for (const int sample_rate : {22050, 44100}) {
    const std::vector<TranscribedNote> notes =
        transcribe_notes(modulated_harmonic_tone(sample_rate, 57, 0.0, 0.0, 6.0, 75.0));
    INFO("220 Hz, 75-cent vibrato at " << sample_rate << " Hz; detected " << notes.size()
                                       << " notes");
    CHECK(notes.size() == 1);
    if (notes.size() == 1) CHECK(notes.front().note == 57);
  }

  for (const int sample_rate : {16000, 48000}) {
    const auto evidence = sonare::editing::note_model::detail::monophonic_onset_evidence(
        modulated_harmonic_tone(sample_rate, 57, 0.0, 0.0, 6.0, 75.0));
    INFO("synthetic onset frames at " << sample_rate << " Hz");
    REQUIRE(evidence.available);
    CHECK((evidence.frames == std::vector<int>{39, 357}));
  }

  const std::vector<TranscribedNote> vibrato50 =
      transcribe_notes(modulated_harmonic_tone(44100, 57, 0.0, 0.0, 6.0, 50.0));
  REQUIRE(vibrato50.size() == 1);
  CHECK(vibrato50.front().note == 57);

  const std::vector<TranscribedNote> glide = transcribe_notes(gliding_harmonic_tone(22050));
  INFO("300-cent glide produced " << glide.size() << " notes");
  CHECK(glide.size() == 3);

  for (const std::vector<int>& intended :
       {std::vector<int>{69, 70}, std::vector<int>{69, 70, 69, 70, 69, 70, 69, 70}}) {
    const RateSequence sequence = contiguous_harmonic_notes(22050, intended);
    const std::vector<TranscribedNote> notes = transcribe_notes(sequence.audio(22050));
    INFO("contiguous semitone sequence, expected " << intended.size() << " notes, got "
                                                   << notes.size());
    CHECK(note_numbers(notes) == intended);
    if (notes.size() == intended.size()) {
      for (size_t i = 0; i < notes.size(); ++i) {
        CHECK(std::llabs(notes[i].onset_sample - sequence.onsets[i]) <=
              static_cast<int64_t>(std::llround(0.050 * 22050)));
      }
    }
  }
}

TEST_CASE("a finite minimum longer than the recording returns no monophonic notes",
          "[note_transcriber]") {
  TranscribeConfig config;
  config.min_note_ms = std::numeric_limits<float>::max();
  CHECK(transcribe_notes(sustained(69), config).empty());
}

TEST_CASE("transcribe_notes answers the same audio identically", "[note_transcriber]") {
  const Sequence sequence = sequence_of({60, 64, 67, 72});
  TranscribeConfig config;
  config.fixed_velocity = 0;

  const std::vector<TranscribedNote> first = transcribe_notes(sequence.audio(), config);
  const std::vector<TranscribedNote> second = transcribe_notes(sequence.audio(), config);
  REQUIRE(!first.empty());
  require_same_notes(second, first);
}

TEST_CASE("a supplied F0 track runs the transcription stages after the tracker",
          "[note_transcriber]") {
  const Sequence sequence = sequence_of({60, 64, 67, 72});
  const sonare::Audio audio = sequence.audio();
  sonare::PitchConfig pitch_config =
      sonare::editing::pitch_editor::default_editing_pitch_config(kSampleRate);
  const TranscribeF0Range range = resolve_transcribe_f0_range(TranscribeConfig{});
  pitch_config.fmin = range.fmin;
  pitch_config.fmax = range.fmax;
  pitch_config.center = true;
  sonare::editing::pitch_editor::PyinF0Provider provider(pitch_config);
  const sonare::editing::pitch_editor::F0Track detected = provider.detect(audio);

  // These tones carry no alias, so the tracker's own track reproduces the default.
  require_same_notes(transcribe_monophonic_track(audio, detected), transcribe_notes(audio));

  // A track two semitones up moves every note, so the pitch comes from the track.
  sonare::editing::pitch_editor::F0Track shifted = detected;
  for (float& hz : shifted.f0_hz) hz *= std::exp2(2.0f / 12.0f);
  std::vector<int> expected;
  for (const int note : sequence.notes) expected.push_back(note + 2);
  CHECK(note_numbers(transcribe_monophonic_track(audio, shifted)) == expected);
}

TEST_CASE("transcribe_monophonic_track refuses a track it cannot align", "[note_transcriber]") {
  const sonare::Audio audio = sustained(60);
  sonare::editing::pitch_editor::F0Track track;
  track.sample_rate = kSampleRate;
  track.hop_length = 256;
  track.f0_hz.assign(8, 261.6f);
  track.voiced.assign(8, true);
  REQUIRE_NOTHROW(transcribe_monophonic_track(audio, track));

  SECTION("another sample rate") {
    track.sample_rate = kSampleRate * 2;
    REQUIRE_THROWS_AS(transcribe_monophonic_track(audio, track), sonare::SonareException);
  }
  SECTION("unequal per-frame arrays") {
    track.voiced.pop_back();
    REQUIRE_THROWS_AS(transcribe_monophonic_track(audio, track), sonare::SonareException);
  }
  SECTION("no frame cadence") {
    track.hop_length = 0;
    REQUIRE_THROWS_AS(transcribe_monophonic_track(audio, track), sonare::SonareException);
  }
  SECTION("the polyphonic source") {
    TranscribeConfig config;
    config.source = TranscribeSource::kPolyphonic;
    REQUIRE_THROWS_AS(transcribe_monophonic_track(audio, track, config), sonare::SonareException);
  }
}

// --- midi_note_for_hz -----------------------------------------------------

TEST_CASE("midi_note_for_hz places a frequency on the equal-tempered grid", "[note_transcriber]") {
  const float a4 = sonare::constants::kA4Hz;

  REQUIRE(midi_note_for_hz(a4, a4) == 69);
  REQUIRE(midi_note_for_hz(261.6256f, a4) == 60);
  REQUIRE(midi_note_for_hz(523.2511f, a4) == 72);

  SECTION("an octave is twelve") {
    for (const int note : {24, 36, 48, 60, 72, 84, 96}) {
      INFO("MIDI " << note);
      REQUIRE(midi_note_for_hz(hz_for_midi(note), a4) == note);
      REQUIRE(midi_note_for_hz(2.0f * hz_for_midi(note), a4) == note + 12);
      REQUIRE(midi_note_for_hz(0.5f * hz_for_midi(note), a4) == note - 12);
    }
  }

  SECTION("rounding takes the nearest note, both ways") {
    // A quarter tone is 50 cents; a third of a semitone lands inside the nearest
    // note's half, so these are the two sides of one boundary rather than a
    // restatement of the exact case.
    const float third_of_a_semitone = std::pow(2.0f, 1.0f / 36.0f);
    REQUIRE(midi_note_for_hz(a4 * third_of_a_semitone, a4) == 69);
    REQUIRE(midi_note_for_hz(a4 / third_of_a_semitone, a4) == 69);
    const float two_thirds = std::pow(2.0f, 2.0f / 36.0f);
    REQUIRE(midi_note_for_hz(a4 * two_thirds, a4) == 70);
    REQUIRE(midi_note_for_hz(a4 / two_thirds, a4) == 68);
  }

  SECTION("a negative is the only no-note spelling") {
    const float a4_reference = a4;
    // Enumerated one by one: an infinity compares normally and would pass a bare
    // ordering test that a NaN fails, so neither stands in for the other.
    REQUIRE(midi_note_for_hz(0.0f, a4_reference) == -1);
    REQUIRE(midi_note_for_hz(-440.0f, a4_reference) == -1);
    REQUIRE(midi_note_for_hz(kNaN, a4_reference) == -1);
    REQUIRE(midi_note_for_hz(kInf, a4_reference) == -1);
    REQUIRE(midi_note_for_hz(-kInf, a4_reference) == -1);

    // Below MIDI 0 (8.1758 Hz) and above MIDI 127 (12543.85 Hz).
    REQUIRE(midi_note_for_hz(4.0f, a4_reference) == -1);
    REQUIRE(midi_note_for_hz(20000.0f, a4_reference) == -1);
    // The two notes just inside those ends are notes, which is what says the
    // refusals above are about the range rather than about the whole extreme.
    REQUIRE(midi_note_for_hz(hz_for_midi(0), a4_reference) == 0);
    REQUIRE(midi_note_for_hz(hz_for_midi(127), a4_reference) == 127);
  }

  SECTION("an unusable reference refuses every frequency") {
    for (const float reference : {0.0f, -440.0f, kNaN, kInf, -kInf}) {
      INFO("reference " << reference);
      REQUIRE(midi_note_for_hz(a4, reference) == -1);
      REQUIRE(midi_note_for_hz(261.6256f, reference) == -1);
    }
  }
}

// --- velocity_for_peak_rms ------------------------------------------------

TEST_CASE("velocity_for_peak_rms maps a level onto the velocity range", "[note_transcriber]") {
  constexpr float kFloorDb = kDeepFloorDb;

  SECTION("both ends") {
    REQUIRE(velocity_for_peak_rms(1.0f, kFloorDb) == 127);
    // Above full scale saturates rather than running past 127.
    REQUIRE(velocity_for_peak_rms(4.0f, kFloorDb) == 127);
    REQUIRE(velocity_for_peak_rms(linear_of(kFloorDb), kFloorDb) == 1);
    REQUIRE(velocity_for_peak_rms(linear_of(kFloorDb - 12.0f), kFloorDb) == 1);
  }

  SECTION("a level midway in dB is midway in velocity") {
    // Linear in decibels, so half the floor is half the span: 1 + 0.5 * 126 = 64.
    // Tolerance 1, which is the rounding of the map's own lround and nothing more.
    const int midpoint = velocity_for_peak_rms(linear_of(0.5f * kFloorDb), kFloorDb);
    INFO("velocity at " << 0.5f * kFloorDb << " dBFS: " << midpoint);
    REQUIRE(std::abs(midpoint - 64) <= 1);

    // A quarter and three quarters up, to the same tolerance, so the map is
    // pinned along its length rather than at one interior point.
    REQUIRE(std::abs(velocity_for_peak_rms(linear_of(0.75f * kFloorDb), kFloorDb) - 33) <= 1);
    REQUIRE(std::abs(velocity_for_peak_rms(linear_of(0.25f * kFloorDb), kFloorDb) - 95) <= 1);
  }

  SECTION("monotonic in level") {
    int previous = 0;
    int strictly_increased = 0;
    for (int db = -60; db <= 6; ++db) {
      const int velocity = velocity_for_peak_rms(linear_of(static_cast<float>(db)), kFloorDb);
      INFO(db << " dBFS -> " << velocity);
      REQUIRE(velocity >= previous);
      REQUIRE(velocity >= 1);
      REQUIRE(velocity <= 127);
      if (velocity > previous) ++strictly_increased;
      previous = velocity;
    }
    // Ordered is not enough: a constant answer is non-decreasing too.
    REQUIRE(strictly_increased > 40);
  }

  SECTION("a level that is not a level is the floor") {
    for (const float peak : {0.0f, -0.5f, kNaN, kInf, -kInf}) {
      INFO("peak " << peak);
      REQUIRE(velocity_for_peak_rms(peak, kFloorDb) == 1);
    }
  }

  SECTION("a floor that is not a floor is the floor") {
    // The contract is "finite and negative"; 0 has no span to map onto and a
    // positive floor would invert the map.
    for (const float floor_db : {0.0f, 6.0f, kNaN, kInf, -kInf}) {
      INFO("floor " << floor_db);
      REQUIRE(velocity_for_peak_rms(1.0f, floor_db) == 1);
    }
  }
}

// --- Config fields that must reach the answer -----------------------------

TEST_CASE("reference_hz moves the note numbers it is measured against", "[note_transcriber]") {
  // The field is read by the rounding and by the segmenter, and a transcriber
  // that stored it and then measured against 440 anyway would pass every other
  // case in this file.
  const sonare::Audio audio = sustained(69);

  TranscribeConfig at_a440;
  const std::vector<TranscribedNote> standard = transcribe_notes(audio, at_a440);
  REQUIRE(standard.size() == 1);
  REQUIRE(standard[0].note == 69);

  TranscribeConfig a_semitone_up;
  a_semitone_up.reference_hz = sonare::constants::kA4Hz * std::pow(2.0f, 1.0f / 12.0f);
  const std::vector<TranscribedNote> raised = transcribe_notes(audio, a_semitone_up);
  REQUIRE(raised.size() == 1);
  // The same tone measured against a reference a semitone higher is a semitone
  // lower on the grid.
  REQUIRE(raised[0].note == 68);

  TranscribeConfig a_semitone_down;
  a_semitone_down.reference_hz = sonare::constants::kA4Hz / std::pow(2.0f, 1.0f / 12.0f);
  const std::vector<TranscribedNote> lowered = transcribe_notes(audio, a_semitone_down);
  REQUIRE(lowered.size() == 1);
  REQUIRE(lowered[0].note == 70);

  // The measured pitch is the same take in all three: only the grid moved.
  CHECK_THAT(raised[0].median_hz, WithinRel(standard[0].median_hz, 1.0e-3f));
  CHECK_THAT(lowered[0].median_hz, WithinRel(standard[0].median_hz, 1.0e-3f));
}

TEST_CASE("polyphonic fmin and fmax constrain the selected tracker range", "[note_transcriber]") {
  const sonare::Audio audio = sustained(69);

  TranscribeConfig inclusive;
  inclusive.source = TranscribeSource::kPolyphonic;
  inclusive.fmin = 400.0f;
  inclusive.fmax = 500.0f;
  const std::vector<TranscribedNote> in_range = transcribe_notes(audio, inclusive);
  REQUIRE(!in_range.empty());
  bool found_a4 = false;
  for (const TranscribedNote& note : in_range) {
    INFO("median " << note.median_hz);
    CHECK(note.median_hz >= inclusive.fmin);
    CHECK(note.median_hz <= inclusive.fmax);
    if (note.note == 69) found_a4 = true;
  }
  REQUIRE(found_a4);

  TranscribeConfig exclusive = inclusive;
  exclusive.fmin = 600.0f;
  exclusive.fmax = 700.0f;
  const std::vector<TranscribedNote> out_of_range = transcribe_notes(audio, exclusive);
  for (const TranscribedNote& note : out_of_range) {
    INFO("median " << note.median_hz);
    CHECK(note.median_hz >= exclusive.fmin);
    CHECK(note.median_hz <= exclusive.fmax);
    CHECK(note.note != 69);
  }
}

TEST_CASE("the F0 range resolver uses the selected source and explicit endpoints",
          "[note_transcriber]") {
  const sonare::PitchConfig mono_defaults;
  const sonare::editing::polyphony::SalienceConfig poly_defaults;

  TranscribeConfig monophonic;
  const TranscribeF0Range mono_range = resolve_transcribe_f0_range(monophonic);
  CHECK(mono_range.fmin == mono_defaults.fmin);
  CHECK(mono_range.fmax == mono_defaults.fmax);

  TranscribeConfig polyphonic;
  polyphonic.source = TranscribeSource::kPolyphonic;
  const TranscribeF0Range poly_range = resolve_transcribe_f0_range(polyphonic);
  CHECK(poly_range.fmin == poly_defaults.f0_min_hz);
  CHECK(poly_range.fmax == poly_defaults.f0_max_hz);

  monophonic.fmin = 80.0f;
  monophonic.fmax = 0.0f;
  const TranscribeF0Range mono_partial = resolve_transcribe_f0_range(monophonic);
  CHECK(mono_partial.fmin == 80.0f);
  CHECK(mono_partial.fmax == mono_defaults.fmax);

  polyphonic.fmin = 0.0f;
  polyphonic.fmax = 1500.0f;
  const TranscribeF0Range poly_partial = resolve_transcribe_f0_range(polyphonic);
  CHECK(poly_partial.fmin == poly_defaults.f0_min_hz);
  CHECK(poly_partial.fmax == 1500.0f);
}

TEST_CASE("omitted tracker bounds resolve from the selected source", "[note_transcriber]") {
  const sonare::PitchConfig mono_defaults;
  const sonare::editing::polyphony::SalienceConfig poly_defaults;

  for (const int midi_note : {33, 35}) {
    INFO("MIDI " << midi_note);
    const sonare::Audio audio = sustained(midi_note);

    TranscribeConfig poly_default;
    poly_default.source = TranscribeSource::kPolyphonic;
    poly_default.fmin = 0.0f;
    poly_default.fmax = 0.0f;
    const std::vector<TranscribedNote> default_notes = transcribe_notes(audio, poly_default);

    TranscribeConfig poly_explicit = poly_default;
    poly_explicit.fmin = poly_defaults.f0_min_hz;
    poly_explicit.fmax = poly_defaults.f0_max_hz;
    const std::vector<TranscribedNote> explicit_notes = transcribe_notes(audio, poly_explicit);
    REQUIRE(!explicit_notes.empty());
    require_same_notes(default_notes, explicit_notes);

    bool found_target = false;
    for (const TranscribedNote& note : default_notes) {
      if (note.note == midi_note) found_target = true;
    }
    REQUIRE(found_target);

    // The monophonic lower bound is the exclusion boundary for both A1 and B1.
    // A polyphonic default that accidentally inherited the monophonic range
    // would drop these notes before it could report them.
    TranscribeConfig mono_boundary = poly_default;
    mono_boundary.fmin = mono_defaults.fmin;
    mono_boundary.fmax = poly_defaults.f0_max_hz;
    const std::vector<TranscribedNote> boundary_notes = transcribe_notes(audio, mono_boundary);
    for (const TranscribedNote& note : boundary_notes) {
      INFO("boundary median " << note.median_hz);
      CHECK(note.median_hz >= mono_boundary.fmin);
      CHECK(note.median_hz <= mono_boundary.fmax);
      CHECK(note.note != midi_note);
    }
  }
}

TEST_CASE("a partial tracker range resolves and validates against source defaults",
          "[note_transcriber]") {
  const sonare::PitchConfig mono_defaults;
  const sonare::editing::polyphony::SalienceConfig poly_defaults;
  const sonare::Audio audio = sustained(69);

  SECTION("polyphonic omitted endpoints use salience defaults") {
    TranscribeConfig expected;
    expected.source = TranscribeSource::kPolyphonic;
    expected.fmin = poly_defaults.f0_min_hz;
    expected.fmax = poly_defaults.f0_max_hz;
    const std::vector<TranscribedNote> explicit_notes = transcribe_notes(audio, expected);
    REQUIRE(!explicit_notes.empty());

    TranscribeConfig omitted_low = expected;
    omitted_low.fmin = 0.0f;
    require_same_notes(transcribe_notes(audio, omitted_low), explicit_notes);

    TranscribeConfig omitted_high = expected;
    omitted_high.fmax = 0.0f;
    require_same_notes(transcribe_notes(audio, omitted_high), explicit_notes);
  }

  SECTION("the same partial values resolve against monophonic defaults") {
    TranscribeConfig expected;
    expected.fmin = mono_defaults.fmin;
    expected.fmax = mono_defaults.fmax;
    const std::vector<TranscribedNote> explicit_notes = transcribe_notes(audio, expected);
    REQUIRE(!explicit_notes.empty());

    TranscribeConfig omitted_low = expected;
    omitted_low.fmin = 0.0f;
    require_same_notes(transcribe_notes(audio, omitted_low), explicit_notes);

    TranscribeConfig omitted_high = expected;
    omitted_high.fmax = 0.0f;
    require_same_notes(transcribe_notes(audio, omitted_high), explicit_notes);
  }

  SECTION("ordering is checked after the source default is resolved") {
    TranscribeConfig polyphonic;
    polyphonic.source = TranscribeSource::kPolyphonic;
    polyphonic.fmin = 1800.0f;
    polyphonic.fmax = 0.0f;
    REQUIRE_THROWS_AS(transcribe_notes(audio, polyphonic), sonare::SonareException);

    TranscribeConfig monophonic;
    monophonic.fmin = 1800.0f;
    monophonic.fmax = 0.0f;
    REQUIRE_NOTHROW(transcribe_notes(audio, monophonic));
  }
}

TEST_CASE("fixed_velocity replaces the measurement and velocity_floor_db changes it",
          "[note_transcriber]") {
  const Sequence sequence = sequence_of({60, 64, 67, 72});

  TranscribeConfig measured;
  const std::vector<TranscribedNote> measured_notes = transcribe_notes(sequence.audio(), measured);
  REQUIRE(measured_notes.size() == sequence.notes.size());

  SECTION("a fixed velocity is every note's velocity") {
    for (const int fixed : {1, 42, 127}) {
      INFO("fixed_velocity " << fixed);
      TranscribeConfig config;
      config.fixed_velocity = fixed;
      const std::vector<TranscribedNote> notes = transcribe_notes(sequence.audio(), config);
      REQUIRE(notes.size() == measured_notes.size());
      for (const TranscribedNote& note : notes) REQUIRE(note.velocity == fixed);
    }

    // And the notes themselves are untouched, so the field replaced the velocity
    // rather than a different measurement having produced it.
    TranscribeConfig config;
    config.fixed_velocity = 42;
    const std::vector<TranscribedNote> notes = transcribe_notes(sequence.audio(), config);
    for (size_t i = 0; i < notes.size(); ++i) {
      REQUIRE(notes[i].onset_sample == measured_notes[i].onset_sample);
      REQUIRE(notes[i].note == measured_notes[i].note);
    }
    // 42 is not what the measurement answers here, so the case is not passing on
    // a coincidence.
    bool any_measured_42 = false;
    for (const TranscribedNote& note : measured_notes) {
      if (note.velocity == 42) any_measured_42 = true;
    }
    REQUIRE_FALSE(any_measured_42);
  }

  SECTION("a higher floor compresses the same level into a lower velocity") {
    TranscribeConfig shallow;
    shallow.velocity_floor_db = kShallowFloorDb;
    const std::vector<TranscribedNote> notes = transcribe_notes(sequence.audio(), shallow);
    REQUIRE(notes.size() == measured_notes.size());
    for (size_t i = 0; i < notes.size(); ++i) {
      INFO("note " << i << ": " << static_cast<int>(measured_notes[i].velocity) << " at -48 dB, "
                   << static_cast<int>(notes[i].velocity) << " at -12 dB");
      REQUIRE(notes[i].velocity < measured_notes[i].velocity);
      // Both velocities have to be the exposed map over ONE measurement, so the
      // level is recovered from the -48 dB answer and pushed back through the map
      // at -12 dB. The recovered level carries the first velocity's rounding,
      // which is 48/126 dB and reads as +-2 velocity steps at the steeper floor.
      const float recovered_db =
          kDeepFloorDb +
          (static_cast<float>(measured_notes[i].velocity) - 1.0f) / 126.0f * -kDeepFloorDb;
      const int predicted = velocity_for_peak_rms(linear_of(recovered_db), kShallowFloorDb);
      INFO("recovered " << recovered_db << " dBFS, predicting " << predicted);
      REQUIRE(std::abs(static_cast<int>(notes[i].velocity) - predicted) <= 3);
    }
  }
}

TEST_CASE("a quiet take and a soft note keep their notes", "[note_transcriber]") {
  const std::vector<int> melody = {60, 64, 67, 72};

  SECTION("a take 40 dB down transcribes the same notes, under the velocity floor") {
    const Sequence quiet = sequence_of(melody, 0.005f);
    const std::vector<TranscribedNote> notes = transcribe_notes(quiet.audio());
    REQUIRE(note_numbers(notes) == melody);
    for (const TranscribedNote& note : notes) CHECK(note.velocity == 1);
  }

  SECTION("a note 34 dB under its neighbours survives in both chains") {
    // Loud and soft alternate, so the soft notes sit 34 dB under the reference and
    // under the -48 dB velocity floor too.
    constexpr float kLoud = 0.1f;
    const float soft = kLoud * std::pow(10.0f, -34.0f / 20.0f);
    Sequence sequence;
    append_silence(sequence.samples, kGapSamples);
    for (size_t i = 0; i < melody.size(); ++i) {
      append_tone(sequence.samples, hz_for_midi(melody[i]), i % 2 == 0 ? kLoud : soft,
                  kToneSamples);
      append_silence(sequence.samples, kGapSamples);
    }
    for (const TranscribeSource source :
         {TranscribeSource::kMonophonic, TranscribeSource::kPolyphonic}) {
      INFO("polyphonic " << (source == TranscribeSource::kPolyphonic));
      TranscribeConfig config;
      config.source = source;
      REQUIRE(note_numbers(transcribe_notes(sequence.audio(), config)) == melody);
    }
  }
}

// --- Rejections -----------------------------------------------------------

TEST_CASE("transcribe_notes refuses audio and config it cannot read", "[note_transcriber]") {
  const sonare::Audio audio = sustained(69);

  auto refused = [&](const TranscribeConfig& config) {
    REQUIRE_THROWS_AS(transcribe_notes(audio, config), sonare::SonareException);
    try {
      transcribe_notes(audio, config);
    } catch (const sonare::SonareException& error) {
      REQUIRE(error.code() == sonare::ErrorCode::InvalidParameter);
    }
  };

  SECTION("empty audio") {
    const sonare::Audio empty;
    REQUIRE(empty.empty());
    REQUIRE_THROWS_AS(transcribe_notes(empty), sonare::SonareException);
    try {
      transcribe_notes(empty);
    } catch (const sonare::SonareException& error) {
      REQUIRE(error.code() == sonare::ErrorCode::InvalidParameter);
    }
  }

  SECTION("a range with nothing in it") {
    TranscribeConfig equal;
    equal.fmin = 440.0f;
    equal.fmax = 440.0f;
    refused(equal);

    TranscribeConfig inverted;
    inverted.fmin = 2093.0f;
    inverted.fmax = 65.0f;
    refused(inverted);
  }

  SECTION("a velocity floor that is not finite and negative") {
    for (const float floor_db : {0.0f, 6.0f, kNaN, kInf, -kInf}) {
      INFO("velocity_floor_db " << floor_db);
      TranscribeConfig config;
      config.velocity_floor_db = floor_db;
      refused(config);
    }
  }

  SECTION("a fixed velocity outside {0} U [1, 127]") {
    for (const int fixed : {-1, 128, 255}) {
      INFO("fixed_velocity " << fixed);
      TranscribeConfig config;
      config.fixed_velocity = fixed;
      refused(config);
    }
  }

  SECTION("a non-finite config field") {
    for (const float bad : {kNaN, kInf, -kInf}) {
      INFO("value " << bad);
      TranscribeConfig reference;
      reference.reference_hz = bad;
      refused(reference);

      TranscribeConfig low;
      low.fmin = bad;
      refused(low);

      TranscribeConfig high;
      high.fmax = bad;
      refused(high);

      TranscribeConfig shortest;
      shortest.min_note_ms = bad;
      refused(shortest);

      TranscribeConfig threshold;
      threshold.segmentation_threshold_cents = bad;
      refused(threshold);
    }
  }

  SECTION("a non-positive reference, negative range or threshold") {
    TranscribeConfig reference;
    reference.reference_hz = 0.0f;
    refused(reference);

    TranscribeConfig low;
    low.fmin = -1.0f;
    refused(low);

    TranscribeConfig high;
    high.fmax = -1.0f;
    refused(high);

    TranscribeConfig shortest;
    shortest.min_note_ms = -1.0f;
    refused(shortest);

    TranscribeConfig threshold;
    threshold.segmentation_threshold_cents = 0.0f;
    refused(threshold);
  }

  SECTION("a source that is neither chain") {
    TranscribeConfig config;
    config.source = static_cast<TranscribeSource>(7);
    refused(config);
  }

  // The defaults are accepted, so every refusal above is attributable to the one
  // field it changed.
  REQUIRE_NOTHROW(transcribe_notes(audio));
}

// --- The polyphonic limits ------------------------------------------------

namespace {

constexpr int kPolyRate = 44100;
constexpr int kPolyPartials = 10;

/// @brief Adds a harmonic tone at @p f0_hz to @p into from @p start for @p length samples,
///        scaled per sample by @p gain.
template <typename Gain>
void add_harmonic(std::vector<float>& into, double f0_hz, size_t start, size_t length, Gain gain) {
  const double nyquist = 0.5 * kPolyRate;
  for (size_t i = 0; i < length && start + i < into.size(); ++i) {
    const double t = static_cast<double>(i) / kPolyRate;
    double sample = 0.0;
    for (int h = 1; h <= kPolyPartials; ++h) {
      const double hz = f0_hz * h;
      if (hz >= nyquist) break;
      sample += 0.25 / h * std::sin(sonare::constants::kTwoPiD * hz * t + 0.37 * h * h);
    }
    into[start + i] += static_cast<float>(sample * gain(t));
  }
}

/// @brief One pitch struck at 0 s and again at 3x the level at 0.45 s while the first
///        still sounds. The second strike rises over 20 ms: a hard step breaks the ridge
///        on its own, which would leave reattack_ratio nothing to decide.
sonare::Audio restruck_tone() {
  constexpr double kRestrikeSec = 0.45;
  constexpr double kDecayPerSec = 1.5;
  constexpr double kAttackSec = 0.02;
  std::vector<float> samples(static_cast<size_t>(0.9 * kPolyRate), 0.0f);
  add_harmonic(samples, hz_for_midi(64), 0, samples.size(), [&](double t) {
    double gain = std::exp(-kDecayPerSec * t);
    if (t >= kRestrikeSec) {
      const double since = t - kRestrikeSec;
      gain += 3.0 * std::min(1.0, since / kAttackSec) * std::exp(-kDecayPerSec * since);
    }
    return gain;
  });
  return sonare::Audio::from_buffer(samples.data(), samples.size(), kPolyRate);
}

/// @brief A held tone and, apart from it, one 100 ms tone with short raised edges.
sonare::Audio held_and_short() {
  constexpr double kEdgeSec = 0.005;
  constexpr double kShortSec = 0.1;
  std::vector<float> samples(static_cast<size_t>(1.0 * kPolyRate), 0.0f);
  add_harmonic(samples, hz_for_midi(64), 0, samples.size(), [](double) { return 1.0; });
  const auto edged = [](double t) {
    const double edge = std::min(1.0, std::min(t, kShortSec - t) / kEdgeSec);
    return std::max(0.0, edge);
  };
  add_harmonic(samples, hz_for_midi(71), static_cast<size_t>(0.4 * kPolyRate),
               static_cast<size_t>(kShortSec * kPolyRate), edged);
  return sonare::Audio::from_buffer(samples.data(), samples.size(), kPolyRate);
}

size_t count_of(const std::vector<TranscribedNote>& notes, int midi_note) {
  return static_cast<size_t>(std::count_if(
      notes.begin(), notes.end(), [&](const TranscribedNote& n) { return n.note == midi_note; }));
}

}  // namespace

TEST_CASE("the polyphonic limits resolve per source", "[note_transcriber]") {
  TranscribeConfig monophonic;
  const TranscribePolyphonyLimits mono = resolve_transcribe_polyphony_limits(monophonic);
  CHECK(mono.min_note_ms == 30.0f);

  TranscribeConfig polyphonic;
  polyphonic.source = TranscribeSource::kPolyphonic;
  const TranscribePolyphonyLimits poly = resolve_transcribe_polyphony_limits(polyphonic);
  CHECK(poly.min_note_ms == 60.0f);
  CHECK(poly.max_polyphony == 10);
  CHECK(poly.min_frame_peak_ratio == 0.20f);
  CHECK(poly.min_ridge_peak_ratio == 0.10f);
  CHECK(poly.reattack_ratio == 2.0f);

  SECTION("a negative ratio is a real 0 and a negative reattack is off") {
    polyphonic.min_frame_peak_ratio = -1.0f;
    polyphonic.min_ridge_peak_ratio = -0.5f;
    polyphonic.reattack_ratio = -1.0f;
    const TranscribePolyphonyLimits floored = resolve_transcribe_polyphony_limits(polyphonic);
    CHECK(floored.min_frame_peak_ratio == 0.0f);
    CHECK(floored.min_ridge_peak_ratio == 0.0f);
    CHECK(floored.reattack_ratio == 0.0f);
  }

  SECTION("a given value is copied") {
    polyphonic.min_note_ms = 45.0f;
    polyphonic.max_polyphony = 3;
    polyphonic.min_frame_peak_ratio = 0.3f;
    polyphonic.min_ridge_peak_ratio = 0.4f;
    polyphonic.reattack_ratio = 1.5f;
    const TranscribePolyphonyLimits given = resolve_transcribe_polyphony_limits(polyphonic);
    CHECK(given.min_note_ms == 45.0f);
    CHECK(given.max_polyphony == 3);
    CHECK(given.min_frame_peak_ratio == 0.3f);
    CHECK(given.min_ridge_peak_ratio == 0.4f);
    CHECK(given.reattack_ratio == 1.5f);
  }
}

TEST_CASE("the shortest note resolves from the tempo", "[note_transcriber]") {
  TranscribeConfig polyphonic;
  polyphonic.source = TranscribeSource::kPolyphonic;

  SECTION("the polyphonic default is 60 ms whatever the tempo") {
    for (const float tempo : {0.0f, 60.0f, 160.0f, 300.0f}) {
      INFO("tempo " << tempo);
      polyphonic.tempo_bpm = tempo;
      CHECK(resolve_transcribe_polyphony_limits(polyphonic).min_note_ms == 60.0f);
    }
  }

  SECTION("a division is a note value at the tempo, unbounded") {
    polyphonic.tempo_bpm = 120.0f;
    polyphonic.min_note_division = 16;
    CHECK(resolve_transcribe_polyphony_limits(polyphonic).min_note_ms == Catch::Approx(125.0f));
    polyphonic.min_note_division = 128;
    CHECK(resolve_transcribe_polyphony_limits(polyphonic).min_note_ms == Catch::Approx(15.625f));
  }

  SECTION("milliseconds outrank the tempo") {
    polyphonic.tempo_bpm = 300.0f;
    polyphonic.min_note_ms = 80.0f;
    CHECK(resolve_transcribe_polyphony_limits(polyphonic).min_note_ms == 80.0f);
  }

  SECTION("the monophonic default does not move with the tempo, a division does") {
    TranscribeConfig monophonic;
    monophonic.tempo_bpm = 300.0f;
    CHECK(resolve_transcribe_polyphony_limits(monophonic).min_note_ms == 30.0f);
    monophonic.min_note_division = 8;
    CHECK(resolve_transcribe_polyphony_limits(monophonic).min_note_ms == Catch::Approx(100.0f));
  }
}

TEST_CASE("min_note_division is the shortest note kept, read at the tempo", "[note_transcriber]") {
  const sonare::Audio audio = held_and_short();
  constexpr int kShortNote = 71;

  TranscribeConfig config;
  config.source = TranscribeSource::kPolyphonic;
  config.tempo_bpm = 120.0f;
  config.min_note_division = 32;
  REQUIRE(count_of(transcribe_notes(audio, config), kShortNote) == 1);

  // An eighth note at 120 BPM is 250 ms, longer than the 100 ms note.
  config.min_note_division = 8;
  CHECK(count_of(transcribe_notes(audio, config), kShortNote) == 0);
  // A sixteenth note at 300 BPM is 50 ms.
  config.tempo_bpm = 300.0f;
  config.min_note_division = 16;
  CHECK(count_of(transcribe_notes(audio, config), kShortNote) == 1);
}

TEST_CASE("min_note_ms is the shortest polyphonic note kept", "[note_transcriber]") {
  const sonare::Audio audio = held_and_short();
  constexpr int kShortNote = 71;

  TranscribeConfig config;
  config.source = TranscribeSource::kPolyphonic;
  config.min_note_ms = 40.0f;
  const std::vector<TranscribedNote> kept = transcribe_notes(audio, config);
  INFO("kept " << kept.size() << " notes");
  REQUIRE(count_of(kept, 64) >= 1);
  REQUIRE(count_of(kept, kShortNote) == 1);

  config.min_note_ms = 300.0f;
  const std::vector<TranscribedNote> dropped = transcribe_notes(audio, config);
  CHECK(count_of(dropped, 64) >= 1);
  CHECK(count_of(dropped, kShortNote) == 0);
}

TEST_CASE("a re-struck pitch is two notes by default and one with the split off",
          "[note_transcriber]") {
  const sonare::Audio audio = restruck_tone();
  TranscribeConfig config;
  config.source = TranscribeSource::kPolyphonic;
  const std::vector<TranscribedNote> split = transcribe_notes(audio, config);
  INFO("default notes " << split.size());
  CHECK(count_of(split, 64) == 2);

  config.reattack_ratio = -1.0f;
  const std::vector<TranscribedNote> whole = transcribe_notes(audio, config);
  INFO("split-off notes " << whole.size());
  CHECK(count_of(whole, 64) == 1);
}

TEST_CASE("the polyphonic limits refuse what they cannot read", "[note_transcriber]") {
  const sonare::Audio audio = sustained(69);

  auto refused_naming = [&](const TranscribeConfig& config, const std::string& field) {
    try {
      (void)transcribe_notes(audio, config);
      FAIL("accepted a config naming " << field);
    } catch (const sonare::SonareException& error) {
      CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
      CHECK(std::string(error.what()).find(field) != std::string::npos);
    }
  };

  SECTION("a monophonic config carrying any of them, negative included") {
    for (const float value : {-1.0f, 0.3f}) {
      INFO("value " << value);
      TranscribeConfig frame;
      frame.min_frame_peak_ratio = value;
      refused_naming(frame, "min_frame_peak_ratio");
      TranscribeConfig ridge;
      ridge.min_ridge_peak_ratio = value;
      refused_naming(ridge, "min_ridge_peak_ratio");
    }
    for (const float value : {-1.0f, 2.5f}) {
      TranscribeConfig reattack;
      reattack.reattack_ratio = value;
      refused_naming(reattack, "reattack_ratio");
    }
    TranscribeConfig voices;
    voices.max_polyphony = 4;
    refused_naming(voices, "max_polyphony");
  }

  SECTION("a note value it cannot read, on either source") {
    for (const TranscribeSource source :
         {TranscribeSource::kMonophonic, TranscribeSource::kPolyphonic}) {
      TranscribeConfig base;
      base.source = source;
      base.tempo_bpm = 120.0f;
      for (const int division : {-1, 129}) {
        TranscribeConfig config = base;
        config.min_note_division = division;
        refused_naming(config, "min_note_division");
      }
      TranscribeConfig both = base;
      both.min_note_division = 32;
      both.min_note_ms = 40.0f;
      refused_naming(both, "min_note_division");
      TranscribeConfig untimed = base;
      untimed.tempo_bpm = 0.0f;
      untimed.min_note_division = 32;
      refused_naming(untimed, "tempo_bpm");
      for (const float tempo : {-120.0f, kNaN, kInf}) {
        TranscribeConfig config = base;
        config.tempo_bpm = tempo;
        refused_naming(config, "tempo_bpm");
      }
    }
  }

  SECTION("a polyphonic value outside its domain") {
    TranscribeConfig base;
    base.source = TranscribeSource::kPolyphonic;
    for (const int voices : {-1, 65}) {
      TranscribeConfig config = base;
      config.max_polyphony = voices;
      refused_naming(config, "max_polyphony");
    }
    for (const float ratio : {1.5f, kNaN, kInf, -kInf}) {
      TranscribeConfig frame = base;
      frame.min_frame_peak_ratio = ratio;
      refused_naming(frame, "min_frame_peak_ratio");
      TranscribeConfig ridge = base;
      ridge.min_ridge_peak_ratio = ratio;
      refused_naming(ridge, "min_ridge_peak_ratio");
    }
    for (const float ratio : {0.5f, 1.0f, kNaN, kInf, -kInf}) {
      TranscribeConfig reattack = base;
      reattack.reattack_ratio = ratio;
      refused_naming(reattack, "reattack_ratio");
    }
  }
}

TEST_CASE("Bach score transcription evaluation", "[.][bach_transcribe_eval]") {
  const char* reference_dir = std::getenv("SONARE_BACH_REFERENCE_DIR");
  REQUIRE(reference_dir != nullptr);
  REQUIRE(reference_dir[0] != '\0');

  std::string directory(reference_dir);
  if (directory.back() != '/') directory.push_back('/');
  const std::vector<BachExcerpt> excerpts{{"BWV1001_4", 0.0, 16.0, 96},
                                          {"BWV1006_1", 320.0, 336.0, 120},
                                          {"BWV1007_1", 36.0, 52.0, 120},
                                          {"BWV1003_4", 276.0, 292.0, 80}};
  const std::vector<int> sample_rates{16000, 22050, 44100, 48000};
  size_t evaluated_cases = 0;
  for (const BachExcerpt& excerpt : excerpts) {
    const sonare::test::JsonValue score =
        sonare::test::JsonReader::parse_file(directory + excerpt.work + ".json");
    for (const int sample_rate : sample_rates) {
      const std::vector<BachReferenceNote> truth = bach_excerpt(score, excerpt, sample_rate);
      REQUIRE(!truth.empty());
      const sonare::Audio audio = render_bach_excerpt(truth, sample_rate);
      TranscribeConfig config;
      config.tempo_bpm = static_cast<float>(excerpt.tempo_bpm);
      const auto start = std::chrono::steady_clock::now();
      const std::vector<TranscribedNote> estimates = transcribe_notes(audio, config);
      const auto stop = std::chrono::steady_clock::now();
      const double wall_ms = std::chrono::duration<double, std::milli>(stop - start).count();
      const sonare::test::NoteMatchCounts match = score_bach_notes(truth, estimates, sample_rate);
      const double precision =
          estimates.empty() ? 0.0 : static_cast<double>(match.true_positive) / estimates.size();
      const double recall = static_cast<double>(match.true_positive) / truth.size();
      const double f_measure =
          precision + recall > 0.0 ? 2.0 * precision * recall / (precision + recall) : 0.0;

      std::cout << std::setprecision(9) << "BACH_TRANSCRIBE_EVAL {\"work\":\"" << excerpt.work
                << "\",\"input\":\"external_score_json\",\"sample_rate\":" << sample_rate
                << ",\"tempo_bpm\":" << excerpt.tempo_bpm << ",\"truth_count\":" << truth.size()
                << ",\"predicted_count\":" << estimates.size() << ",\"tp\":" << match.true_positive
                << ",\"fp\":" << match.false_positive << ",\"fn\":" << match.false_negative
                << ",\"precision\":" << precision << ",\"recall\":" << recall
                << ",\"f_measure\":" << f_measure << ",\"wall_ms\":" << wall_ms << ",\"notes\":[";
      for (size_t i = 0; i < estimates.size(); ++i) {
        if (i != 0) std::cout << ',';
        const TranscribedNote& note = estimates[i];
        std::cout << '[' << note.onset_sample << ',' << note.offset_sample << ','
                  << static_cast<int>(note.note) << ',' << static_cast<int>(note.velocity) << ','
                  << note.median_hz << ']';
      }
      std::cout << "]}" << std::endl;
      ++evaluated_cases;
    }
  }
  REQUIRE(evaluated_cases == excerpts.size() * sample_rates.size());
}

TEST_CASE("native monophonic onset frames match the frozen reference",
          "[.][superflux_native_oracle]") {
  const char* audio_dir = std::getenv("SONARE_SUPERFLUX_AUDIO_DIR");
  const char* oracle_path = std::getenv("SONARE_SUPERFLUX_ORACLE_JSON");
  REQUIRE(audio_dir != nullptr);
  REQUIRE(audio_dir[0] != '\0');
  REQUIRE(oracle_path != nullptr);
  REQUIRE(oracle_path[0] != '\0');

  const sonare::test::JsonValue oracle = sonare::test::JsonReader::parse_file(oracle_path);
  REQUIRE(oracle.is_object());
  REQUIRE(oracle.size() == 28);
  std::string directory(audio_dir);
  if (directory.back() != '/') directory.push_back('/');
  size_t total_expected_frames = 0;
  for (const auto& [track_id, values] : oracle.as_object()) {
    const sonare::Audio audio =
        sonare::Audio::from_file(directory + "vocadito_" + track_id + ".wav");
    REQUIRE(audio.sample_rate() == 44100);
    std::vector<int> expected;
    expected.reserve(values.size());
    for (const sonare::test::JsonValue& value : values.as_array()) {
      expected.push_back(value.as_int());
    }
    total_expected_frames += expected.size();
    const auto evidence = sonare::editing::note_model::detail::monophonic_onset_evidence(audio);
    INFO("audit track " << track_id << ", expected " << expected.size() << " onsets, got "
                        << evidence.frames.size());
    REQUIRE(evidence.available);
    CHECK(evidence.frames == expected);
  }
  REQUIRE(total_expected_frames == 2107);
}

TEST_CASE("monophonic onset reconciliation exports the audit notes",
          "[.][superflux_transcribe_replay]") {
  const char* audio_dir = std::getenv("SONARE_SUPERFLUX_AUDIO_DIR");
  const char* oracle_path = std::getenv("SONARE_SUPERFLUX_ORACLE_JSON");
  REQUIRE(audio_dir != nullptr);
  REQUIRE(audio_dir[0] != '\0');
  REQUIRE(oracle_path != nullptr);
  REQUIRE(oracle_path[0] != '\0');
  const sonare::test::JsonValue oracle = sonare::test::JsonReader::parse_file(oracle_path);
  REQUIRE(oracle.is_object());

  std::string directory(audio_dir);
  if (directory.back() != '/') directory.push_back('/');
  size_t exported_tracks = 0;
  for (const auto& entry : oracle.as_object()) {
    const std::string& track_id = entry.first;
    const sonare::Audio audio =
        sonare::Audio::from_file(directory + "vocadito_" + track_id + ".wav");
    const std::vector<TranscribedNote> notes = transcribe_notes(audio);
    std::cout << std::setprecision(17) << "ACOUSTIC_TRANSCRIBE {\"file\":\"audio/vocadito_"
              << track_id << ".wav\",\"sample_rate\":" << audio.sample_rate() << ",\"notes\":[";
    for (size_t i = 0; i < notes.size(); ++i) {
      if (i != 0) std::cout << ',';
      std::cout << '[' << static_cast<double>(notes[i].onset_sample) / audio.sample_rate() << ','
                << static_cast<double>(notes[i].offset_sample) / audio.sample_rate() << ','
                << notes[i].median_hz << ']';
    }
    std::cout << "]}" << std::endl;
    ++exported_tracks;
  }
  REQUIRE(exported_tracks == 28);
}
