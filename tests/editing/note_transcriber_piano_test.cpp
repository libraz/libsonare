/// @file note_transcriber_piano_test.cpp
/// @brief Polyphonic transcription on material shaped like a piano note.
///
/// Each case is a synthetic signal built to carry one measured property of a
/// sampled piano -- a third partial over the fundamental, a tail far under the
/// note, a note over the tracker range -- so the assertion is about
/// what the transcriber reports and not about a rendering.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "core/audio.h"
#include "editing/note_model/note_transcriber.h"
#include "util/constants.h"

using namespace sonare::editing::note_model;

namespace {

constexpr int kSampleRate = 44100;

float hz_for_midi(int note) {
  return sonare::constants::kA4Hz *
         std::pow(2.0f, static_cast<float>(note - 69) / sonare::constants::kSemitonesPerOctave);
}

/// @brief A harmonic tone with a piano's partial envelope (measured on a
///        sampled A2: the third partial 1.6 times the fundamental, the second
///        and fourth weak), @p seconds long at @p level, with 5 ms edges.
std::vector<float> piano_tone(int midi_note, double seconds, float level) {
  constexpr float kEnvelope[] = {1.0f,  0.17f, 1.64f, 0.03f, 0.06f,
                                 0.23f, 0.12f, 0.01f, 0.22f, 0.16f};
  constexpr double kEdgeSec = 0.005;
  const double f0 = hz_for_midi(midi_note);
  std::vector<float> samples(static_cast<size_t>(seconds * kSampleRate), 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    const double edge = std::min(1.0, std::min(t, seconds - t) / kEdgeSec);
    double sample = 0.0;
    for (int h = 1; h <= 10; ++h) {
      const double hz = f0 * h;
      if (hz >= 0.5 * kSampleRate) break;
      sample += kEnvelope[h - 1] * std::sin(sonare::constants::kTwoPiD * hz * t + 0.37 * h * h);
    }
    samples[i] = static_cast<float>(level * edge * sample);
  }
  return samples;
}

size_t count_of(const std::vector<TranscribedNote>& notes, int midi_note) {
  size_t count = 0;
  for (const TranscribedNote& note : notes) count += note.note == midi_note;
  return count;
}

TranscribeConfig polyphonic() {
  TranscribeConfig config;
  config.source = TranscribeSource::kPolyphonic;
  return config;
}

}  // namespace

TEST_CASE("a held note with a dominant third partial is one note and no twelfth",
          "[note_transcriber][piano]") {
  constexpr int kA2 = 45;
  const std::vector<float> samples = piano_tone(kA2, 2.0, 0.2f);
  const std::vector<TranscribedNote> notes = transcribe_notes(
      sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate), polyphonic());

  INFO("notes " << notes.size());
  CHECK(count_of(notes, kA2) == 1);
  CHECK(count_of(notes, kA2 + 19) == 0);
  CHECK(count_of(notes, kA2 + 12) == 0);
  REQUIRE(notes.size() == 1);
  // The one note spans the tone: within two hops at either end.
  constexpr int64_t kTwoHops = 2 * 512;
  CHECK(notes[0].onset_sample <= kTwoHops);
  CHECK(notes[0].offset_sample >= static_cast<int64_t>(samples.size()) - kTwoHops);
}

