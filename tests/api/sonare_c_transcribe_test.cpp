/// @file sonare_c_transcribe_test.cpp
/// @brief Tests for the transcription C ABI: the config door, the note-on /
///        note-off pairs the result carries, the tempo the PPQ grid was built
///        on, and the entry that writes straight into a project's MIDI clip.
///
/// The core is the oracle for what the notes are; this file is about what the C
/// layer does on top of it -- refusing a config it cannot read, spelling every
/// default as 0, pairing the notes, and placing them on a grid a caller's tempo
/// actually moves.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "util/constants.h"

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)

#include "c_api/project_internal.h"
#include "editing/note_model/note_transcriber.h"
#include "editing/polyphony/f0_salience.h"
#include "feature/pitch.h"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

namespace ntm = sonare::editing::note_model;

constexpr int kSampleRate = 22050;
constexpr int kToneSamples = kSampleRate / 4;       // 0.25 s
constexpr int kGapSamples = kSampleRate * 6 / 100;  // 0.06 s
constexpr float kEdgeSamples = 0.005f * kSampleRate;

/// UMP MIDI 1.0 channel-voice status nibbles.
constexpr uint32_t kNoteOff = 0x8u;
constexpr uint32_t kNoteOn = 0x9u;

float hz_for_midi(int note) {
  return sonare::constants::kA4Hz *
         std::pow(2.0f, static_cast<float>(note - 69) / sonare::constants::kSemitonesPerOctave);
}

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

/// @brief Separated tones, so each note has an onset of its own.
std::vector<float> separated_notes(const std::vector<int>& midi_notes) {
  std::vector<float> samples(static_cast<size_t>(kGapSamples), 0.0f);
  for (const int note : midi_notes) {
    append_tone(samples, hz_for_midi(note), 0.5f, kToneSamples);
    samples.insert(samples.end(), static_cast<size_t>(kGapSamples), 0.0f);
  }
  return samples;
}

/// @brief Adjacent tones with no silence between them, so one note's offset is
///        the next one's onset and the two land on the same tick.
std::vector<float> legato_notes(const std::vector<int>& midi_notes) {
  std::vector<float> samples;
  for (const int note : midi_notes) {
    append_tone(samples, hz_for_midi(note), 0.5f, kToneSamples);
  }
  return samples;
}

/// @brief Owns a result for the duration of a scope, so a failed assertion still
///        releases it.
class Result {
 public:
  Result() = default;
  Result(const Result&) = delete;
  Result& operator=(const Result&) = delete;
  ~Result() { sonare_free_transcribe_result(&value_); }

  SonareTranscribeResult* out() { return &value_; }
  const SonareTranscribeResult& get() const { return value_; }
  std::vector<SonareMidiEventPod> events() const {
    return std::vector<SonareMidiEventPod>(value_.events, value_.events + value_.count);
  }

 private:
  SonareTranscribeResult value_{};
};

struct Decoded {
  double ppq = 0.0;
  uint32_t type = 0;
  uint32_t group = 0;
  uint32_t status = 0;
  uint32_t channel = 0;
  uint8_t note = 0;
  uint8_t velocity = 0;
};

Decoded decode(const SonareMidiEventPod& pod) {
  Decoded out;
  out.ppq = pod.ppq;
  out.type = (pod.data0 >> 28) & 0xFu;
  out.group = (pod.data0 >> 24) & 0xFu;
  out.status = (pod.data0 >> 20) & 0xFu;
  out.channel = (pod.data0 >> 16) & 0xFu;
  out.note = static_cast<uint8_t>((pod.data0 >> 8) & 0x7Fu);
  out.velocity = static_cast<uint8_t>(pod.data0 & 0x7Fu);
  return out;
}

bool same_event(const SonareMidiEventPod& a, const SonareMidiEventPod& b) {
  return a.ppq == b.ppq && a.data0 == b.data0 && a.data1 == b.data1;
}

void require_same_events(const std::vector<SonareMidiEventPod>& actual,
                         const std::vector<SonareMidiEventPod>& expected) {
  REQUIRE(actual.size() == expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    INFO("event " << i);
    REQUIRE(same_event(actual[i], expected[i]));
  }
}

/// @brief Transcribes @p samples, requiring success and at least one note, so a
///        case about the events is never a case about an empty array.
void transcribe_into(Result* result, const std::vector<float>& samples, float tempo_bpm,
                     const SonareTranscribeConfig* config) {
  REQUIRE(sonare_transcribe(samples.data(), samples.size(), kSampleRate, tempo_bpm, config,
                            result->out()) == SONARE_OK);
  REQUIRE(result->get().note_count > 0);
  REQUIRE(result->get().count == 2u * result->get().note_count);
  REQUIRE(result->get().events != nullptr);
}

/// @brief The project's own tempo, which is the grid
///        @ref sonare_project_transcribe_to_clip builds on.
double project_tempo_of(const SonareProject* project) {
  const auto& segments = project->history.project().tempo_segments();
  return segments.empty() ? sonare::constants::kDefaultBpm : segments.front().bpm;
}

/// @brief A project holding one empty MIDI clip, released with the scope so a
///        failed assertion does not leak the handle.
class MidiFixture {
 public:
  MidiFixture() {
    REQUIRE(sonare_project_create(&project_) == SONARE_OK);
    REQUIRE(sonare_project_add_midi_clip(project_, 0.0, 16.0, &track_, &clip_) == SONARE_OK);
    REQUIRE(clip_ != 0);
  }
  MidiFixture(const MidiFixture&) = delete;
  MidiFixture& operator=(const MidiFixture&) = delete;
  ~MidiFixture() { sonare_project_destroy(project_); }

  SonareProject* project() const { return project_; }
  uint32_t clip() const { return clip_; }

 private:
  SonareProject* project_ = nullptr;
  uint32_t track_ = 0;
  uint32_t clip_ = 0;
};

std::vector<SonareMidiEventPod> clip_events_of(const MidiFixture& fixture) {
  const auto& store = fixture.project()->history.midi_content().events;
  const auto it = store.find(fixture.clip());
  std::vector<SonareMidiEventPod> events;
  if (it == store.end()) return events;
  for (const auto& event : it->second) {
    events.push_back(SonareMidiEventPod{event.ppq, event.data0, event.data1});
  }
  return events;
}

}  // namespace

// --- The config door ------------------------------------------------------

TEST_CASE("sonare_transcribe_config_default is the core's defaults, spelled out",
          "[c_api][transcribe]") {
  const SonareTranscribeConfig config = sonare_transcribe_config_default();
  const ntm::TranscribeConfig core;

  REQUIRE(config.struct_version == 3);
  CHECK(config.polyphonic == (core.source == ntm::TranscribeSource::kPolyphonic ? 1 : 0));
  CHECK(config.reference_hz == core.reference_hz);
  CHECK(config.fmin == core.fmin);
  CHECK(config.fmax == core.fmax);
  CHECK(config.min_note_ms == core.min_note_ms);
  CHECK(config.segmentation_threshold_cents == core.segmentation_threshold_cents);
  CHECK(config.velocity_floor_db == core.velocity_floor_db);
  CHECK(config.fixed_velocity == core.fixed_velocity);
  CHECK(config.group == 0);
  CHECK(config.channel == 0);

  CHECK(config.max_polyphony == core.max_polyphony);
  CHECK(config.min_frame_peak_ratio == core.min_frame_peak_ratio);
  CHECK(config.min_ridge_peak_ratio == core.min_ridge_peak_ratio);
  CHECK(config.reattack_ratio == core.reattack_ratio);

  // Every one of those is a value, not a zero the struct happened to arrive
  // with: a default seeder that stopped copying would leave these at 0. The
  // fields whose default depends on the source are the intentional exceptions:
  // the tracker endpoints, min_note_ms and the four polyphonic limits stay 0, so
  // the selected source supplies each when the config is read.
  CHECK(config.reference_hz > 0.0f);
  CHECK(config.fmin == 0.0f);
  CHECK(config.fmax == 0.0f);
  CHECK(config.min_note_ms == 0.0f);
  CHECK(config.max_polyphony == 0);
  CHECK(config.min_frame_peak_ratio == 0.0f);
  CHECK(config.min_ridge_peak_ratio == 0.0f);
  CHECK(config.reattack_ratio == 0.0f);
  CHECK(config.segmentation_threshold_cents > 0.0f);
  CHECK(config.velocity_floor_db < 0.0f);
}

TEST_CASE("the three spellings of the defaults are one answer", "[c_api][transcribe]") {
  const std::vector<float> samples = separated_notes({60, 64, 67, 72});

  Result by_null;
  transcribe_into(&by_null, samples, 120.0f, nullptr);

  const SonareTranscribeConfig explicit_defaults = sonare_transcribe_config_default();
  Result by_default;
  transcribe_into(&by_default, samples, 120.0f, &explicit_defaults);

  // The documented "0 => default" contract: a zeroed struct carrying only its
  // version is the defaults, field for field.
  SonareTranscribeConfig zeroed{};
  zeroed.struct_version = 1;
  Result by_zero;
  transcribe_into(&by_zero, samples, 120.0f, &zeroed);

  require_same_events(by_default.events(), by_null.events());
  require_same_events(by_zero.events(), by_null.events());
  CHECK(by_default.get().tempo_bpm == by_null.get().tempo_bpm);
  CHECK(by_zero.get().tempo_bpm == by_null.get().tempo_bpm);
}