TEST_CASE("material far under the take's own notes is not a note", "[note_transcriber][piano]") {
  // A note, then a tail about 66 dB under it: the tracker finds ridges in it all
  // the same, because its thresholds are relative, and the level gate says no.
  constexpr int kC4 = 60;
  std::vector<float> samples = piano_tone(kC4, 0.6, 0.2f);
  const std::vector<float> tail = piano_tone(kC4, 1.5, 1e-4f);
  samples.insert(samples.end(), tail.begin(), tail.end());
  const sonare::Audio audio =
      sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate);

  TranscribeConfig config = polyphonic();
  const std::vector<TranscribedNote> notes = transcribe_notes(audio, config);
  REQUIRE(notes.size() == 1);
  CHECK(notes[0].note == kC4);
  CHECK(notes[0].velocity > 1);
  CHECK(notes[0].offset_sample <= static_cast<int64_t>(0.7 * kSampleRate));

  SECTION("the gate decides, not the mapping: a fixed velocity drops it too") {
    config.fixed_velocity = 100;
    REQUIRE(transcribe_notes(audio, config).size() == 1);
  }

  SECTION("the velocity floor does not decide: one under the tail's level still drops it") {
    config.velocity_floor_db = -100.0f;
    REQUIRE(transcribe_notes(audio, config).size() == 1);
  }

  SECTION("the same take 40 dB quieter keeps its note, at velocity 1") {
    std::vector<float> quiet = samples;
    for (float& v : quiet) v *= 0.01f;
    const std::vector<TranscribedNote> quiet_notes = transcribe_notes(
        sonare::Audio::from_buffer(quiet.data(), quiet.size(), kSampleRate), config);
    REQUIRE(quiet_notes.size() == 1);
    CHECK(quiet_notes[0].note == kC4);
    CHECK(quiet_notes[0].onset_sample == notes[0].onset_sample);
    // About -51 dBFS of peak RMS, under the -48 dB velocity floor.
    CHECK(quiet_notes[0].velocity == 1);
  }

  SECTION("noise under -80 dBFS alone yields nothing") {
    std::vector<float> noise(static_cast<size_t>(2 * kSampleRate));
    uint32_t state = 0x9e3779b9u;
    for (float& v : noise) {
      state = state * 1664525u + 1013904223u;
      v = 1e-4f * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
    }
    REQUIRE(transcribe_notes(sonare::Audio::from_buffer(noise.data(), noise.size(), kSampleRate),
                             polyphonic())
                .empty());
  }
}

TEST_CASE("a note over fmax is dropped, not reported an octave low", "[note_transcriber][piano]") {
  constexpr int kC7 = 96;
  const std::vector<float> samples = piano_tone(kC7, 1.0, 0.2f);
  const sonare::Audio audio =
      sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate);

  // The polyphonic default ceiling is A6, under C7.
  REQUIRE(resolve_transcribe_f0_range(polyphonic()).fmax < hz_for_midi(kC7));
  const std::vector<TranscribedNote> notes = transcribe_notes(audio, polyphonic());
  CHECK(count_of(notes, kC7 - 12) == 0);
  CHECK(notes.empty());

  SECTION("raising fmax over the note reports it as itself") {
    TranscribeConfig config = polyphonic();
    config.fmax = 4200.0f;
    const std::vector<TranscribedNote> raised = transcribe_notes(audio, config);
    REQUIRE(count_of(raised, kC7) == 1);
    CHECK(count_of(raised, kC7 - 12) == 0);
  }
}

TEST_CASE("an isolated note's onset is read at its strike, not where the window first reaches it",
          "[note_transcriber][piano]") {
  // Silence, then the tone: with nothing else sounding, a candidate appears as
  // soon as the window's edge touches the tone, up to three hops before the strike.
  constexpr double kLeadSec = 0.3;
  constexpr int kG4 = 67;
  std::vector<float> samples(static_cast<size_t>(kLeadSec * kSampleRate), 0.0f);
  const std::vector<float> tone = piano_tone(kG4, 1.0, 0.2f);
  samples.insert(samples.end(), tone.begin(), tone.end());
  const std::vector<TranscribedNote> notes = transcribe_notes(
      sonare::Audio::from_buffer(samples.data(), samples.size(), kSampleRate), polyphonic());

  REQUIRE(notes.size() == 1);
  CHECK(notes[0].note == kG4);
  // Within one hop of the strike either way.
  constexpr int64_t kHop = 512;
  const auto strike = static_cast<int64_t>(kLeadSec * kSampleRate);
  INFO("onset " << notes[0].onset_sample << " strike " << strike);
  CHECK(std::llabs(notes[0].onset_sample - strike) <= kHop);
}