TEST_CASE("sonare_transcribe refuses what it cannot read", "[c_api][transcribe]") {
  const std::vector<float> samples = separated_notes({60, 64});

  SECTION("the arguments") {
    Result result;
    CHECK(sonare_transcribe(samples.data(), samples.size(), kSampleRate, 120.0f, nullptr,
                            nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_transcribe(nullptr, samples.size(), kSampleRate, 120.0f, nullptr, result.out()) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_transcribe(samples.data(), 0, kSampleRate, 120.0f, nullptr, result.out()) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_transcribe(samples.data(), samples.size(), 0, 120.0f, nullptr, result.out()) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_transcribe(samples.data(), samples.size(), -44100, 120.0f, nullptr,
                            result.out()) == SONARE_ERROR_INVALID_PARAMETER);
    // The out struct is cleared before validation, so a refused call leaves
    // nothing for the caller to free and nothing to mistake for a result.
    CHECK(result.get().events == nullptr);
    CHECK(result.get().count == 0);
    CHECK(result.get().note_count == 0);
  }

  SECTION("a struct version that is not 1, 2 or 3") {
    for (const int32_t version : {0, 4, -1, 99}) {
      INFO("struct_version " << version);
      SonareTranscribeConfig config = sonare_transcribe_config_default();
      config.struct_version = version;
      Result result;
      CHECK(sonare_transcribe(samples.data(), samples.size(), kSampleRate, 120.0f, &config,
                              result.out()) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK(result.get().events == nullptr);
    }
  }

  SECTION("a field outside its domain, which is refused rather than replaced") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    std::vector<std::pair<const char*, SonareTranscribeConfig>> rejected;
    auto add = [&rejected](const char* what, SonareTranscribeConfig config) {
      config.struct_version = 1;
      rejected.emplace_back(what, config);
    };
    for (const float bad : {nan, inf, -inf, -1.0f}) {
      SonareTranscribeConfig reference{};
      reference.reference_hz = bad;
      add("reference_hz", reference);
      SonareTranscribeConfig low{};
      low.fmin = bad;
      add("fmin", low);
      SonareTranscribeConfig shortest{};
      shortest.min_note_ms = bad;
      add("min_note_ms", shortest);
      SonareTranscribeConfig threshold{};
      threshold.segmentation_threshold_cents = bad;
      add("segmentation_threshold_cents", threshold);
    }
    {
      SonareTranscribeConfig inverted{};
      inverted.fmin = 2093.0f;
      inverted.fmax = 65.0f;
      add("fmin above fmax", inverted);
    }
    for (const float bad : {nan, inf, 1.0f}) {
      SonareTranscribeConfig floor_db{};
      floor_db.velocity_floor_db = bad;
      add("velocity_floor_db", floor_db);
    }
    for (const int32_t bad : {-1, 128, 1000}) {
      SonareTranscribeConfig fixed{};
      fixed.fixed_velocity = bad;
      add("fixed_velocity", fixed);
    }
    for (const int32_t bad : {-1, 16, 99}) {
      SonareTranscribeConfig group{};
      group.group = bad;
      add("group", group);
      SonareTranscribeConfig channel{};
      channel.channel = bad;
      add("channel", channel);
    }

    for (const auto& entry : rejected) {
      INFO(entry.first);
      Result result;
      CHECK(sonare_transcribe(samples.data(), samples.size(), kSampleRate, 120.0f, &entry.second,
                              result.out()) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK(result.get().events == nullptr);
    }
  }
}

// --- The events -----------------------------------------------------------

TEST_CASE("the result is note-on / note-off pairs in canonical order", "[c_api][transcribe]") {
  const std::vector<float> samples = separated_notes({60, 64, 67, 72});
  Result result;
  transcribe_into(&result, samples, 120.0f, nullptr);

  const std::vector<SonareMidiEventPod> events = result.events();
  INFO("note_count " << result.get().note_count);
  REQUIRE(result.get().note_count == 4);

  size_t note_ons = 0;
  size_t note_offs = 0;
  for (size_t i = 0; i < events.size(); ++i) {
    const Decoded event = decode(events[i]);
    INFO("event " << i << ": status " << event.status << " note " << static_cast<int>(event.note)
                  << " velocity " << static_cast<int>(event.velocity) << " at ppq " << event.ppq);
    CHECK(event.type == 0x2u);
    CHECK(event.group == 0u);
    CHECK(event.channel == 0u);
    CHECK(event.ppq >= 0.0);
    if (event.status == kNoteOn) {
      ++note_ons;
      // A MIDI 1.0 note-on at velocity 0 is a note-off, which is why the core
      // floors velocity at 1.
      CHECK(event.velocity >= 1);
    } else {
      CHECK(event.status == kNoteOff);
      ++note_offs;
    }
    if (i > 0) CHECK(events[i].ppq >= events[i - 1].ppq);
  }
  CHECK(note_ons == result.get().note_count);
  CHECK(note_offs == result.get().note_count);

  // The note numbers are the ones synthesized, in order, which is what makes the
  // pairing above a statement about this take rather than about any four notes.
  std::vector<int> transcribed;
  for (const SonareMidiEventPod& pod : events) {
    const Decoded event = decode(pod);
    if (event.status == kNoteOn) transcribed.push_back(event.note);
  }
  REQUIRE(transcribed == std::vector<int>{60, 64, 67, 72});
}

TEST_CASE("a note-off sharing a tick with a note-on comes first", "[c_api][transcribe]") {
  // Legato material, so one note's offset IS the next one's onset. Without a
  // shared tick the ordering rule has nothing to order, which is why the shared
  // tick is required rather than looked for.
  const std::vector<float> samples = legato_notes({60, 64, 67});
  Result result;
  transcribe_into(&result, samples, 120.0f, nullptr);

  const std::vector<SonareMidiEventPod> events = result.events();
  size_t shared_ticks = 0;
  for (size_t i = 1; i < events.size(); ++i) {
    if (events[i].ppq != events[i - 1].ppq) continue;
    ++shared_ticks;
    const Decoded first = decode(events[i - 1]);
    const Decoded second = decode(events[i]);
    INFO("tick " << events[i].ppq << ": status " << first.status << " then " << second.status);
    // Equal ticks are ordered off-before-on; two events of the same kind at one
    // tick is not a case this material produces, but it is not an error either.
    CHECK_FALSE((first.status == kNoteOn && second.status == kNoteOff));
  }
  INFO("note_count " << result.get().note_count);
  REQUIRE(shared_ticks > 0);
}

// --- The tempo the grid is built on ---------------------------------------

TEST_CASE("the tempo is echoed when given and detected when not", "[c_api][transcribe]") {
  const std::vector<float> samples = separated_notes({60, 64, 67, 72});

  SECTION("a caller's tempo comes back unchanged") {
    for (const float bpm : {90.0f, 120.0f, 137.5f}) {
      INFO("bpm " << bpm);
      Result result;
      transcribe_into(&result, samples, bpm, nullptr);
      CHECK(result.get().tempo_bpm == bpm);
    }
  }

  SECTION("a non-tempo is detected") {
    for (const float bpm : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()}) {
      INFO("requested " << bpm);
      Result result;
      transcribe_into(&result, samples, bpm, nullptr);
      const float detected = result.get().tempo_bpm;
      INFO("detected " << detected);
      CHECK(std::isfinite(detected));
      CHECK(detected > 0.0f);
    }
  }
}

TEST_CASE("the tempo places the notes on the grid rather than being carried along",
          "[c_api][transcribe]") {
  const std::vector<float> samples = separated_notes({60, 64, 67, 72});

  Result slow;
  transcribe_into(&slow, samples, 120.0f, nullptr);
  Result fast;
  transcribe_into(&fast, samples, 240.0f, nullptr);

  const std::vector<SonareMidiEventPod> slow_events = slow.events();
  const std::vector<SonareMidiEventPod> fast_events = fast.events();
  REQUIRE(fast_events.size() == slow_events.size());

  for (size_t i = 0; i < slow_events.size(); ++i) {
    INFO("event " << i << ": " << slow_events[i].ppq << " at 120, " << fast_events[i].ppq
                  << " at 240");
    // The same instant in seconds is twice as many quarter notes at twice the
    // tempo. The events themselves are the same notes, so only the grid moved.
    CHECK(fast_events[i].data0 == slow_events[i].data0);
    CHECK_THAT(fast_events[i].ppq, WithinRel(2.0 * slow_events[i].ppq, 1.0e-6));
  }
  // A fixture whose first event sits at ppq 0 would satisfy the ratio for free.
  REQUIRE(slow_events.back().ppq > 0.0);
}

// --- Release --------------------------------------------------------------

TEST_CASE("sonare_free_transcribe_result accepts nothing to free", "[c_api][transcribe]") {
  sonare_free_transcribe_result(nullptr);

  SonareTranscribeResult zeroed{};
  sonare_free_transcribe_result(&zeroed);
  CHECK(zeroed.events == nullptr);
  CHECK(zeroed.count == 0);
  CHECK(zeroed.note_count == 0);
  CHECK(zeroed.tempo_bpm == 0.0f);

  // And a real result is emptied rather than merely released, so a caller that
  // keeps the struct cannot read a freed pointer out of it.
  const std::vector<float> samples = separated_notes({60, 64});
  SonareTranscribeResult result{};
  REQUIRE(sonare_transcribe(samples.data(), samples.size(), kSampleRate, 120.0f, nullptr,
                            &result) == SONARE_OK);
  REQUIRE(result.events != nullptr);
  sonare_free_transcribe_result(&result);
  CHECK(result.events == nullptr);
  CHECK(result.count == 0);
  CHECK(result.note_count == 0);
}

// --- Into a project's clip ------------------------------------------------

TEST_CASE("sonare_project_transcribe_to_clip writes the project's own grid",
          "[c_api][transcribe]") {
  const std::vector<float> samples = separated_notes({60, 64, 67, 72});
  const MidiFixture fixture;

  size_t note_count = 0;
  REQUIRE(sonare_project_transcribe_to_clip(fixture.project(), fixture.clip(), samples.data(),
                                            samples.size(), kSampleRate, nullptr,
                                            &note_count) == SONARE_OK);
  REQUIRE(note_count == 4);

  // The same audio through the standalone entry at the project's own tempo is
  // the oracle: the clip entry differs only in where the tempo comes from.
  Result standalone;
  transcribe_into(&standalone, samples, static_cast<float>(project_tempo_of(fixture.project())),
                  nullptr);

  const std::vector<SonareMidiEventPod> written = clip_events_of(fixture);
  const std::vector<SonareMidiEventPod> expected = standalone.events();
  REQUIRE(written.size() == expected.size());
  REQUIRE(written.size() == 2u * note_count);
  for (size_t i = 0; i < expected.size(); ++i) {
    INFO("event " << i);
    CHECK(written[i].data0 == expected[i].data0);
    CHECK(written[i].data1 == expected[i].data1);
    // The two grids are built from the same seconds through tempo maps prepared
    // at different sample rates, so the tick can differ by the project rate's
    // own rounding and no more.
    CHECK_THAT(written[i].ppq, WithinAbs(expected[i].ppq, 1.0e-3));
  }

  SECTION("and replaces the clip's events rather than adding to them") {
    SonareMidiEventPod existing[2];
    REQUIRE(sonare_midi_note_on(0.0, 0, 0, 100, 64, &existing[0]) == SONARE_OK);
    REQUIRE(sonare_midi_note_off(1.0, 0, 0, 100, 0, &existing[1]) == SONARE_OK);
    REQUIRE(sonare_project_set_midi_events(fixture.project(), fixture.clip(), existing, 2) ==
            SONARE_OK);
    REQUIRE(clip_events_of(fixture).size() == 2);

    REQUIRE(sonare_project_transcribe_to_clip(fixture.project(), fixture.clip(), samples.data(),
                                              samples.size(), kSampleRate, nullptr,
                                              &note_count) == SONARE_OK);
    const std::vector<SonareMidiEventPod> after = clip_events_of(fixture);
    REQUIRE(after.size() == 2u * note_count);
    // Note 100 was never in the take, so its survival would be the append.
    for (const SonareMidiEventPod& pod : after) CHECK(decode(pod).note != 100);
  }
}

TEST_CASE("sonare_project_transcribe_to_clip refuses what it cannot write", "[c_api][transcribe]") {
  const std::vector<float> samples = separated_notes({60, 64});
  const MidiFixture fixture;

  size_t note_count = 99;
  CHECK(sonare_project_transcribe_to_clip(nullptr, fixture.clip(), samples.data(), samples.size(),
                                          kSampleRate, nullptr,
                                          &note_count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(note_count == 0);
  CHECK(sonare_project_transcribe_to_clip(fixture.project(), fixture.clip(), nullptr,
                                          samples.size(), kSampleRate, nullptr,
                                          &note_count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_transcribe_to_clip(fixture.project(), fixture.clip(), samples.data(), 0,
                                          kSampleRate, nullptr,
                                          &note_count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_project_transcribe_to_clip(fixture.project(), fixture.clip(), samples.data(),
                                          samples.size(), 0, nullptr,
                                          &note_count) == SONARE_ERROR_INVALID_PARAMETER);

  SonareTranscribeConfig config = sonare_transcribe_config_default();
  config.struct_version = 4;
  CHECK(sonare_project_transcribe_to_clip(fixture.project(), fixture.clip(), samples.data(),
                                          samples.size(), kSampleRate, &config,
                                          &note_count) == SONARE_ERROR_INVALID_PARAMETER);

  // A clip id the project does not have. The transcription itself succeeds here
  // and the commit is what refuses, so this is where a written clip and a refused
  // one part company.
  CHECK(sonare_project_transcribe_to_clip(fixture.project(), fixture.clip() + 9999, samples.data(),
                                          samples.size(), kSampleRate, nullptr,
                                          &note_count) == SONARE_ERROR_INVALID_PARAMETER);

  // Nothing above wrote events, so a refusal is not a silent clear either.
  CHECK(clip_events_of(fixture).empty());
}

// --- Config fields that must reach the notes ------------------------------

TEST_CASE("a config field the C door forwards changes the events", "[c_api][transcribe]") {
  const std::vector<float> samples = separated_notes({60, 64, 67, 72});

  SECTION("fixed_velocity is every note-on's velocity") {
    SonareTranscribeConfig config = sonare_transcribe_config_default();
    config.fixed_velocity = 42;
    Result result;
    transcribe_into(&result, samples, 120.0f, &config);
    size_t note_ons = 0;
    for (const SonareMidiEventPod& pod : result.events()) {
      const Decoded event = decode(pod);
      if (event.status != kNoteOn) continue;
      ++note_ons;
      CHECK(event.velocity == 42);
    }
    REQUIRE(note_ons == result.get().note_count);

    // The measured velocities are not 42, so the case is not a coincidence.
    Result measured;
    transcribe_into(&measured, samples, 120.0f, nullptr);
    bool any_42 = false;
    for (const SonareMidiEventPod& pod : measured.events()) {
      const Decoded event = decode(pod);
      if (event.status == kNoteOn && event.velocity == 42) any_42 = true;
    }
    REQUIRE_FALSE(any_42);
  }

  SECTION("reference_hz moves every note number") {
    SonareTranscribeConfig config = sonare_transcribe_config_default();
    config.reference_hz = sonare::constants::kA4Hz * std::pow(2.0f, 1.0f / 12.0f);
    Result raised;
    transcribe_into(&raised, samples, 120.0f, &config);
    Result standard;
    transcribe_into(&standard, samples, 120.0f, nullptr);

    const std::vector<SonareMidiEventPod> raised_events = raised.events();
    const std::vector<SonareMidiEventPod> standard_events = standard.events();
    REQUIRE(raised_events.size() == standard_events.size());
    for (size_t i = 0; i < standard_events.size(); ++i) {
      INFO("event " << i);
      CHECK(decode(raised_events[i]).note + 1 == decode(standard_events[i]).note);
    }
  }

  SECTION("group and channel are the ones the events carry") {
    SonareTranscribeConfig config = sonare_transcribe_config_default();
    config.group = 3;
    config.channel = 9;
    Result result;
    transcribe_into(&result, samples, 120.0f, &config);
    for (const SonareMidiEventPod& pod : result.events()) {
      const Decoded event = decode(pod);
      CHECK(event.group == 3u);
      CHECK(event.channel == 9u);
    }
  }
}

TEST_CASE("the C door forwards the polyphonic tracker range", "[c_api][transcribe]") {
  const std::vector<float> samples = separated_notes({69});

  SonareTranscribeConfig inclusive = sonare_transcribe_config_default();
  inclusive.polyphonic = 1;
  inclusive.fmin = 400.0f;
  inclusive.fmax = 500.0f;
  Result in_range;
  transcribe_into(&in_range, samples, 120.0f, &inclusive);
  bool found_a4 = false;
  for (const SonareMidiEventPod& pod : in_range.events()) {
    const Decoded event = decode(pod);
    if (event.status == kNoteOn && event.note == 69) found_a4 = true;
  }
  REQUIRE(found_a4);

  SonareTranscribeConfig exclusive = inclusive;
  exclusive.fmin = 600.0f;
  exclusive.fmax = 700.0f;
  Result out_of_range;
  REQUIRE(sonare_transcribe(samples.data(), samples.size(), kSampleRate, 120.0f, &exclusive,
                            out_of_range.out()) == SONARE_OK);
  if (out_of_range.get().events != nullptr) {
    for (const SonareMidiEventPod& pod : out_of_range.events()) {
      const Decoded event = decode(pod);
      if (event.status == kNoteOn) CHECK(event.note != 69);
    }
  }
}

TEST_CASE("the C door resolves a partial tracker range from its source", "[c_api][transcribe]") {
  const std::vector<float> samples = separated_notes({69});

  SonareTranscribeConfig monophonic = sonare_transcribe_config_default();
  monophonic.fmin = 1800.0f;
  monophonic.fmax = 0.0f;
  Result mono_result;
  REQUIRE(sonare_transcribe(samples.data(), samples.size(), kSampleRate, 120.0f, &monophonic,
                            mono_result.out()) == SONARE_OK);

  SonareTranscribeConfig polyphonic = monophonic;
  polyphonic.polyphonic = 1;
  Result poly_result;
  CHECK(sonare_transcribe(samples.data(), samples.size(), kSampleRate, 120.0f, &polyphonic,
                          poly_result.out()) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(poly_result.get().events == nullptr);
  CHECK(std::string(sonare_last_error_message()) == "fmax must be above fmin");
}

// --- The tuning reference --------------------------------------------------

TEST_CASE("reference_auto measures the tuning and the result reports what was used",
          "[c_api][transcribe][tuning]") {
  // Every tone sits 0.3 semitone above equal temperament at A440.
  constexpr float kDetune = 0.3f;
  const float stretch = std::pow(2.0f, kDetune / sonare::constants::kSemitonesPerOctave);
  std::vector<float> samples(static_cast<size_t>(kGapSamples), 0.0f);
  for (const int note : {60, 64, 67, 72}) {
    append_tone(samples, hz_for_midi(note) * stretch, 0.5f, kToneSamples);
    samples.insert(samples.end(), static_cast<size_t>(kGapSamples), 0.0f);
  }

  SECTION("a given reference is reported as the tuning it amounts to") {
    SonareTranscribeConfig config = sonare_transcribe_config_default();
    float expected = 0.0f;
    REQUIRE(sonare_reference_hz_to_tuning(432.0f, 440.0f, &expected) == SONARE_OK);
    config.reference_hz = 432.0f;
    Result result;
    transcribe_into(&result, samples, 120.0f, &config);
    CHECK(result.get().struct_version == 2);
    CHECK_THAT(result.get().tuning, WithinAbs(expected, 1e-5));

    Result defaulted;
    transcribe_into(&defaulted, samples, 120.0f, nullptr);
    CHECK(defaulted.get().tuning == 0.0f);
  }

  SECTION("reference_auto reads the recording's own tuning, and the notes stay on their pitches") {
    SonareTranscribeConfig config = sonare_transcribe_config_default();
    config.reference_auto = 1;
    Result result;
    transcribe_into(&result, samples, 120.0f, &config);
    CHECK_THAT(result.get().tuning, WithinAbs(kDetune, 0.03));

    std::set<int> notes;
    for (const SonareMidiEventPod& pod : result.events()) {
      const Decoded event = decode(pod);
      if (event.status == kNoteOn) notes.insert(event.note);
    }
    CHECK(notes == std::set<int>{60, 64, 67, 72});

    // The flag is a version-2 field: a version-1 struct that carries it is read as not asking.
    config.struct_version = 1;
    Result v1;
    transcribe_into(&v1, samples, 120.0f, &config);
    CHECK(v1.get().tuning == 0.0f);
  }
}

// --- The version-3 polyphonic limits --------------------------------------

namespace {

/// @brief Two harmonic tones held together for 1 s, so the polyphonic chain has
///        a second voice for max_polyphony to withhold.
std::vector<float> held_chord() {
  constexpr int kPartials = 10;
  std::vector<float> samples(static_cast<size_t>(kSampleRate), 0.0f);
  for (const int note : {64, 71}) {
    const double f0 = static_cast<double>(hz_for_midi(note));
    for (int h = 1; h <= kPartials; ++h) {
      const double level = 0.2 / static_cast<double>(h);
      for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] += static_cast<float>(level * std::sin(sonare::constants::kTwoPiD * f0 * h *
                                                          static_cast<double>(i) / kSampleRate));
      }
    }
  }
  return samples;
}

/// @brief The most notes sounding at once, read off the canonical event order
///        (a note-off sharing a tick with a note-on comes first).
int most_sounding_at_once(const std::vector<float>& samples, const SonareTranscribeConfig& config) {
  Result result;
  REQUIRE(sonare_transcribe(samples.data(), samples.size(), kSampleRate, 120.0f, &config,
                            result.out()) == SONARE_OK);
  int sounding = 0;
  int most = 0;
  for (const SonareMidiEventPod& pod : result.events()) {
    sounding += decode(pod).status == kNoteOn ? 1 : -1;
    most = std::max(most, sounding);
  }
  return most;
}

}  // namespace

TEST_CASE("a version-3 struct reads the polyphonic limits and an older one does not",
          "[c_api][transcribe]") {
  const std::vector<float> samples = held_chord();
  SonareTranscribeConfig config = sonare_transcribe_config_default();
  config.polyphonic = 1;
  REQUIRE(most_sounding_at_once(samples, config) == 2);

  // One voice per frame, so no two ridges share a frame and no two notes overlap.
  config.max_polyphony = 1;
  CHECK(most_sounding_at_once(samples, config) == 1);

  SECTION("a version-2 struct ignores the trailing fields, valid or not") {
    config.struct_version = 2;
    CHECK(most_sounding_at_once(samples, config) == 2);
    // Out of every field's domain, and on a monophonic config: never read, so never refused.
    config.max_polyphony = 99;
    config.min_frame_peak_ratio = std::numeric_limits<float>::quiet_NaN();
    config.min_ridge_peak_ratio = 7.0f;
    config.reattack_ratio = 0.5f;
    CHECK(most_sounding_at_once(samples, config) == 2);
    config.polyphonic = 0;
    Result mono;
    CHECK(sonare_transcribe(samples.data(), samples.size(), kSampleRate, 120.0f, &config,
                            mono.out()) == SONARE_OK);
  }
}

TEST_CASE("a monophonic config carrying a polyphonic limit is refused by name",
          "[c_api][transcribe]") {
  const std::vector<float> samples = separated_notes({60, 64});
  struct Case {
    const char* field;
    void (*set)(SonareTranscribeConfig*);
  };
  const Case cases[] = {
      {"max_polyphony", [](SonareTranscribeConfig* c) { c->max_polyphony = 4; }},
      {"min_frame_peak_ratio", [](SonareTranscribeConfig* c) { c->min_frame_peak_ratio = -1.0f; }},
      {"min_frame_peak_ratio", [](SonareTranscribeConfig* c) { c->min_frame_peak_ratio = 0.3f; }},
      {"min_ridge_peak_ratio", [](SonareTranscribeConfig* c) { c->min_ridge_peak_ratio = -1.0f; }},
      {"min_ridge_peak_ratio", [](SonareTranscribeConfig* c) { c->min_ridge_peak_ratio = 0.3f; }},
      {"reattack_ratio", [](SonareTranscribeConfig* c) { c->reattack_ratio = -1.0f; }},
      {"reattack_ratio", [](SonareTranscribeConfig* c) { c->reattack_ratio = 2.5f; }},
  };
  for (const Case& entry : cases) {
    SonareTranscribeConfig config = sonare_transcribe_config_default();
    entry.set(&config);
    INFO(entry.field);
    Result result;
    CHECK(sonare_transcribe(samples.data(), samples.size(), kSampleRate, 120.0f, &config,
                            result.out()) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()).find(entry.field) != std::string::npos);
    CHECK(result.get().events == nullptr);

    // The same field on the polyphonic chain is a value, so the refusal is the source's.
    config.polyphonic = 1;
    Result polyphonic;
    CHECK(sonare_transcribe(samples.data(), samples.size(), kSampleRate, 120.0f, &config,
                            polyphonic.out()) == SONARE_OK);
  }
}

// Model: source {mono, poly} x max_polyphony {0, 1, 64, 65} x min_frame_peak_ratio
// {0, neg, 0.3, 1.5} x min_ridge_peak_ratio {0, neg, 0.3, 1.5} x reattack_ratio
// {0, neg, 0.5, 1.0, 2.5} x min_note_ms {0, 45}; pairwise (coverwise, seed 1): 26 cases.
TEST_CASE("the polyphonic limits refuse out of domain and resolve 0 as the explicit default",
          "[c_api][transcribe]") {
  struct Row {
    bool polyphonic;
    int32_t max_polyphony;
    float frame_ratio;
    float ridge_ratio;
    float reattack;
    float min_note_ms;
  };
  constexpr float kNeg = -1.0f;
  const Row rows[] = {
      {false, 64, 0.3f, 1.5f, 1.0f, 45.0f}, {true, 65, kNeg, 1.5f, kNeg, 0.0f},
      {true, 0, 0.3f, 0.0f, 0.0f, 45.0f},   {false, 1, 0.0f, 0.3f, 0.5f, 0.0f},
      {true, 0, 0.0f, kNeg, 1.0f, 0.0f},    {true, 1, 1.5f, 0.3f, 2.5f, 45.0f},
      {false, 0, kNeg, 0.3f, 1.0f, 45.0f},  {false, 65, 0.0f, kNeg, kNeg, 45.0f},
      {false, 64, 1.5f, kNeg, 0.0f, 0.0f},  {false, 1, kNeg, 0.0f, 2.5f, 45.0f},
      {true, 65, 1.5f, 0.0f, 0.5f, 45.0f},  {false, 65, 0.3f, 0.0f, 2.5f, 0.0f},
      {true, 65, kNeg, 0.3f, 0.0f, 0.0f},   {false, 1, 0.3f, kNeg, 1.0f, 45.0f},
      {true, 64, 0.0f, 0.0f, kNeg, 0.0f},   {true, 1, 0.0f, 1.5f, 0.0f, 45.0f},
      {true, 1, 0.3f, 0.0f, kNeg, 0.0f},    {false, 0, 1.5f, 1.5f, kNeg, 45.0f},
      {true, 0, 0.0f, kNeg, 2.5f, 45.0f},   {false, 64, kNeg, 1.5f, 2.5f, 45.0f},
      {true, 65, kNeg, kNeg, 0.5f, 0.0f},   {false, 64, 0.3f, 0.3f, 0.5f, 45.0f},
      {true, 0, 1.5f, 0.0f, 1.0f, 0.0f},    {true, 65, 0.3f, kNeg, 1.0f, 45.0f},
      {true, 0, 1.5f, 1.5f, 0.5f, 0.0f},    {true, 64, 0.0f, 0.3f, kNeg, 0.0f},
  };

  const std::vector<float> samples = separated_notes({60, 64});
  size_t accepted = 0;
  size_t refused = 0;
  for (size_t i = 0; i < std::size(rows); ++i) {
    const Row& row = rows[i];
    INFO("row " << i);
    SonareTranscribeConfig config = sonare_transcribe_config_default();
    config.polyphonic = row.polyphonic ? 1 : 0;
    config.max_polyphony = row.max_polyphony;
    config.min_frame_peak_ratio = row.frame_ratio;
    config.min_ridge_peak_ratio = row.ridge_ratio;
    config.reattack_ratio = row.reattack;
    config.min_note_ms = row.min_note_ms;

    // The fields that make the row unreadable: out of domain first, else the
    // polyphonic-only fields a monophonic row carries.
    std::vector<std::string> offending;
    if (row.max_polyphony == 65) offending.emplace_back("max_polyphony");
    if (row.frame_ratio > 1.0f) offending.emplace_back("min_frame_peak_ratio");
    if (row.ridge_ratio > 1.0f) offending.emplace_back("min_ridge_peak_ratio");
    if (row.reattack > 0.0f && row.reattack <= 1.0f) offending.emplace_back("reattack_ratio");
    if (offending.empty() && !row.polyphonic) {
      if (row.max_polyphony != 0) offending.emplace_back("max_polyphony");
      if (row.frame_ratio != 0.0f) offending.emplace_back("min_frame_peak_ratio");
      if (row.ridge_ratio != 0.0f) offending.emplace_back("min_ridge_peak_ratio");
      if (row.reattack != 0.0f) offending.emplace_back("reattack_ratio");
    }

    Result result;
    const SonareError error = sonare_transcribe(samples.data(), samples.size(), kSampleRate, 120.0f,
                                                &config, result.out());
    if (!offending.empty()) {
      ++refused;
      REQUIRE(error == SONARE_ERROR_INVALID_PARAMETER);
      const std::string message = sonare_last_error_message();
      INFO("message: " << message);
      CHECK(std::any_of(offending.begin(), offending.end(), [&](const std::string& field) {
        return message.find(field) != std::string::npos;
      }));
      CHECK(result.get().events == nullptr);
      continue;
    }

    ++accepted;
    REQUIRE(error == SONARE_OK);
    // Each 0 is the core's resolved default spelled out; a negative stays negative,
    // since the struct has no other spelling of a real 0 or of "no split".
    ntm::TranscribeConfig source;
    source.source = ntm::TranscribeSource::kPolyphonic;
    const ntm::TranscribePolyphonyLimits defaults =
        ntm::resolve_transcribe_polyphony_limits(source);
    SonareTranscribeConfig spelled = config;
    if (spelled.max_polyphony == 0) spelled.max_polyphony = defaults.max_polyphony;
    if (spelled.min_frame_peak_ratio == 0.0f) {
      spelled.min_frame_peak_ratio = defaults.min_frame_peak_ratio;
    }
    if (spelled.min_ridge_peak_ratio == 0.0f) {
      spelled.min_ridge_peak_ratio = defaults.min_ridge_peak_ratio;
    }
    if (spelled.reattack_ratio == 0.0f) spelled.reattack_ratio = defaults.reattack_ratio;
    if (spelled.min_note_ms == 0.0f) spelled.min_note_ms = defaults.min_note_ms;
    Result explicit_result;
    REQUIRE(sonare_transcribe(samples.data(), samples.size(), kSampleRate, 120.0f, &spelled,
                              explicit_result.out()) == SONARE_OK);
    require_same_events(result.events(), explicit_result.events());
  }
  // Both outcomes are reached, so neither half of the table is vacuous.
  CHECK(accepted == 5);
  CHECK(refused == 21);
}

#endif  // SONARE_WITH_ARRANGEMENT && SONARE_WITH_PITCH_EDITOR
