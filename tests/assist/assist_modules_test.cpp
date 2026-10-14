/// @file assist_modules_test.cpp
/// @brief The built-in assist modules against a real project: the placement
///        judge's three rules, the dissonance ranking, and the two generators.
///        Tag: [assist][modules].
///
/// The modules are rule-based and allocate no randomness, so every case here
/// asserts the notes that come out rather than only that a run completed. The
/// two rules with the most room to go quietly wrong are pinned hardest: a judge
/// that ADJUSTS for register and key but REFUSES for harmony, and a harmonizer
/// whose "third below" is a scale step and not a fixed interval.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_history.h"
#include "arrangement/edit_model.h"
#include "arrangement/harmonic_timeline.h"
#include "arrangement/project_view.h"
#include "midi/assist/assist_budget_detail.h"
#include "midi/assist/assist_registry.h"
#include "midi/assist/clip_ppq.h"
#include "midi/assist/composition_assist.h"
#include "midi/assist/i_harmony_context.h"
#include "midi/assist/i_note_generator.h"
#include "midi/assist/modules/chord_tone_generator.h"
#include "midi/assist/modules/diatonic_harmonizer.h"
#include "midi/assist/modules/dissonance_analyzer.h"
#include "midi/assist/modules/generator_params.h"
#include "midi/assist/modules/harmony_context.h"
#include "midi/assist/modules/music_theory.h"
#include "midi/assist/modules/placement_judge.h"
#include "midi/ump.h"
#include "serialize/project_serializer.h"
#include "util/json.h"

namespace {

namespace json = sonare::util::json;

using sonare::arrangement::AddClip;
using sonare::arrangement::AddTrack;
using sonare::arrangement::AttachMidiSource;
using sonare::arrangement::ChordQuality;
using sonare::arrangement::ChordSymbol;
using sonare::arrangement::ClipId;
using sonare::arrangement::EditClip;
using sonare::arrangement::EditHistory;
using sonare::arrangement::KeyMode;
using sonare::arrangement::KeySegment;
using sonare::arrangement::MidiClipEvent;
using sonare::arrangement::MidiClipEventList;
using sonare::arrangement::MidiClipPatch;
using sonare::arrangement::MidiSourceRef;
using sonare::arrangement::PatchMidiClip;
using sonare::arrangement::ProjectAnnotation;
using sonare::arrangement::ProjectView;
using sonare::arrangement::ReplaceMidiClipEvents;
using sonare::arrangement::SetAnnotation;
using sonare::arrangement::Track;
using sonare::arrangement::TrackId;
using sonare::midi::assist::AssistQueryContext;
using sonare::midi::assist::AssistRegistry;
using sonare::midi::assist::AssistRequest;
using sonare::midi::assist::AssistResult;
using sonare::midi::assist::AssistStatus;
using sonare::midi::assist::CandidateNote;
using sonare::midi::assist::clip_content_to_timeline_ppq;
using sonare::midi::assist::CompositionAssist;
using sonare::midi::assist::IHarmonyContext;
using sonare::midi::assist::INoteGenerator;
using sonare::midi::assist::PlacementVerdict;
using sonare::midi::assist::timeline_to_clip_content_ppq;
using sonare::midi::assist::VoiceModel;
using sonare::midi::assist::modules::ChordToneGenerator;
using sonare::midi::assist::modules::ChordToneGeneratorConfig;
using sonare::midi::assist::modules::DiatonicHarmonizer;
using sonare::midi::assist::modules::DiatonicHarmonizerConfig;
using sonare::midi::assist::modules::GeneratorParams;
using sonare::midi::assist::modules::IntervalDissonanceAnalyzer;
using sonare::midi::assist::modules::RangeScaleJudge;
using sonare::midi::assist::modules::RangeScaleJudgeConfig;
using sonare::midi::assist::modules::read_generator_params;
using sonare::midi::assist::modules::TimelineHarmonyContext;

constexpr double kClipLengthPpq = 1920.0;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

/// One MIDI track carrying one clip, built through the real command layer so the
/// modules read the same ProjectView a host would hand them.
struct Fixture {
  EditHistory history;
  TrackId track_id = 0;
  ClipId clip_id = 0;

  ProjectView view() const {
    return ProjectView(history.project(), history.midi_content(), "test");
  }
  std::string serialized() const {
    return sonare::serialize::project_to_json(history.project(), history.midi_content());
  }
};

Fixture make_fixture() {
  Fixture f;
  auto add_track = std::make_unique<AddTrack>([] {
    Track t;
    t.name = "lead";
    t.kind = Track::Kind::kMidi;
    return t;
  }());
  auto* add_track_ptr = add_track.get();
  REQUIRE(f.history.apply(std::move(add_track)));
  f.track_id = add_track_ptr->allocated_id();

  MidiSourceRef ref;
  ref.name = "lead-src";
  auto attach = std::make_unique<AttachMidiSource>(ref);
  auto* attach_ptr = attach.get();
  REQUIRE(f.history.apply(std::move(attach)));

  EditClip clip;
  clip.track_id = f.track_id;
  clip.source_id = attach_ptr->allocated_id();
  clip.start_ppq = 0.0;
  clip.length_ppq = kClipLengthPpq;
  auto add_clip = std::make_unique<AddClip>(clip);
  auto* add_clip_ptr = add_clip.get();
  REQUIRE(f.history.apply(std::move(add_clip)));
  f.clip_id = add_clip_ptr->allocated_id();
  return f;
}

void annotate(Fixture* f, std::vector<KeySegment> keys, std::vector<ChordSymbol> chords) {
  ProjectAnnotation annotation = f->history.project().annotation();
  annotation.keys = std::move(keys);
  annotation.chords = std::move(chords);
  REQUIRE(f->history.apply(std::make_unique<SetAnnotation>(std::move(annotation))));
}

KeySegment key_segment(double start_ppq, double end_ppq, uint8_t tonic_pc, KeyMode mode) {
  KeySegment key;
  key.start_ppq = start_ppq;
  key.end_ppq = end_ppq;
  key.tonic_pc = tonic_pc;
  key.mode = mode;
  return key;
}

ChordSymbol chord_symbol(double start_ppq, double end_ppq, uint8_t root_pc, ChordQuality quality) {
  ChordSymbol chord;
  chord.start_ppq = start_ppq;
  chord.end_ppq = end_ppq;
  chord.root_pc = root_pc;
  chord.quality = quality;
  return chord;
}

MidiClipEvent note_on_event(double ppq, uint8_t note, uint8_t velocity, uint8_t group = 0,
                            uint8_t channel = 0) {
  const sonare::midi::Ump ump = sonare::midi::make_midi1_note_on(group, channel, note, velocity);
  return MidiClipEvent{ppq, ump.words[0], ump.words[1], 0};
}

MidiClipEvent note_off_event(double ppq, uint8_t note, uint8_t group = 0, uint8_t channel = 0) {
  const sonare::midi::Ump ump = sonare::midi::make_midi1_note_off(group, channel, note, 0);
  return MidiClipEvent{ppq, ump.words[0], ump.words[1], 0};
}

MidiClipEvent midi2_note_on_event(double ppq, uint8_t note, uint16_t velocity16, uint8_t group = 0,
                                  uint8_t channel = 0) {
  const sonare::midi::Ump ump = sonare::midi::make_midi2_note_on(group, channel, note, velocity16);
  return MidiClipEvent{ppq, ump.words[0], ump.words[1], 0};
}

MidiClipEvent midi2_note_off_event(double ppq, uint8_t note, uint8_t group = 0,
                                   uint8_t channel = 0) {
  const sonare::midi::Ump ump = sonare::midi::make_midi2_note_off(group, channel, note, 0);
  return MidiClipEvent{ppq, ump.words[0], ump.words[1], 0};
}

/// Four rising notes of the C major triad plus its octave, one per beat.
MidiClipEventList c_major_line(uint8_t velocity = 100) {
  MidiClipEventList events;
  const uint8_t notes[] = {60, 64, 67, 72};
  for (int i = 0; i < 4; ++i) {
    const double on_ppq = i * 480.0;
    events.push_back(note_on_event(on_ppq, notes[i], velocity));
    events.push_back(note_off_event(on_ppq + 240.0, notes[i]));
  }
  return events;
}

void set_events(Fixture* f, MidiClipEventList events) {
  REQUIRE(f->history.apply(std::make_unique<ReplaceMidiClipEvents>(f->clip_id, std::move(events))));
}

void set_events(Fixture* f, ClipId clip_id, MidiClipEventList events) {
  REQUIRE(f->history.apply(std::make_unique<ReplaceMidiClipEvents>(clip_id, std::move(events))));
}

void set_clip_position(Fixture* f, ClipId clip_id, double start_ppq, double source_offset_ppq) {
  EditClip* clip = f->history.project().find_clip_mutable(clip_id);
  REQUIRE(clip != nullptr);
  clip->start_ppq = start_ppq;
  clip->source_offset_ppq = source_offset_ppq;
  clip->length_ppq = 4000.0;
}

ClipId add_midi_clip(Fixture* f) {
  Track target_track;
  target_track.name = "target";
  target_track.kind = Track::Kind::kMidi;
  auto add_track = std::make_unique<AddTrack>(target_track);
  auto* add_track_ptr = add_track.get();
  REQUIRE(f->history.apply(std::move(add_track)));

  MidiSourceRef target_source;
  target_source.name = "target-src";
  auto attach = std::make_unique<AttachMidiSource>(target_source);
  auto* attach_ptr = attach.get();
  REQUIRE(f->history.apply(std::move(attach)));

  EditClip target;
  target.track_id = add_track_ptr->allocated_id();
  target.source_id = attach_ptr->allocated_id();
  target.start_ppq = 4000.0;
  target.length_ppq = 4000.0;
  auto add_clip = std::make_unique<AddClip>(target);
  auto* add_clip_ptr = add_clip.get();
  REQUIRE(f->history.apply(std::move(add_clip)));
  return add_clip_ptr->allocated_id();
}

// ---------------------------------------------------------------------------
// Reading a module's proposal back
// ---------------------------------------------------------------------------

struct EmittedNote {
  double on_ppq = 0.0;
  double off_ppq = 0.0;
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;
  uint8_t velocity = 0;
};

const PatchMidiClip* sole_patch(const AssistResult& result) {
  REQUIRE(result.commands.size() == 1u);
  const auto* patch = dynamic_cast<const PatchMidiClip*>(result.commands.front().get());
  REQUIRE(patch != nullptr);
  return patch;
}

/// Pairs the patch's note-ons with their note-offs in emission order. A patch
/// whose events do not pair up fails here rather than being summarised away.
std::vector<EmittedNote> emitted_notes(const PatchMidiClip& patch) {
  using NoteKey = std::tuple<uint8_t, uint8_t, uint8_t>;
  std::map<NoteKey, std::vector<EmittedNote>> open;
  std::vector<EmittedNote> notes;
  for (const MidiClipEvent& event : patch.patch().add) {
    sonare::midi::Ump ump;
    ump.words[0] = event.data0;
    ump.words[1] = event.data1;
    ump.word_count = sonare::midi::ump_word_count_for_word0(event.data0);
    ump.group = sonare::midi::ump_group_from_word0(event.data0);
    if (ump.is_note_off()) {
      const NoteKey key{ump.group, ump.channel(), ump.note_number()};
      const auto it = open.find(key);
      REQUIRE(it != open.end());
      REQUIRE_FALSE(it->second.empty());
      EmittedNote note = it->second.back();
      it->second.pop_back();
      if (it->second.empty()) open.erase(it);
      note.off_ppq = event.ppq;
      notes.push_back(note);
    } else {
      REQUIRE(ump.is_note_on());
      open[NoteKey{ump.group, ump.channel(), ump.note_number()}].push_back(EmittedNote{
          event.ppq, 0.0, ump.group, ump.channel(), ump.note_number(), ump.data2_7bit()});
    }
  }
  CHECK(open.empty());
  return notes;
}

std::vector<int> note_numbers(const std::vector<EmittedNote>& notes) {
  std::vector<int> out;
  out.reserve(notes.size());
  for (const EmittedNote& note : notes) out.push_back(static_cast<int>(note.note));
  return out;
}

bool reason_mentions(const std::string& reason, const std::string& fragment) {
  return reason.find(fragment) != std::string::npos;
}

std::vector<bool> decision_acceptances(const AssistResult& result) {
  const json::Value payload = json::parse_strict(result.candidate_payload);
  REQUIRE(payload.contains("decisions"));
  REQUIRE(payload["decisions"].is_array());
  std::vector<bool> accepted;
  for (const json::Value& decision : payload["decisions"].as_array()) {
    REQUIRE(decision.contains("accepted"));
    accepted.push_back(decision["accepted"].as_bool());
  }
  return accepted;
}

AssistRequest request_with(const std::string& params_json) {
  AssistRequest request;
  request.params_json = params_json;
  return request;
}

std::string target_params(ClipId clip_id) {
  return "{\"target_clip_id\": " + std::to_string(clip_id) + "}";
}

std::string source_target_params(ClipId target_clip_id, ClipId source_clip_id) {
  return "{\"target_clip_id\": " + std::to_string(target_clip_id) +
         ", \"source_clip_id\": " + std::to_string(source_clip_id) + "}";
}

constexpr char kStubRefusal[] = "the host module will not answer this request";

/// A host-installed module, standing in for the one thing no built-in can be:
/// a module whose verdict on a request differs from the other slot's. Both
/// built-ins read the same params blob through one reader, so a params fault
/// fells every one of them at once and the mixed case is unreachable with them.
class StubGenerator final : public INoteGenerator {
 public:
  StubGenerator(ClipId clip_id, AssistStatus status) noexcept
      : clip_id_(clip_id), status_(status) {}

  const char* module_id() const noexcept override { return "test.stub_generator"; }

  AssistResult generate(const ProjectView& view, const AssistRequest& request) override {
    (void)view;
    (void)request;
    AssistResult result;
    result.diagnostics.status = status_;
    result.diagnostics.iterations_consumed = 1;
    if (status_ == AssistStatus::kRejected) {
      result.diagnostics.reason = kStubRefusal;
      return result;
    }
    MidiClipPatch patch;
    patch.clip_id = clip_id_;
    patch.add.push_back(note_on_event(0.0, 48, 90));
    patch.add.push_back(note_off_event(240.0, 48));
    result.commands.push_back(std::make_unique<PatchMidiClip>(std::move(patch)));
    result.candidate_payload = "{\"module\":\"test.stub_generator\"}";
    result.candidate_payloads.push_back(result.candidate_payload);
    return result;
  }

 private:
  ClipId clip_id_;
  AssistStatus status_;
};

/// A test-only oracle that records which timeline positions a harmonizer asks
/// about while delegating the answer to the project's real timeline.
class RecordingHarmony final : public IHarmonyContext {
 public:
  ChordSymbol chord_at(const ProjectView& view, double ppq) const override {
    return delegate_.chord_at(view, ppq);
  }

  KeySegment key_at(const ProjectView& view, double ppq) const override {
    return delegate_.key_at(view, ppq);
  }

  std::vector<uint8_t> scale_pitch_classes(const ProjectView& view, double ppq) const override {
    scale_queries.push_back(ppq);
    return delegate_.scale_pitch_classes(view, ppq);
  }

  mutable std::vector<double> scale_queries;

 private:
  TimelineHarmonyContext delegate_;
};

/// A test-only judge that records timeline onset and can move accepted notes.
class RecordingJudge final : public sonare::midi::assist::INotePlacementJudge {
 public:
  PlacementVerdict judge(const ProjectView& view, const VoiceModel& voice,
                         const CandidateNote& candidate) const override {
    (void)view;
    (void)voice;
    seen_ppq.push_back(candidate.ppq);
    PlacementVerdict verdict;
    verdict.accepted = true;
    verdict.adjusted = candidate;
    verdict.adjusted.ppq += ppq_adjustment;
    verdict.reason = "recorded timeline onset";
    return verdict;
  }

  mutable std::vector<double> seen_ppq;
  double ppq_adjustment = 0.0;
};

/// A deterministic judge script for the generator's per-voice acceptance
/// diagnostic. Rejected voices still appear in the candidate payload, but only
/// voices whose patch events were actually emitted make the chord accepted.
class ScriptedJudge final : public sonare::midi::assist::INotePlacementJudge {
 public:
  explicit ScriptedJudge(std::vector<bool> accepted) noexcept : accepted_(std::move(accepted)) {}

  PlacementVerdict judge(const ProjectView& view, const VoiceModel& voice,
                         const CandidateNote& candidate) const override {
    (void)view;
    (void)voice;
    const bool accepted = calls_ < accepted_.size() && accepted_[calls_++];
    PlacementVerdict verdict;
    verdict.accepted = accepted;
    verdict.adjusted = candidate;
    verdict.reason = accepted ? "script accepted" : "script rejected";
    return verdict;
  }

 private:
  std::vector<bool> accepted_;
  mutable size_t calls_ = 0;
};

sonare::midi::assist::detail::AssistTimePoint g_assist_test_now;
int g_assist_test_reads_until_expiry = -1;

sonare::midi::assist::detail::AssistTimePoint assist_test_now() noexcept {
  if (g_assist_test_reads_until_expiry == 0) {
    g_assist_test_now += std::chrono::milliseconds(100);
    g_assist_test_reads_until_expiry = -1;
  } else if (g_assist_test_reads_until_expiry > 0) {
    --g_assist_test_reads_until_expiry;
  }
  return g_assist_test_now;
}

/// A deterministic judge clock used to exercise the cooperative deadline at a
/// candidate boundary without sleeping or depending on wall-clock scheduling.
class AdvancingJudge final : public sonare::midi::assist::INotePlacementJudge {
 public:
  explicit AdvancingJudge(std::chrono::milliseconds advance) noexcept : advance_(advance) {}

  PlacementVerdict judge(const ProjectView& view, const VoiceModel& voice,
                         const CandidateNote& candidate) const override {
    (void)view;
    (void)voice;
    g_assist_test_now += advance_;
    PlacementVerdict verdict;
    verdict.accepted = true;
    verdict.adjusted = candidate;
    verdict.reason = "clock-advancing test judge";
    return verdict;
  }

 private:
  std::chrono::milliseconds advance_;
};

class ArmDeadlineAfterFirstJudge final : public sonare::midi::assist::INotePlacementJudge {
 public:
  PlacementVerdict judge(const ProjectView& view, const VoiceModel& voice,
                         const CandidateNote& candidate) const override {
    (void)view;
    (void)voice;
    if (!armed_) {
      armed_ = true;
      g_assist_test_reads_until_expiry = 1;
    }
    PlacementVerdict verdict;
    verdict.accepted = true;
    verdict.adjusted = candidate;
    verdict.reason = "deadline-arm test judge";
    return verdict;
  }

 private:
  mutable bool armed_ = false;
};

}  // namespace

// ===========================================================================
// TimelineHarmonyContext
// ===========================================================================

TEST_CASE("TimelineHarmonyContext serves the project's own annotations", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(
      &f,
      {key_segment(0.0, 960.0, 0, KeyMode::kMajor), key_segment(960.0, 1920.0, 9, KeyMode::kMinor)},
      {chord_symbol(0.0, 960.0, 0, ChordQuality::kMajor),
       chord_symbol(960.0, 1920.0, 7, ChordQuality::kDominant)});
  const TimelineHarmonyContext harmony;
  const ProjectView view = f.view();

  CHECK(harmony.key_at(view, 0.0).tonic_pc == 0);
  CHECK(harmony.key_at(view, 0.0).mode == KeyMode::kMajor);
  CHECK(harmony.key_at(view, 1000.0).tonic_pc == 9);
  CHECK(harmony.key_at(view, 1000.0).mode == KeyMode::kMinor);
  CHECK(harmony.chord_at(view, 0.0).root_pc == 0);
  CHECK(harmony.chord_at(view, 1000.0).root_pc == 7);
  CHECK(harmony.chord_at(view, 1000.0).quality == ChordQuality::kDominant);

  // The scale follows the key segment the position falls in.
  std::vector<uint8_t> scale = harmony.scale_pitch_classes(view, 0.0);
  std::sort(scale.begin(), scale.end());
  CHECK(scale == std::vector<uint8_t>{0, 2, 4, 5, 7, 9, 11});

  // Past the last segment nothing is stated, and an unstated key is empty rather
  // than a default collection.
  CHECK(harmony.key_at(view, 5000.0).mode == KeyMode::kUnknown);
  CHECK(harmony.chord_at(view, 5000.0).quality == ChordQuality::kUnknown);
  CHECK(harmony.scale_pitch_classes(view, 5000.0).empty());
}

TEST_CASE("clip PPQ transforms keep equal large origins finite", "[assist][modules]") {
  EditClip clip;
  clip.start_ppq = 1.0e308;
  clip.source_offset_ppq = 1.0e308;

  CHECK(clip_content_to_timeline_ppq(clip, 1.0e308) == 1.0e308);
  CHECK(timeline_to_clip_content_ppq(clip, 1.0e308) == 1.0e308);
}

TEST_CASE("an unannotated project answers unknown everywhere", "[assist][modules]") {
  Fixture f = make_fixture();
  const TimelineHarmonyContext harmony;
  const ProjectView view = f.view();
  CHECK(harmony.key_at(view, 0.0).mode == KeyMode::kUnknown);
  CHECK(harmony.chord_at(view, 0.0).quality == ChordQuality::kUnknown);
  CHECK(harmony.chord_at(view, 0.0).root_pc == sonare::arrangement::kUnknownPitchClass);
  CHECK(harmony.scale_pitch_classes(view, 0.0).empty());
}

// ===========================================================================
// IntervalDissonanceAnalyzer
// ===========================================================================

TEST_CASE("a chord tone scores exactly 0 and a semitone off one scores 1", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {}, {chord_symbol(0.0, 1920.0, 0, ChordQuality::kMajor)});
  const TimelineHarmonyContext harmony;
  const IntervalDissonanceAnalyzer analyzer(&harmony);
  const ProjectView view = f.view();

  CHECK(analyzer.score_note(view, CandidateNote{0.0, 240.0, 60, 96}) == Catch::Approx(0.0f));
  CHECK(analyzer.score_note(view, CandidateNote{0.0, 240.0, 64, 96}) == Catch::Approx(0.0f));
  CHECK(analyzer.score_note(view, CandidateNote{0.0, 240.0, 67, 96}) == Catch::Approx(0.0f));
  // An octave of a chord tone is still a chord tone.
  CHECK(analyzer.score_note(view, CandidateNote{0.0, 240.0, 84, 96}) == Catch::Approx(0.0f));
  // A semitone off the root.
  CHECK(analyzer.score_note(view, CandidateNote{0.0, 240.0, 61, 96}) == Catch::Approx(1.0f));
  // A sixth over the triad is mild, and strictly between the two.
  const float sixth = analyzer.score_note(view, CandidateNote{0.0, 240.0, 69, 96});
  CHECK(sixth > 0.0f);
  CHECK(sixth < 1.0f);
}

TEST_CASE("the score is the WORST interval a note makes, not the mildest", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {}, {chord_symbol(0.0, 1920.0, 0, ChordQuality::kMajor)});
  const TimelineHarmonyContext harmony;
  const IntervalDissonanceAnalyzer analyzer(&harmony);
  const ProjectView view = f.view();

  namespace theory = sonare::midi::assist::theory;
  // F is a perfect fourth over C -- among the most consonant intervals there is
  // -- and a semitone under E. The semitone is what it is scored by.
  CHECK(theory::interval_class_roughness(theory::interval_class(65, 0)) < 0.1f);
  CHECK(theory::interval_class_roughness(theory::interval_class(65, 4)) == Catch::Approx(1.0f));
  CHECK(analyzer.score_note(view, CandidateNote{0.0, 240.0, 65, 96}) == Catch::Approx(1.0f));
}

TEST_CASE("an unannotated passage and a missing harmony context both score 0",
          "[assist][modules]") {
  Fixture f = make_fixture();
  const TimelineHarmonyContext harmony;
  const IntervalDissonanceAnalyzer analyzer(&harmony);
  const ProjectView view = f.view();
  // No chord annotated: nothing is stated to clash against.
  for (uint8_t note = 60; note < 72; ++note) {
    CHECK(analyzer.score_note(view, CandidateNote{0.0, 240.0, note, 96}) == Catch::Approx(0.0f));
  }

  // A chord annotated only over the first half leaves the second half unstated.
  annotate(&f, {}, {chord_symbol(0.0, 960.0, 0, ChordQuality::kMajor)});
  const ProjectView annotated = f.view();
  CHECK(analyzer.score_note(annotated, CandidateNote{0.0, 240.0, 61, 96}) == Catch::Approx(1.0f));
  CHECK(analyzer.score_note(annotated, CandidateNote{1000.0, 240.0, 61, 96}) ==
        Catch::Approx(0.0f));

  // No harmony context installed at all.
  const IntervalDissonanceAnalyzer blind(nullptr);
  CHECK(blind.score_note(annotated, CandidateNote{0.0, 240.0, 61, 96}) == Catch::Approx(0.0f));
}

TEST_CASE("score_vertical is 0 under two notes and the worst pair otherwise", "[assist][modules]") {
  Fixture f = make_fixture();
  const TimelineHarmonyContext harmony;
  const IntervalDissonanceAnalyzer analyzer(&harmony);
  const ProjectView view = f.view();

  CHECK(analyzer.score_vertical(view, 0.0, {}) == Catch::Approx(0.0f));
  CHECK(analyzer.score_vertical(view, 0.0, {CandidateNote{0.0, 0.0, 61, 96}}) ==
        Catch::Approx(0.0f));

  const std::vector<CandidateNote> triad{CandidateNote{0.0, 0.0, 60, 96},
                                         CandidateNote{0.0, 0.0, 64, 96},
                                         CandidateNote{0.0, 0.0, 67, 96}};
  const float triad_score = analyzer.score_vertical(view, 0.0, triad);
  CHECK(triad_score > 0.0f);
  CHECK(triad_score < 0.5f);

  // Adding one semitone clash dominates the whole stack.
  std::vector<CandidateNote> with_clash = triad;
  with_clash.push_back(CandidateNote{0.0, 0.0, 61, 96});
  CHECK(analyzer.score_vertical(view, 0.0, with_clash) == Catch::Approx(1.0f));
  CHECK(analyzer.score_vertical(view, 0.0, with_clash) > triad_score);

  // The pair is what is scored, so the order of the stack cannot change it.
  std::reverse(with_clash.begin(), with_clash.end());
  CHECK(analyzer.score_vertical(view, 0.0, with_clash) == Catch::Approx(1.0f));
}

// ===========================================================================
// RangeScaleJudge
// ===========================================================================

TEST_CASE("an out-of-range note is octave-folded and kept, with the fold named",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)},
           {chord_symbol(0.0, 1920.0, 0, ChordQuality::kMajor)});
  const TimelineHarmonyContext harmony;
  const IntervalDissonanceAnalyzer dissonance(&harmony);
  const RangeScaleJudge judge(&harmony, &dissonance);
  const ProjectView view = f.view();

  VoiceModel voice;
  voice.low_note = 60;
  voice.high_note = 72;

  const PlacementVerdict low = judge.judge(view, voice, CandidateNote{0.0, 240.0, 48, 96});
  CHECK(low.accepted);
  CHECK(low.adjusted.note == 60);
  CHECK(reason_mentions(low.reason, "folded"));
  CHECK(reason_mentions(low.reason, "48"));
  CHECK(reason_mentions(low.reason, "60"));

  const PlacementVerdict high = judge.judge(view, voice, CandidateNote{0.0, 240.0, 88, 96});
  CHECK(high.accepted);
  CHECK(high.adjusted.note == 64);
  CHECK(reason_mentions(high.reason, "folded"));

  // The fold keeps the pitch class: it corrects a register, it does not pick a
  // different note.
  CHECK(low.adjusted.note % 12 == 48 % 12);
  CHECK(high.adjusted.note % 12 == 88 % 12);
  // Nothing else about the candidate moves.
  CHECK(low.adjusted.ppq == 0.0);
  CHECK(low.adjusted.length_ppq == 240.0);
  CHECK(low.adjusted.velocity == 96);
}

TEST_CASE("a voice range no octave fits is refused rather than forced", "[assist][modules]") {
  Fixture f = make_fixture();
  const TimelineHarmonyContext harmony;
  const IntervalDissonanceAnalyzer dissonance(&harmony);
  const RangeScaleJudge judge(&harmony, &dissonance);
  const ProjectView view = f.view();

  VoiceModel narrow;
  narrow.low_note = 60;
  narrow.high_note = 64;  // narrower than an octave

  const PlacementVerdict refused = judge.judge(view, narrow, CandidateNote{0.0, 240.0, 66, 96});
  CHECK_FALSE(refused.accepted);
  CHECK(reason_mentions(refused.reason, "no octave"));
  CHECK(reason_mentions(refused.reason, "66"));
  CHECK(reason_mentions(refused.reason, "60..64"));

  // A note whose octave DOES fit the same narrow range is accepted, so the
  // refusal above is about that note and not about the range being narrow.
  const PlacementVerdict accepted = judge.judge(view, narrow, CandidateNote{0.0, 240.0, 74, 96});
  CHECK(accepted.accepted);
  CHECK(accepted.adjusted.note == 62);
}

TEST_CASE("an out-of-key note is snapped and kept, and snap_to_scale off leaves it",
          "[assist][modules]") {
  Fixture f = make_fixture();
  // A key but no chord: the scale rule acts and the dissonance rule has nothing
  // to say, which isolates the snap.
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)}, {});
  const TimelineHarmonyContext harmony;
  const IntervalDissonanceAnalyzer dissonance(&harmony);
  const ProjectView view = f.view();

  VoiceModel voice;
  voice.low_note = 36;
  voice.high_note = 84;

  const RangeScaleJudge snapping(&harmony, &dissonance);
  const PlacementVerdict snapped = snapping.judge(view, voice, CandidateNote{0.0, 240.0, 61, 96});
  CHECK(snapped.accepted);
  CHECK(snapped.adjusted.note == 60);
  CHECK(reason_mentions(snapped.reason, "snapped"));
  CHECK(reason_mentions(snapped.reason, "61"));
  CHECK(reason_mentions(snapped.reason, "60"));

  RangeScaleJudgeConfig no_snap;
  no_snap.snap_to_scale = false;
  const RangeScaleJudge leaving(&harmony, &dissonance, no_snap);
  const PlacementVerdict left = leaving.judge(view, voice, CandidateNote{0.0, 240.0, 61, 96});
  CHECK(left.accepted);
  CHECK(left.adjusted.note == 61);
  CHECK_FALSE(reason_mentions(left.reason, "snapped"));
}

TEST_CASE("a note that still clashes is REFUSED and the reason names the score",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)},
           {chord_symbol(0.0, 1920.0, 0, ChordQuality::kMajor)});
  const TimelineHarmonyContext harmony;
  const IntervalDissonanceAnalyzer dissonance(&harmony);
  const RangeScaleJudge judge(&harmony, &dissonance);
  const ProjectView view = f.view();

  VoiceModel voice;
  voice.low_note = 36;
  voice.high_note = 84;

  // B is IN C major and inside the range, so the register and key rules both
  // pass it. It is a semitone under the root, and the third rule refuses it.
  const PlacementVerdict refused = judge.judge(view, voice, CandidateNote{0.0, 240.0, 71, 96});
  CHECK_FALSE(refused.accepted);
  CHECK(reason_mentions(refused.reason, "1.00"));
  CHECK(reason_mentions(refused.reason, "0.70"));
  CHECK(reason_mentions(refused.reason, "71"));

  // Raising the limit past the score admits the same note: the refusal is the
  // configured limit acting, not the note being unrepresentable.
  RangeScaleJudgeConfig permissive;
  permissive.max_dissonance = 1.0f;
  const RangeScaleJudge lenient(&harmony, &dissonance, permissive);
  const PlacementVerdict allowed = lenient.judge(view, voice, CandidateNote{0.0, 240.0, 71, 96});
  CHECK(allowed.accepted);
  CHECK(allowed.adjusted.note == 71);

  // With no dissonance analyzer installed nothing is refused for clashing.
  const RangeScaleJudge blind(&harmony, nullptr);
  CHECK(blind.judge(view, voice, CandidateNote{0.0, 240.0, 71, 96}).accepted);
}

TEST_CASE("with no key annotated the scale rule is SKIPPED, not defaulted to C major",
          "[assist][modules]") {
  Fixture f = make_fixture();
  const TimelineHarmonyContext harmony;
  const RangeScaleJudge judge(&harmony, nullptr);
  const ProjectView unannotated = f.view();

  VoiceModel voice;
  voice.low_note = 36;
  voice.high_note = 84;

  // C sharp is not in C major. On an unannotated project it is left exactly
  // where it is: a silent fallback to C major would have moved it to 60.
  const PlacementVerdict open = judge.judge(unannotated, voice, CandidateNote{0.0, 240.0, 61, 96});
  CHECK(open.accepted);
  CHECK(open.adjusted.note == 61);
  CHECK_FALSE(reason_mentions(open.reason, "snapped"));

  // The two-sided control: annotate C major and the SAME judge on the SAME
  // candidate does move it, so the case above is the rule being skipped rather
  // than the rule being unable to act.
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)}, {});
  const PlacementVerdict keyed = judge.judge(f.view(), voice, CandidateNote{0.0, 240.0, 61, 96});
  CHECK(keyed.accepted);
  CHECK(keyed.adjusted.note == 60);
  CHECK(reason_mentions(keyed.reason, "snapped"));
}

TEST_CASE("the judge refuses a malformed candidate or an empty voice", "[assist][modules]") {
  Fixture f = make_fixture();
  const TimelineHarmonyContext harmony;
  const IntervalDissonanceAnalyzer dissonance(&harmony);
  const RangeScaleJudge judge(&harmony, &dissonance);
  const ProjectView view = f.view();

  VoiceModel voice;
  voice.low_note = 36;
  voice.high_note = 84;

  CandidateNote silent{0.0, 240.0, 60, 0};
  const PlacementVerdict no_velocity = judge.judge(view, voice, silent);
  CHECK_FALSE(no_velocity.accepted);
  CHECK(reason_mentions(no_velocity.reason, "velocity 0"));

  CandidateNote over{0.0, 240.0, 60, 128};
  const PlacementVerdict too_loud = judge.judge(view, voice, over);
  CHECK_FALSE(too_loud.accepted);
  CHECK(reason_mentions(too_loud.reason, "velocity 128"));

  CandidateNote off_keyboard{0.0, 240.0, 200, 96};
  const PlacementVerdict out_of_midi = judge.judge(view, voice, off_keyboard);
  CHECK_FALSE(out_of_midi.accepted);
  CHECK(reason_mentions(out_of_midi.reason, "MIDI range"));

  VoiceModel empty_voice;
  empty_voice.low_note = 72;
  empty_voice.high_note = 60;
  const PlacementVerdict no_voice =
      judge.judge(view, empty_voice, CandidateNote{0.0, 240.0, 65, 96});
  CHECK_FALSE(no_voice.accepted);
  CHECK(reason_mentions(no_voice.reason, "72..60"));
  CHECK(reason_mentions(no_voice.reason, "empty"));

  // The boundary values themselves are accepted, so the refusals above are the
  // boundary and not a blanket rejection.
  CHECK(judge.judge(view, voice, CandidateNote{0.0, 240.0, 60, 1}).accepted);
  CHECK(judge.judge(view, voice, CandidateNote{0.0, 240.0, 60, 127}).accepted);
}

TEST_CASE("every accepted verdict carries a reason, including an untouched note",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)},
           {chord_symbol(0.0, 1920.0, 0, ChordQuality::kMajor)});
  const TimelineHarmonyContext harmony;
  const IntervalDissonanceAnalyzer dissonance(&harmony);
  const ProjectView view = f.view();

  VoiceModel voice;
  voice.low_note = 36;
  voice.high_note = 84;

  // A note that passes every rule untouched still says why it passed.
  const RangeScaleJudge judge(&harmony, &dissonance);
  const PlacementVerdict untouched = judge.judge(view, voice, CandidateNote{0.0, 240.0, 64, 96});
  CHECK(untouched.accepted);
  CHECK(untouched.adjusted.note == 64);
  CHECK_FALSE(untouched.reason.empty());
  CHECK(reason_mentions(untouched.reason, "0.00"));

  // With no analyzer there is no score to report, and the verdict still speaks.
  const RangeScaleJudge quiet(&harmony, nullptr);
  const PlacementVerdict plain = quiet.judge(view, voice, CandidateNote{0.0, 240.0, 64, 96});
  CHECK(plain.accepted);
  CHECK(plain.reason == "accepted unchanged");

  // A reason is present for every note across the voice, adjusted or not.
  for (uint8_t note = 36; note <= 84; ++note) {
    const PlacementVerdict verdict = judge.judge(view, voice, CandidateNote{0.0, 240.0, note, 96});
    CHECK_FALSE(verdict.reason.empty());
  }
}

// ===========================================================================
// read_generator_params
// ===========================================================================

TEST_CASE("read_generator_params keeps the defaults for fields nobody named", "[assist][modules]") {
  GeneratorParams params;
  std::string error;
  REQUIRE(read_generator_params("", &params, &error));
  CHECK(error.empty());
  CHECK(params.target_clip_id == 0u);
  CHECK(params.source_clip_id == 0u);
  CHECK(params.low_note == 36);
  CHECK(params.high_note == 84);
  CHECK(params.base_velocity == 90);
  CHECK(params.velocity_scale == Catch::Approx(0.85f));

  GeneratorParams partial;
  REQUIRE(read_generator_params("{\"target_clip_id\": 7, \"low_note\": 48}", &partial, &error));
  CHECK(partial.target_clip_id == 7u);
  CHECK(partial.low_note == 48);
  // Untouched fields keep their defaults rather than being reset.
  CHECK(partial.high_note == 84);
  CHECK(partial.velocity_scale == Catch::Approx(0.85f));
}

TEST_CASE("read_generator_params refuses a wrong type instead of substituting a default",
          "[assist][modules]") {
  GeneratorParams params;
  std::string error;

  CHECK_FALSE(read_generator_params("{", &params, &error));
  CHECK(reason_mentions(error, "params_json is not valid JSON"));

  CHECK_FALSE(read_generator_params("[1, 2]", &params, &error));
  CHECK(reason_mentions(error, "must be a JSON object"));

  CHECK_FALSE(read_generator_params("{\"low_note\": \"48\"}", &params, &error));
  CHECK(reason_mentions(error, "low_note"));
  // The default is intact: a refused read leaves nothing half-applied.
  CHECK(params.low_note == 36);

  CHECK_FALSE(read_generator_params("{\"low_note\": 48.5}", &params, &error));
  CHECK(reason_mentions(error, "integer"));
  CHECK_FALSE(read_generator_params("{\"high_note\": 200}", &params, &error));
  CHECK(reason_mentions(error, "high_note"));
  CHECK_FALSE(read_generator_params("{\"velocity_scale\": 0}", &params, &error));
  CHECK(reason_mentions(error, "velocity_scale"));
  CHECK_FALSE(read_generator_params("{\"velocity_scale\": 2.5}", &params, &error));
  CHECK(reason_mentions(error, "(0, 2]"));
  CHECK_FALSE(read_generator_params("{\"low_note\": 80, \"high_note\": 40}", &params, &error));
  CHECK(reason_mentions(error, "low_note must not be above high_note"));

  // A refused read leaves the fields it got to already written, so each case
  // below starts from a struct nothing has touched.
  GeneratorParams fresh;
  CHECK_FALSE(read_generator_params("{\"base_velocity\": 0}", &fresh, &error));
  CHECK(reason_mentions(error, "base_velocity"));
  // The range check the partly-written struct above would have tripped first.
  CHECK(params.low_note == 80);
  CHECK(params.high_note == 40);

  // The boundary values themselves read cleanly.
  GeneratorParams edge;
  CHECK(read_generator_params("{\"velocity_scale\": 2}", &edge, &error));
  CHECK(edge.velocity_scale == Catch::Approx(2.0f));
  CHECK(read_generator_params("{\"low_note\": 0, \"high_note\": 127}", &edge, &error));
  CHECK(edge.low_note == 0);
  CHECK(edge.high_note == 127);
}

TEST_CASE("read_generator_params refuses params_json that exceeds its resource budget",
          "[assist][modules][resource]") {
  GeneratorParams params;
  std::string error;

  // A tiny override exercises the boundary without building a production-sized
  // document. Byte axis: the text itself is longer than the budget allows.
  sonare::resource::ProjectImportResourceLimits tiny_bytes =
      sonare::resource::kDefaultProjectImportResourceLimits;
  tiny_bytes.max_json_bytes = 8;
  const std::string small = "{\"low_note\": 40}";
  REQUIRE(small.size() > tiny_bytes.max_json_bytes);
  CHECK_FALSE(read_generator_params(small, &params, &error, tiny_bytes));
  CHECK(reason_mentions(error, "byte budget"));

  // Node axis: a flat array of six numbers is seven nodes (the array plus each
  // element), one past a budget of six -- refused before the type check that
  // would otherwise reject an array params_json for a different reason.
  sonare::resource::ProjectImportResourceLimits tiny_nodes =
      sonare::resource::kDefaultProjectImportResourceLimits;
  tiny_nodes.max_json_nodes = 6;
  CHECK_FALSE(read_generator_params("[1,2,3,4,5,6]", &params, &error, tiny_nodes));
  CHECK(reason_mentions(error, "node limit exceeded"));

  // An exactly-fitting document under the same tight node budget still reads:
  // the refusal above is the budget, not an unrelated regression.
  GeneratorParams fits;
  CHECK(read_generator_params("{\"low_note\": 40}", &fits, &error, tiny_nodes));
  CHECK(fits.low_note == 40);

  // The production default admits an ordinary request untouched.
  GeneratorParams normal;
  CHECK(read_generator_params("{\"target_clip_id\": 3, \"low_note\": 48}", &normal, &error));
  CHECK(normal.target_clip_id == 3u);
  CHECK(normal.low_note == 48);
}

// ===========================================================================
// DiatonicHarmonizer
// ===========================================================================

TEST_CASE("the harmonizer derives a third below that follows the key", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)},
           {chord_symbol(0.0, 1920.0, 0, ChordQuality::kMajor)});
  set_events(&f, c_major_line());

  const TimelineHarmonyContext harmony;
  const IntervalDissonanceAnalyzer dissonance(&harmony);
  const RangeScaleJudge judge(&harmony, &dissonance);
  const AssistQueryContext queries{&harmony, &dissonance, &judge};

  DiatonicHarmonizer harmonizer;
  const AssistResult result =
      harmonizer.derive(f.view(), request_with(target_params(f.clip_id)), {}, queries);

  CHECK(result.diagnostics.status == AssistStatus::kOk);
  const PatchMidiClip* patch = sole_patch(result);
  CHECK(patch->patch().clip_id == f.clip_id);
  CHECK(patch->patch().remove.empty());

  const std::vector<EmittedNote> notes = emitted_notes(*patch);
  REQUIRE(notes.size() == 4u);
  // A third below C4 / E4 / G4 / C5 in C major.
  CHECK(note_numbers(notes) == std::vector<int>{57, 60, 64, 69});
  // The source's rhythm is kept exactly.
  CHECK(notes[0].on_ppq == 0.0);
  CHECK(notes[1].on_ppq == 480.0);
  CHECK(notes[2].on_ppq == 960.0);
  CHECK(notes[3].on_ppq == 1440.0);
  for (const EmittedNote& note : notes) {
    CHECK(note.off_ppq == note.on_ppq + 240.0);
    CHECK(note.velocity > 0);
  }

  // The intervals the derived voice sits at are NOT all the same: this is the
  // assertion a fixed-semitone transposition fails.
  CHECK(60 - notes[0].note == 3);
  CHECK(64 - notes[1].note == 4);
  CHECK(67 - notes[2].note == 3);
  CHECK(72 - notes[3].note == 3);
}

TEST_CASE("the harmonizer scales velocity off the source note's own velocity",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)}, {});

  MidiClipEventList events;
  events.push_back(note_on_event(0.0, 60, 100));
  events.push_back(note_off_event(240.0, 60));
  events.push_back(note_on_event(480.0, 64, 60));
  events.push_back(note_off_event(720.0, 64));
  set_events(&f, events);

  const TimelineHarmonyContext harmony;
  const RangeScaleJudge judge(&harmony, nullptr);
  const AssistQueryContext queries{&harmony, nullptr, &judge};
  DiatonicHarmonizer harmonizer;

  const AssistResult defaulted =
      harmonizer.derive(f.view(), request_with(target_params(f.clip_id)), {}, queries);
  const std::vector<EmittedNote> at_default = emitted_notes(*sole_patch(defaulted));
  REQUIRE(at_default.size() == 2u);
  // 0.85 of each SOURCE velocity, not a flat value and not the base velocity.
  CHECK(at_default[0].velocity == 85);
  CHECK(at_default[1].velocity == 51);

  const std::string params =
      "{\"target_clip_id\": " + std::to_string(f.clip_id) + ", \"velocity_scale\": 0.5}";
  const AssistResult halved = harmonizer.derive(f.view(), request_with(params), {}, queries);
  const std::vector<EmittedNote> at_half = emitted_notes(*sole_patch(halved));
  REQUIRE(at_half.size() == 2u);
  CHECK(at_half[0].velocity == 50);
  CHECK(at_half[1].velocity == 30);
}

TEST_CASE("the harmonizer is deterministic and leaves the project untouched", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)},
           {chord_symbol(0.0, 1920.0, 0, ChordQuality::kMajor)});
  set_events(&f, c_major_line());

  const TimelineHarmonyContext harmony;
  const IntervalDissonanceAnalyzer dissonance(&harmony);
  const RangeScaleJudge judge(&harmony, &dissonance);
  const AssistQueryContext queries{&harmony, &dissonance, &judge};

  const std::string before = f.serialized();
  const auto midi_before = f.history.midi_content();

  DiatonicHarmonizer harmonizer;
  AssistRequest request = request_with(target_params(f.clip_id));
  request.seed = 12345;
  const AssistResult first = harmonizer.derive(f.view(), request, {}, queries);
  const AssistResult second = harmonizer.derive(f.view(), request, {}, queries);

  CHECK(sole_patch(first)->patch().add == sole_patch(second)->patch().add);
  CHECK(first.candidate_payload == second.candidate_payload);
  CHECK(first.diagnostics.iterations_consumed == second.diagnostics.iterations_consumed);

  // A seed the modules do not consume cannot change the answer either.
  request.seed = 999;
  const AssistResult reseeded = harmonizer.derive(f.view(), request, {}, queries);
  CHECK(sole_patch(reseeded)->patch().add == sole_patch(first)->patch().add);

  // The module proposes; it does not apply.
  CHECK(f.serialized() == before);
  CHECK(f.history.midi_content() == midi_before);
}

TEST_CASE("a refused request and an empty outcome are different statuses", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)}, {});
  set_events(&f, c_major_line());
  const TimelineHarmonyContext harmony;
  const AssistQueryContext queries{&harmony, nullptr, nullptr};
  DiatonicHarmonizer harmonizer;

  // A request the module will not answer: the caller named no clip to write to.
  AssistResult rejected;
  REQUIRE_NOTHROW(rejected = harmonizer.derive(f.view(), AssistRequest{}, {}, queries));
  CHECK(rejected.commands.empty());
  CHECK(rejected.diagnostics.status == AssistStatus::kRejected);
  CHECK(reason_mentions(rejected.diagnostics.reason, "target_clip_id"));

  // A well-formed request with nothing to act on: the clip named exists as far
  // as the request is concerned, it simply holds no notes to derive from. That
  // is a musical outcome, not a caller mistake.
  AssistResult empty;
  REQUIRE_NOTHROW(
      empty = harmonizer.derive(f.view(), request_with("{\"target_clip_id\": 4242}"), {}, queries));
  CHECK(empty.commands.empty());
  CHECK(empty.diagnostics.status == AssistStatus::kEmpty);
  CHECK(reason_mentions(empty.diagnostics.reason, "4242"));

  // Both produce no commands and both carry a reason, so the STATUS is the only
  // thing separating a typo from a quiet nothing. This is the assertion that
  // catches the two collapsing back into one.
  CHECK(rejected.diagnostics.status != empty.diagnostics.status);
  CHECK_FALSE(rejected.diagnostics.reason.empty());
  CHECK_FALSE(empty.diagnostics.reason.empty());
}

TEST_CASE("missing assist clips are empty while a zero target is rejected", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)},
           {chord_symbol(0.0, 960.0, 0, ChordQuality::kMajor)});
  set_events(&f, c_major_line());

  const TimelineHarmonyContext harmony;
  const AssistQueryContext queries{&harmony, nullptr, nullptr};
  DiatonicHarmonizer harmonizer;
  ChordToneGenerator generator;

  const AssistResult missing_source =
      harmonizer.derive(f.view(), request_with(source_target_params(f.clip_id, 4242)), {}, queries);
  CHECK(missing_source.commands.empty());
  CHECK(missing_source.diagnostics.status == AssistStatus::kEmpty);
  CHECK(reason_mentions(missing_source.diagnostics.reason, "4242"));

  const AssistResult missing_target =
      harmonizer.derive(f.view(), request_with(source_target_params(4242, f.clip_id)), {}, queries);
  CHECK(missing_target.commands.empty());
  CHECK(missing_target.diagnostics.status == AssistStatus::kEmpty);
  CHECK(reason_mentions(missing_target.diagnostics.reason, "4242"));

  const AssistResult zero_harmonizer =
      harmonizer.derive(f.view(), request_with(source_target_params(0, f.clip_id)), {}, queries);
  CHECK(zero_harmonizer.commands.empty());
  CHECK(zero_harmonizer.diagnostics.status == AssistStatus::kRejected);
  CHECK(reason_mentions(zero_harmonizer.diagnostics.reason, "target_clip_id"));

  const AssistResult missing_chord_target =
      generator.generate(f.view(), request_with(target_params(4242)));
  CHECK(missing_chord_target.commands.empty());
  CHECK(missing_chord_target.diagnostics.status == AssistStatus::kEmpty);
  CHECK(reason_mentions(missing_chord_target.diagnostics.reason, "4242"));

  const AssistResult zero_chord_target =
      generator.generate(f.view(), request_with(target_params(0)));
  CHECK(zero_chord_target.commands.empty());
  CHECK(zero_chord_target.diagnostics.status == AssistStatus::kRejected);
  CHECK(reason_mentions(zero_chord_target.diagnostics.reason, "target_clip_id"));
}

TEST_CASE("a malformed params document is REFUSED, naming the field", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)}, {});
  set_events(&f, c_major_line());
  const TimelineHarmonyContext harmony;
  const RangeScaleJudge judge(&harmony, nullptr);
  const AssistQueryContext queries{&harmony, nullptr, &judge};
  DiatonicHarmonizer harmonizer;

  struct Case {
    const char* params;
    const char* fragment;
  };
  const Case cases[] = {
      {"{", "params_json is not valid JSON"},
      {"[]", "must be a JSON object"},
      {"{\"target_clip_id\": \"1\"}", "target_clip_id"},
      {"{\"target_clip_id\": 1, \"low_note\": true}", "low_note"},
      {"{\"target_clip_id\": 1, \"velocity_scale\": 0}", "velocity_scale"},
      {"{\"target_clip_id\": 1, \"velocity_scale\": 3}", "velocity_scale"},
  };
  for (const Case& test_case : cases) {
    AssistResult result;
    REQUIRE_NOTHROW(result =
                        harmonizer.derive(f.view(), request_with(test_case.params), {}, queries));
    INFO("params: " << test_case.params);
    CHECK(result.commands.empty());
    CHECK(result.diagnostics.status == AssistStatus::kRejected);
    CHECK(reason_mentions(result.diagnostics.reason, test_case.fragment));
  }

  // The same request with a valid params document does produce notes, so the
  // refusals above are the fields being read and not the module being inert.
  const AssistResult good =
      harmonizer.derive(f.view(), request_with(target_params(f.clip_id)), {}, queries);
  CHECK(good.diagnostics.status == AssistStatus::kOk);
  CHECK_FALSE(good.commands.empty());

  // And the other side of the split: a params document with nothing wrong in it
  // over a project that states no key. Every note is dropped for a musical
  // reason, so the run is EMPTY rather than refused.
  Fixture keyless = make_fixture();
  set_events(&keyless, c_major_line());
  const AssistResult nothing_to_do =
      harmonizer.derive(keyless.view(), request_with(target_params(keyless.clip_id)), {}, queries);
  CHECK(nothing_to_do.commands.empty());
  CHECK(nothing_to_do.diagnostics.status == AssistStatus::kEmpty);
  CHECK(nothing_to_do.diagnostics.status != AssistStatus::kRejected);
  CHECK(reason_mentions(nothing_to_do.diagnostics.reason, "every derived note was refused"));
}

TEST_CASE("the harmonizer truncates on the iteration budget and says so", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)}, {});
  set_events(&f, c_major_line());
  const TimelineHarmonyContext harmony;
  const RangeScaleJudge judge(&harmony, nullptr);
  const AssistQueryContext queries{&harmony, nullptr, &judge};
  DiatonicHarmonizer harmonizer;

  AssistRequest request = request_with(target_params(f.clip_id));
  request.budget.max_iterations = 2;
  const AssistResult truncated = harmonizer.derive(f.view(), request, {}, queries);

  CHECK(truncated.diagnostics.status == AssistStatus::kBudgetTruncated);
  CHECK(truncated.diagnostics.iterations_consumed == 2u);
  CHECK_FALSE(truncated.diagnostics.reason.empty());
  const std::vector<EmittedNote> partial = emitted_notes(*sole_patch(truncated));
  CHECK(note_numbers(partial) == std::vector<int>{57, 60});

  // Without the cap the same request gives all four, so the truncation is the
  // budget acting rather than the source being short.
  const AssistResult whole =
      harmonizer.derive(f.view(), request_with(target_params(f.clip_id)), {}, queries);
  CHECK(whole.diagnostics.status == AssistStatus::kOk);
  CHECK(emitted_notes(*sole_patch(whole)).size() == 4u);
}

TEST_CASE("the scope's PPQ bounds exclude the notes outside them", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)}, {});
  set_events(&f, c_major_line());
  const TimelineHarmonyContext harmony;
  const RangeScaleJudge judge(&harmony, nullptr);
  const AssistQueryContext queries{&harmony, nullptr, &judge};
  DiatonicHarmonizer harmonizer;

  AssistRequest request = request_with(target_params(f.clip_id));
  request.scope.start_ppq = 480.0;
  request.scope.end_ppq = 1440.0;
  const AssistResult scoped = harmonizer.derive(f.view(), request, {}, queries);

  const std::vector<EmittedNote> notes = emitted_notes(*sole_patch(scoped));
  // The bound is half-open: the note AT start_ppq is in, the one at end_ppq out.
  CHECK(note_numbers(notes) == std::vector<int>{60, 64});
  CHECK(notes.front().on_ppq == 480.0);
  CHECK(notes.back().on_ppq == 960.0);

  // A scope covering nothing produces nothing, with a reason rather than a patch.
  AssistRequest empty_scope = request_with(target_params(f.clip_id));
  empty_scope.scope.start_ppq = 5000.0;
  const AssistResult none = harmonizer.derive(f.view(), empty_scope, {}, queries);
  CHECK(none.commands.empty());
  CHECK(none.diagnostics.status == AssistStatus::kEmpty);
  CHECK_FALSE(none.diagnostics.reason.empty());
}

TEST_CASE("the harmonizer keeps group/channel note stacks and protocol zero velocities distinct",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 2000.0, 0, KeyMode::kMajor)}, {});

  MidiClipEventList events;
  // Same key: the second note-on is closed first, then the first one.
  events.push_back(note_on_event(0.0, 60, 100, 1, 2));
  events.push_back(note_on_event(120.0, 60, 80, 1, 2));
  events.push_back(note_off_event(240.0, 60, 1, 2));
  events.push_back(note_off_event(360.0, 60, 1, 2));
  // Equal note numbers remain independent across group and channel.
  events.push_back(note_on_event(0.0, 60, 90, 2, 2));
  events.push_back(note_off_event(240.0, 60, 2, 2));
  events.push_back(note_on_event(0.0, 60, 90, 1, 3));
  events.push_back(note_off_event(240.0, 60, 1, 3));
  // A wrong-group off cannot close the open note; both that note and a
  // zero-length pair are intentionally absent from the derived patch.
  events.push_back(note_on_event(0.0, 60, 90, 3, 4));
  events.push_back(note_off_event(120.0, 60, 4, 4));
  events.push_back(note_on_event(480.0, 60, 90, 3, 5));
  events.push_back(note_off_event(480.0, 60, 3, 5));
  set_events(&f, std::move(events));

  const TimelineHarmonyContext harmony;
  const AssistQueryContext queries{&harmony, nullptr, nullptr};
  const AssistResult result =
      DiatonicHarmonizer().derive(f.view(), request_with(target_params(f.clip_id)), {}, queries);

  REQUIRE(result.diagnostics.status == AssistStatus::kOk);
  const std::vector<EmittedNote> notes = emitted_notes(*sole_patch(result));
  REQUIRE(notes.size() == 4u);
  std::map<std::tuple<uint8_t, uint8_t, uint8_t>, std::vector<double>> durations;
  for (const EmittedNote& note : notes) {
    durations[{note.group, note.channel, note.note}].push_back(note.off_ppq - note.on_ppq);
  }
  REQUIRE(durations.size() == 3u);
  CHECK(durations.find({3, 4, 57}) == durations.end());
  std::sort(durations[{1, 2, 57}].begin(), durations[{1, 2, 57}].end());
  CHECK(durations[{1, 2, 57}] == std::vector<double>{120.0, 360.0});
  CHECK(durations[{2, 2, 57}] == std::vector<double>{240.0});
  CHECK(durations[{1, 3, 57}] == std::vector<double>{240.0});
}

TEST_CASE("MIDI 1 zero note-on closes while MIDI 2 zero note-on remains open",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1200.0, 0, KeyMode::kMajor)}, {});
  MidiClipEventList events;
  events.push_back(note_on_event(0.0, 60, 100, 1, 2));
  events.push_back(note_on_event(240.0, 60, 0, 1, 2));
  events.push_back(midi2_note_on_event(480.0, 64, 0, 3, 4));
  events.push_back(midi2_note_off_event(720.0, 64, 3, 4));
  set_events(&f, std::move(events));

  const TimelineHarmonyContext harmony;
  const AssistQueryContext queries{&harmony, nullptr, nullptr};
  const AssistResult result =
      DiatonicHarmonizer().derive(f.view(), request_with(target_params(f.clip_id)), {}, queries);

  REQUIRE(result.diagnostics.status == AssistStatus::kOk);
  const std::vector<EmittedNote> notes = emitted_notes(*sole_patch(result));
  REQUIRE(notes.size() == 2u);
  CHECK(note_numbers(notes) == std::vector<int>{57, 60});
  CHECK(notes[0].off_ppq - notes[0].on_ppq == 240.0);
  CHECK(notes[1].off_ppq - notes[1].on_ppq == 240.0);

  // The pairing policy must not convert the source packet or its protocol
  // velocity before the harmonizer's existing 7-bit scaling. The output is
  // deliberately MIDI 1, but group/channel, velocity, and both words are
  // pinned to the derived values for each non-overlapping source note.
  const sonare::midi::Ump c_on = sonare::midi::make_midi1_note_on(1, 2, 57, 85);
  const sonare::midi::Ump c_off = sonare::midi::make_midi1_note_off(1, 2, 57, 0);
  const sonare::midi::Ump e_on = sonare::midi::make_midi1_note_on(3, 4, 60, 77);
  const sonare::midi::Ump e_off = sonare::midi::make_midi1_note_off(3, 4, 60, 0);
  CHECK(sole_patch(result)->patch().add ==
        MidiClipEventList{MidiClipEvent{0.0, c_on.words[0], c_on.words[1], 0},
                          MidiClipEvent{240.0, c_off.words[0], c_off.words[1], 0},
                          MidiClipEvent{480.0, e_on.words[0], e_on.words[1], 0},
                          MidiClipEvent{720.0, e_off.words[0], e_off.words[1], 0}});
}

TEST_CASE("scope filtering happens before the harmonizer iteration budget", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 2400.0, 0, KeyMode::kMajor)}, {});
  MidiClipEventList events;
  for (int i = 0; i < 5; ++i) {
    events.push_back(note_on_event(i * 100.0, static_cast<uint8_t>(60 + i), 90));
    events.push_back(note_off_event(i * 100.0 + 40.0, static_cast<uint8_t>(60 + i)));
  }
  events.push_back(note_on_event(1000.0, 67, 90));
  events.push_back(note_off_event(1040.0, 67));
  events.push_back(note_on_event(1200.0, 69, 90));
  events.push_back(note_off_event(1240.0, 69));
  set_events(&f, std::move(events));

  AssistRequest request = request_with(target_params(f.clip_id));
  request.scope.start_ppq = 1000.0;
  request.scope.end_ppq = 1300.0;
  request.budget.max_iterations = 1;
  const TimelineHarmonyContext harmony;
  const AssistQueryContext queries{&harmony, nullptr, nullptr};
  const AssistResult result = DiatonicHarmonizer().derive(f.view(), request, {}, queries);

  CHECK(result.diagnostics.status == AssistStatus::kBudgetTruncated);
  CHECK(result.diagnostics.iterations_consumed == 1u);
  const std::vector<EmittedNote> partial = emitted_notes(*sole_patch(result));
  REQUIRE(partial.size() == 1u);
  CHECK(partial[0].on_ppq == 1000.0);
}

TEST_CASE("scope crossing preserves the LIFO closing note outside the scope", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 500.0, 0, KeyMode::kMajor)}, {});
  MidiClipEventList events;
  events.push_back(note_on_event(0.0, 60, 90));
  events.push_back(note_on_event(100.0, 60, 90));
  events.push_back(note_on_event(200.0, 60, 90));
  events.push_back(note_off_event(220.0, 60));
  events.push_back(note_off_event(240.0, 60));
  events.push_back(note_off_event(300.0, 60));
  set_events(&f, std::move(events));

  AssistRequest request = request_with(target_params(f.clip_id));
  request.scope.start_ppq = 100.0;
  request.scope.end_ppq = 200.0;
  const TimelineHarmonyContext harmony;
  const AssistQueryContext queries{&harmony, nullptr, nullptr};
  const AssistResult result = DiatonicHarmonizer().derive(f.view(), request, {}, queries);

  REQUIRE(result.diagnostics.status == AssistStatus::kOk);
  const std::vector<EmittedNote> notes = emitted_notes(*sole_patch(result));
  REQUIRE(notes.size() == 1u);
  CHECK(notes[0].on_ppq == 100.0);
  CHECK(notes[0].off_ppq == 240.0);
}

TEST_CASE("harmonizer orders candidates by onset then note group channel and end",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1000.0, 0, KeyMode::kMajor)}, {});

  // Completion order is E4 first and C4 second; onset decides.
  MidiClipEventList unique_events;
  unique_events.push_back(note_on_event(0.0, 60, 100));
  unique_events.push_back(note_on_event(100.0, 64, 100));
  unique_events.push_back(note_off_event(150.0, 64));
  unique_events.push_back(note_off_event(300.0, 60));
  set_events(&f, std::move(unique_events));

  const TimelineHarmonyContext harmony;
  const AssistQueryContext queries{&harmony, nullptr, nullptr};
  const AssistResult unique =
      DiatonicHarmonizer().derive(f.view(), request_with(target_params(f.clip_id)), {}, queries);
  REQUIRE(unique.diagnostics.status == AssistStatus::kOk);
  const PatchMidiClip* unique_patch = sole_patch(unique);
  const sonare::midi::Ump c_on = sonare::midi::make_midi1_note_on(0, 0, 57, 85);
  const sonare::midi::Ump c_off = sonare::midi::make_midi1_note_off(0, 0, 57, 0);
  const sonare::midi::Ump e_on = sonare::midi::make_midi1_note_on(0, 0, 60, 85);
  const sonare::midi::Ump e_off = sonare::midi::make_midi1_note_off(0, 0, 60, 0);
  CHECK(unique_patch->patch().add ==
        MidiClipEventList{MidiClipEvent{0.0, c_on.words[0], c_on.words[1], 0},
                          MidiClipEvent{300.0, c_off.words[0], c_off.words[1], 0},
                          MidiClipEvent{100.0, e_on.words[0], e_on.words[1], 0},
                          MidiClipEvent{150.0, e_off.words[0], e_off.words[1], 0}});

  // Same onset and note: group, then channel, then end decide. Completion order
  // is the reverse of the expected order, and one outside note sits before the scope.
  MidiClipEventList tie_events;
  tie_events.push_back(note_on_event(0.0, 62, 90, 3, 0));
  tie_events.push_back(note_off_event(20.0, 62, 3, 0));
  tie_events.push_back(note_on_event(100.0, 60, 90, 1, 2));
  tie_events.push_back(note_on_event(100.0, 60, 90, 1, 2));
  tie_events.push_back(note_on_event(100.0, 60, 90, 1, 0));
  tie_events.push_back(note_on_event(100.0, 60, 90, 0, 5));
  tie_events.push_back(note_on_event(100.0, 60, 90, 0, 3));
  tie_events.push_back(note_off_event(110.0, 60, 1, 2));
  tie_events.push_back(note_off_event(120.0, 60, 1, 2));
  tie_events.push_back(note_off_event(150.0, 60, 1, 0));
  tie_events.push_back(note_off_event(180.0, 60, 0, 5));
  tie_events.push_back(note_off_event(190.0, 60, 0, 3));
  set_events(&f, std::move(tie_events));
  AssistRequest scoped = request_with(target_params(f.clip_id));
  scoped.scope.start_ppq = 100.0;
  scoped.scope.end_ppq = 200.0;
  const AssistResult tie = DiatonicHarmonizer().derive(f.view(), scoped, {}, queries);
  REQUIRE(tie.diagnostics.status == AssistStatus::kOk);

  struct Expected {
    uint8_t group;
    uint8_t channel;
    double end;
  };
  const Expected order[] = {
      {0, 3, 190.0}, {0, 5, 180.0}, {1, 0, 150.0}, {1, 2, 110.0}, {1, 2, 120.0}};
  MidiClipEventList expected_tie;
  for (const Expected& e : order) {
    const sonare::midi::Ump on = sonare::midi::make_midi1_note_on(e.group, e.channel, 57, 77);
    const sonare::midi::Ump off = sonare::midi::make_midi1_note_off(e.group, e.channel, 57, 0);
    expected_tie.push_back(MidiClipEvent{100.0, on.words[0], on.words[1], 0});
    expected_tie.push_back(MidiClipEvent{e.end, off.words[0], off.words[1], 0});
  }
  CHECK(sole_patch(tie)->patch().add == expected_tie);
}

TEST_CASE("built-in modules keep a deterministic partial patch at a deadline",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)},
           {chord_symbol(0.0, 960.0, 0, ChordQuality::kMajor)});
  set_events(&f, c_major_line());

  const TimelineHarmonyContext harmony;
  AdvancingJudge judge(std::chrono::milliseconds(3));
  const AssistQueryContext queries{&harmony, nullptr, &judge};
  AssistRequest harmonizer_request = request_with(target_params(f.clip_id));
  harmonizer_request.budget.max_time_ms = 5;
  AssistRequest chord_request = harmonizer_request;

  g_assist_test_now = {};
  {
    sonare::midi::assist::detail::NowFnScope clock(&assist_test_now);
    const AssistResult result =
        DiatonicHarmonizer().derive(f.view(), harmonizer_request, {}, queries);
    CHECK(result.diagnostics.status == AssistStatus::kBudgetTruncated);
    CHECK(result.diagnostics.iterations_consumed == 2u);
    REQUIRE(result.commands.size() == 1u);
    CHECK(emitted_notes(*sole_patch(result)).size() == 1u);
    const json::Value payload = json::parse_strict(result.candidate_payload);
    REQUIRE(payload.contains("decisions"));
    CHECK(payload["decisions"].as_array().size() <= result.diagnostics.iterations_consumed);
  }

  g_assist_test_now = {};
  {
    sonare::midi::assist::detail::NowFnScope clock(&assist_test_now);
    const AssistResult result =
        ChordToneGenerator(nullptr, &judge).generate(f.view(), chord_request);
    CHECK(result.diagnostics.status == AssistStatus::kBudgetTruncated);
    CHECK(result.diagnostics.iterations_consumed == 1u);
    REQUIRE(result.commands.size() == 1u);
    CHECK(emitted_notes(*sole_patch(result)).size() == 1u);
    const json::Value payload = json::parse_strict(result.candidate_payload);
    REQUIRE(payload.contains("decisions"));
    CHECK(payload["decisions"].as_array().size() <= result.diagnostics.iterations_consumed);
  }
}

TEST_CASE("built-in modules prefer iteration when the next candidate meets both caps",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)},
           {chord_symbol(0.0, 960.0, 0, ChordQuality::kMajor),
            chord_symbol(960.0, 1920.0, 7, ChordQuality::kMajor)});
  set_events(&f, c_major_line());

  AssistRequest request = request_with(target_params(f.clip_id));
  request.budget.max_iterations = 1;
  request.budget.max_time_ms = 5;

  const TimelineHarmonyContext harmony;
  ArmDeadlineAfterFirstJudge judge;
  const AssistQueryContext queries{&harmony, nullptr, &judge};

  g_assist_test_now = {};
  g_assist_test_reads_until_expiry = -1;
  {
    sonare::midi::assist::detail::NowFnScope clock(&assist_test_now);
    const AssistResult result = DiatonicHarmonizer().derive(f.view(), request, {}, queries);
    CHECK(result.diagnostics.status == AssistStatus::kBudgetTruncated);
    CHECK(result.diagnostics.reason.find("iteration budget") != std::string::npos);
    CHECK(result.diagnostics.iterations_consumed == 1u);
    REQUIRE(result.commands.size() == 1u);
    CHECK(emitted_notes(*sole_patch(result)).size() == 1u);
    const json::Value payload = json::parse_strict(result.candidate_payload);
    REQUIRE(payload.contains("decisions"));
    CHECK(payload["decisions"].as_array().size() <= result.diagnostics.iterations_consumed);
  }

  g_assist_test_now = {};
  g_assist_test_reads_until_expiry = -1;
  ChordToneGeneratorConfig bass;
  bass.voice_count = 1;
  ArmDeadlineAfterFirstJudge chord_judge;
  {
    sonare::midi::assist::detail::NowFnScope clock(&assist_test_now);
    const AssistResult result =
        ChordToneGenerator(nullptr, &chord_judge, bass).generate(f.view(), request);
    CHECK(result.diagnostics.status == AssistStatus::kBudgetTruncated);
    CHECK(result.diagnostics.reason.find("iteration budget") != std::string::npos);
    CHECK(result.diagnostics.iterations_consumed == 1u);
    REQUIRE(result.commands.size() == 1u);
    CHECK(emitted_notes(*sole_patch(result)).size() == 1u);
    const json::Value payload = json::parse_strict(result.candidate_payload);
    REQUIRE(payload.contains("decisions"));
    CHECK(payload["decisions"].as_array().size() <= result.diagnostics.iterations_consumed);
  }

  // Consuming the exact final candidate is complete when no deadline is
  // active: there is no unprocessed work for the iteration cap to truncate.
  AssistRequest exact = request_with(target_params(f.clip_id));
  exact.budget.max_iterations = 4;
  const AssistResult exact_harmonizer = DiatonicHarmonizer().derive(
      f.view(), exact, {}, AssistQueryContext{&harmony, nullptr, nullptr});
  CHECK(exact_harmonizer.diagnostics.status == AssistStatus::kOk);
  CHECK(exact_harmonizer.diagnostics.iterations_consumed == 4u);

  exact.budget.max_iterations = 2;
  const AssistResult exact_chord =
      ChordToneGenerator(nullptr, nullptr, bass).generate(f.view(), exact);
  CHECK(exact_chord.diagnostics.status == AssistStatus::kOk);
  CHECK(exact_chord.diagnostics.iterations_consumed == 2u);
}

TEST_CASE("the harmonizer scopes source content on the timeline and writes target content",
          "[assist][modules]") {
  struct ClipPosition {
    double start_ppq;
    double source_offset_ppq;
  };
  const ClipPosition positions[] = {{0.0, 0.0}, {0.0, 120.0}, {1440.0, 0.0}, {1440.0, 120.0}};
  DiatonicHarmonizer harmonizer;

  for (const ClipPosition source_position : positions) {
    for (const ClipPosition target_position : positions) {
      Fixture f = make_fixture();
      const ClipId target_clip_id = add_midi_clip(&f);
      set_clip_position(&f, f.clip_id, source_position.start_ppq,
                        source_position.source_offset_ppq);
      set_clip_position(&f, target_clip_id, target_position.start_ppq,
                        target_position.source_offset_ppq);

      annotate(&f, {key_segment(0.0, 4000.0, 0, KeyMode::kMajor)}, {});
      const double source_content_ppq =
          source_position.source_offset_ppq + 1920.0 - source_position.start_ppq;
      MidiClipEventList events;
      events.push_back(note_on_event(source_content_ppq, 60, 100));
      events.push_back(note_off_event(source_content_ppq + 240.0, 60));
      set_events(&f, f.clip_id, std::move(events));

      const TimelineHarmonyContext harmony;
      const RangeScaleJudge judge(&harmony, nullptr);
      const AssistQueryContext queries{&harmony, nullptr, &judge};
      AssistRequest request = request_with(source_target_params(target_clip_id, f.clip_id));
      request.scope.start_ppq = 1920.0;
      request.scope.end_ppq = 1921.0;
      const AssistResult result = harmonizer.derive(f.view(), request, {}, queries);

      INFO("source start=" << source_position.start_ppq
                           << ", source offset=" << source_position.source_offset_ppq
                           << ", target start=" << target_position.start_ppq
                           << ", target offset=" << target_position.source_offset_ppq);
      REQUIRE(result.diagnostics.status == AssistStatus::kOk);
      const std::vector<EmittedNote> notes = emitted_notes(*sole_patch(result));
      REQUIRE(notes.size() == 1u);
      const double expected_on_content =
          target_position.source_offset_ppq + 1920.0 - target_position.start_ppq;
      const double expected_off_content = expected_on_content + 240.0;
      CHECK(notes[0].on_ppq == Catch::Approx(expected_on_content));
      CHECK(notes[0].off_ppq == Catch::Approx(expected_off_content));
      CHECK(notes[0].on_ppq + target_position.start_ppq - target_position.source_offset_ppq ==
            Catch::Approx(1920.0));
      CHECK(notes[0].off_ppq - notes[0].on_ppq == Catch::Approx(240.0));
      const json::Value payload = json::parse_strict(result.candidate_payload);
      REQUIRE(payload.contains("decisions"));
      REQUIRE(payload["decisions"].is_array());
      REQUIRE(payload["decisions"].as_array().size() == 1u);
      CHECK(payload["decisions"][0]["sourcePpq"].as_number() == Catch::Approx(source_content_ppq));
    }
  }
}

TEST_CASE("the harmonizer sends timeline PPQ to harmony and judge before target conversion",
          "[assist][modules]") {
  Fixture f = make_fixture();
  const ClipId target_clip_id = add_midi_clip(&f);
  set_clip_position(&f, f.clip_id, 1440.0, 120.0);
  set_clip_position(&f, target_clip_id, 1440.0, 0.0);
  // The raw source-content position is 600, while the sounding timeline
  // position is 1920. Distinct keys make the coordinate choice observable.
  annotate(&f,
           {key_segment(0.0, 1000.0, 2, KeyMode::kMajor),
            key_segment(1000.0, 3000.0, 0, KeyMode::kMajor)},
           {});
  MidiClipEventList events;
  events.push_back(note_on_event(600.0, 60, 100));
  events.push_back(note_off_event(840.0, 60));
  set_events(&f, f.clip_id, std::move(events));

  RecordingHarmony harmony;
  RecordingJudge judge;
  judge.ppq_adjustment = 60.0;
  const AssistQueryContext queries{&harmony, nullptr, &judge};
  AssistRequest request = request_with(source_target_params(target_clip_id, f.clip_id));
  request.scope.start_ppq = 1920.0;
  request.scope.end_ppq = 1921.0;
  const AssistResult result = DiatonicHarmonizer().derive(f.view(), request, {}, queries);

  REQUIRE(result.diagnostics.status == AssistStatus::kOk);
  REQUIRE(harmony.scale_queries.size() == 1u);
  REQUIRE(judge.seen_ppq.size() == 1u);
  CHECK(harmony.scale_queries[0] == Catch::Approx(1920.0));
  CHECK(judge.seen_ppq[0] == Catch::Approx(1920.0));
  const std::vector<EmittedNote> notes = emitted_notes(*sole_patch(result));
  REQUIRE(notes.size() == 1u);
  // The key is read at the timeline position (C major), not the content
  // position, where D major at 600 would give a different third.
  CHECK(notes[0].note == 57);
  CHECK(notes[0].on_ppq == Catch::Approx(540.0));
  CHECK(notes[0].off_ppq == Catch::Approx(780.0));
  CHECK(notes[0].on_ppq + 1440.0 == Catch::Approx(1980.0));

  // A scope around the raw source-content value must not select the note: the
  // same source is only in scope at its translated timeline position above.
  RecordingHarmony raw_scope_harmony;
  RecordingJudge raw_scope_judge;
  AssistRequest raw_scope = request_with(source_target_params(target_clip_id, f.clip_id));
  raw_scope.scope.start_ppq = 600.0;
  raw_scope.scope.end_ppq = 601.0;
  const AssistResult raw_scope_result = DiatonicHarmonizer().derive(
      f.view(), raw_scope, {}, AssistQueryContext{&raw_scope_harmony, nullptr, &raw_scope_judge});
  CHECK(raw_scope_result.commands.empty());
  CHECK(raw_scope_result.diagnostics.status == AssistStatus::kEmpty);
  CHECK(raw_scope_harmony.scale_queries.empty());
  CHECK(raw_scope_judge.seen_ppq.empty());
}

TEST_CASE("a note with no key under it is dropped with its reason recorded", "[assist][modules]") {
  Fixture f = make_fixture();
  // The key covers only the first two beats.
  annotate(&f, {key_segment(0.0, 960.0, 0, KeyMode::kMajor)}, {});
  set_events(&f, c_major_line());
  const TimelineHarmonyContext harmony;
  const RangeScaleJudge judge(&harmony, nullptr);
  const AssistQueryContext queries{&harmony, nullptr, &judge};
  DiatonicHarmonizer harmonizer;

  const AssistResult result =
      harmonizer.derive(f.view(), request_with(target_params(f.clip_id)), {}, queries);
  CHECK(result.diagnostics.status == AssistStatus::kOk);
  CHECK(note_numbers(emitted_notes(*sole_patch(result))) == std::vector<int>{57, 60});
  // Every source note is accounted for in the payload, dropped ones included.
  CHECK(result.diagnostics.iterations_consumed == 4u);
  CHECK(reason_mentions(result.candidate_payload, "no key is annotated here"));
  CHECK(reason_mentions(result.candidate_payload, "\"sourceNote\":67"));
}

TEST_CASE("an explicitly modelled voice outranks the params' range", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)},
           {chord_symbol(0.0, 1920.0, 0, ChordQuality::kMajor)});
  set_events(&f, c_major_line());
  const TimelineHarmonyContext harmony;
  const IntervalDissonanceAnalyzer dissonance(&harmony);
  const RangeScaleJudge judge(&harmony, &dissonance);
  const AssistQueryContext queries{&harmony, &dissonance, &judge};
  DiatonicHarmonizer harmonizer;

  VoiceModel cantus;
  cantus.low_note = 70;
  cantus.high_note = 84;
  const AssistResult result =
      harmonizer.derive(f.view(), request_with(target_params(f.clip_id)), {cantus}, queries);

  // Every derived note is folded up into the modelled voice instead of the
  // params' default 36..84.
  const std::vector<EmittedNote> notes = emitted_notes(*sole_patch(result));
  CHECK(note_numbers(notes) == std::vector<int>{81, 72, 76, 81});
  for (const EmittedNote& note : notes) {
    CHECK(note.note >= 70);
    CHECK(note.note <= 84);
  }
}

TEST_CASE("the interval the harmonizer moves by is configurable in scale steps",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)}, {});
  set_events(&f, c_major_line());
  const TimelineHarmonyContext harmony;
  const RangeScaleJudge judge(&harmony, nullptr);
  const AssistQueryContext queries{&harmony, nullptr, &judge};

  DiatonicHarmonizerConfig sixth_below;
  sixth_below.interval_steps = -5;
  DiatonicHarmonizer harmonizer(sixth_below);
  const AssistResult result =
      harmonizer.derive(f.view(), request_with(target_params(f.clip_id)), {}, queries);
  CHECK(note_numbers(emitted_notes(*sole_patch(result))) == std::vector<int>{52, 55, 59, 64});

  DiatonicHarmonizerConfig third_above;
  third_above.interval_steps = 2;
  DiatonicHarmonizer upper(third_above);
  const AssistResult above =
      upper.derive(f.view(), request_with(target_params(f.clip_id)), {}, queries);
  CHECK(note_numbers(emitted_notes(*sole_patch(above))) == std::vector<int>{64, 67, 71, 76});
}

// ===========================================================================
// Driver: a refusal settles the whole run
// ===========================================================================

TEST_CASE("one module refusing settles the run even when another produced commands",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)},
           {chord_symbol(0.0, 1920.0, 0, ChordQuality::kMajor)});
  set_events(&f, c_major_line());

  TimelineHarmonyContext harmony;
  IntervalDissonanceAnalyzer dissonance(&harmony);
  RangeScaleJudge judge(&harmony, &dissonance);
  DiatonicHarmonizer harmonizer;

  SECTION("the contributor runs first and the refusal comes second") {
    StubGenerator contributor(f.clip_id, AssistStatus::kOk);
    AssistRegistry registry;
    registry.register_harmony_context(&harmony);
    registry.register_dissonance_analyzer(&dissonance);
    registry.register_judge(&judge);
    registry.register_generator(&contributor);
    registry.register_counterpoint(&harmonizer);

    // The control first: with a request the harmonizer accepts, BOTH slots
    // contribute. Without this the empty command list below would be just as
    // consistent with a stub that never produced anything.
    const AssistResult accepted =
        CompositionAssist(registry).run(f.view(), request_with(target_params(f.clip_id)));
    CHECK(accepted.diagnostics.status == AssistStatus::kOk);
    CHECK(accepted.commands.size() == 2u);

    // Now the same registry with a request the harmonizer refuses. The stub
    // still produces its patch, and the run is refused all the same.
    const AssistResult refused = CompositionAssist(registry).run(f.view(), AssistRequest{});
    CHECK(refused.diagnostics.status == AssistStatus::kRejected);
    CHECK(refused.commands.empty());
    CHECK(reason_mentions(refused.diagnostics.reason, "target_clip_id"));
    // A refusal is not a discard: nothing threw and nothing failed validation.
    CHECK(refused.diagnostics.slots_discarded == 0u);
    // The stub did run -- its payload and its iteration are both accounted for,
    // so the empty command list is the driver clearing them, not a slot that
    // never fired.
    // Exactly the stub's own iteration: it ran and was counted, while the
    // harmonizer refused before doing any work.
    CHECK(refused.diagnostics.iterations_consumed == 1u);
    CHECK(reason_mentions(refused.candidate_payload, "test.stub_generator"));

    // What settles the run is the refusal, not whether some other slot happened
    // to produce commands -- so a refusal WITH a contributor and a refusal
    // without one have to land on the same status. Conditioning the refusal on
    // an empty run splits exactly this pair, and nothing else here would notice.
    AssistRegistry lone;
    lone.register_harmony_context(&harmony);
    lone.register_dissonance_analyzer(&dissonance);
    lone.register_judge(&judge);
    lone.register_counterpoint(&harmonizer);
    const AssistResult refused_alone = CompositionAssist(lone).run(f.view(), AssistRequest{});
    CHECK(refused_alone.commands.empty());
    CHECK(refused_alone.diagnostics.status == AssistStatus::kRejected);
    CHECK(refused.diagnostics.status == refused_alone.diagnostics.status);
  }

  SECTION("the refusal comes first and the contributor runs second") {
    StubGenerator refuser(f.clip_id, AssistStatus::kRejected);
    AssistRegistry registry;
    registry.register_harmony_context(&harmony);
    registry.register_dissonance_analyzer(&dissonance);
    registry.register_judge(&judge);
    registry.register_generator(&refuser);
    registry.register_counterpoint(&harmonizer);

    // Params the harmonizer is perfectly happy with: it derives four notes.
    const AssistResult refused =
        CompositionAssist(registry).run(f.view(), request_with(target_params(f.clip_id)));
    CHECK(refused.diagnostics.status == AssistStatus::kRejected);
    CHECK(refused.commands.empty());
    CHECK(reason_mentions(refused.diagnostics.reason, kStubRefusal));
    CHECK(refused.diagnostics.slots_discarded == 0u);

    // The control: swap the refusing stub for a contributing one and the same
    // request over the same project comes back with both slots' commands, so
    // the harmonizer really did have something to contribute above.
    StubGenerator contributor(f.clip_id, AssistStatus::kOk);
    AssistRegistry permissive;
    permissive.register_harmony_context(&harmony);
    permissive.register_dissonance_analyzer(&dissonance);
    permissive.register_judge(&judge);
    permissive.register_generator(&contributor);
    permissive.register_counterpoint(&harmonizer);
    const AssistResult accepted =
        CompositionAssist(permissive).run(f.view(), request_with(target_params(f.clip_id)));
    CHECK(accepted.diagnostics.status == AssistStatus::kOk);
    CHECK(accepted.commands.size() == 2u);
  }
}

TEST_CASE("a refused run leaves the project untouched even with commands in flight",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)}, {});
  set_events(&f, c_major_line());
  const std::string before = f.serialized();

  TimelineHarmonyContext harmony;
  RangeScaleJudge judge(&harmony, nullptr);
  DiatonicHarmonizer harmonizer;
  StubGenerator contributor(f.clip_id, AssistStatus::kOk);

  AssistRegistry registry;
  registry.register_harmony_context(&harmony);
  registry.register_judge(&judge);
  registry.register_generator(&contributor);
  registry.register_counterpoint(&harmonizer);

  const AssistResult refused = CompositionAssist(registry).run(f.view(), AssistRequest{});
  CHECK(refused.diagnostics.status == AssistStatus::kRejected);
  CHECK(refused.commands.empty());
  // Nothing to apply, and the driver never touched the project in the first
  // place: a caller fixes the request and runs again.
  CHECK(f.serialized() == before);

  // And the control: the same registry over a request the harmonizer accepts
  // does produce commands, so the empty list above is the refusal acting.
  const AssistResult accepted =
      CompositionAssist(registry).run(f.view(), request_with(target_params(f.clip_id)));
  CHECK(accepted.diagnostics.status == AssistStatus::kOk);
  CHECK(accepted.commands.size() == 2u);
  CHECK(f.serialized() == before);
}

// ===========================================================================
// ChordToneGenerator
// ===========================================================================

TEST_CASE("the chord tone generator voices each chord ascending from the low note",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)},
           {chord_symbol(0.0, 960.0, 0, ChordQuality::kMajor),
            chord_symbol(960.0, 1920.0, 7, ChordQuality::kMajor)});

  ChordToneGenerator generator;
  const AssistResult result = generator.generate(f.view(), request_with(target_params(f.clip_id)));

  CHECK(result.diagnostics.status == AssistStatus::kOk);
  const PatchMidiClip* patch = sole_patch(result);
  CHECK(patch->patch().clip_id == f.clip_id);

  const std::vector<EmittedNote> notes = emitted_notes(*patch);
  REQUIRE(notes.size() == 6u);
  // C major from 36 up, then G major from 36 up: three tones each, ascending.
  CHECK(note_numbers(notes) == std::vector<int>{36, 40, 43, 38, 43, 47});
  for (size_t i = 0; i < 3; ++i) {
    CHECK(notes[i].on_ppq == 0.0);
    CHECK(notes[i].off_ppq == 960.0);
  }
  for (size_t i = 3; i < 6; ++i) {
    CHECK(notes[i].on_ppq == 960.0);
    CHECK(notes[i].off_ppq == 1920.0);
  }
  // Each block is strictly ascending and starts at or above the low note.
  CHECK(notes[0].note < notes[1].note);
  CHECK(notes[1].note < notes[2].note);
  CHECK(notes[3].note < notes[4].note);
  CHECK(notes[4].note < notes[5].note);
  for (const EmittedNote& note : notes) CHECK(note.note >= 36);
}

TEST_CASE("the chord tone generator writes target content for shifted clips", "[assist][modules]") {
  struct ClipPosition {
    double start_ppq;
    double source_offset_ppq;
  };
  const ClipPosition positions[] = {{0.0, 0.0}, {0.0, 120.0}, {1440.0, 0.0}, {1440.0, 120.0}};

  for (const ClipPosition position : positions) {
    Fixture f = make_fixture();
    set_clip_position(&f, f.clip_id, position.start_ppq, position.source_offset_ppq);
    annotate(&f, {}, {chord_symbol(1920.0, 2160.0, 0, ChordQuality::kMajor)});

    const AssistResult result =
        ChordToneGenerator().generate(f.view(), request_with(target_params(f.clip_id)));
    INFO("target start=" << position.start_ppq << ", target offset=" << position.source_offset_ppq);
    REQUIRE(result.diagnostics.status == AssistStatus::kOk);
    const std::vector<EmittedNote> notes = emitted_notes(*sole_patch(result));
    REQUIRE(notes.size() == 3u);
    const double expected_on = position.source_offset_ppq + 1920.0 - position.start_ppq;
    const double expected_off = expected_on + 240.0;
    for (const EmittedNote& note : notes) {
      CHECK(note.on_ppq == Catch::Approx(expected_on));
      CHECK(note.off_ppq == Catch::Approx(expected_off));
      CHECK(note.on_ppq + position.start_ppq - position.source_offset_ppq == Catch::Approx(1920.0));
    }
    const json::Value payload = json::parse_strict(result.candidate_payload);
    REQUIRE(payload.contains("decisions"));
    REQUIRE(payload["decisions"].is_array());
    REQUIRE(payload["decisions"].as_array().size() == 1u);
    CHECK(payload["decisions"][0]["startPpq"].as_number() == Catch::Approx(1920.0));
  }
}

TEST_CASE("the chord target conversion follows an adjusted judge onset", "[assist][modules]") {
  Fixture f = make_fixture();
  set_clip_position(&f, f.clip_id, 1440.0, 120.0);
  annotate(&f, {}, {chord_symbol(1920.0, 2160.0, 0, ChordQuality::kMajor)});

  RecordingJudge judge;
  judge.ppq_adjustment = 60.0;
  ChordToneGenerator generator(nullptr, &judge);
  const AssistResult result = generator.generate(f.view(), request_with(target_params(f.clip_id)));

  REQUIRE(result.diagnostics.status == AssistStatus::kOk);
  REQUIRE(judge.seen_ppq.size() == 3u);
  for (const double ppq : judge.seen_ppq) CHECK(ppq == Catch::Approx(1920.0));
  const std::vector<EmittedNote> notes = emitted_notes(*sole_patch(result));
  REQUIRE(notes.size() == 3u);
  for (const EmittedNote& note : notes) {
    CHECK(note.on_ppq == Catch::Approx(660.0));
    CHECK(note.off_ppq == Catch::Approx(900.0));
  }
}

TEST_CASE("voice_count and the voice range decide how a chord is voiced", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {}, {chord_symbol(0.0, 960.0, 0, ChordQuality::kMajor)});

  ChordToneGeneratorConfig bass;
  bass.voice_count = 1;
  ChordToneGenerator bass_line(nullptr, nullptr, bass);
  const AssistResult one = bass_line.generate(f.view(), request_with(target_params(f.clip_id)));
  CHECK(note_numbers(emitted_notes(*sole_patch(one))) == std::vector<int>{36});

  // A higher floor moves the whole voicing up to the first tone at or above it.
  const std::string high_params =
      "{\"target_clip_id\": " + std::to_string(f.clip_id) + ", \"low_note\": 60}";
  ChordToneGenerator triad;
  const AssistResult voiced = triad.generate(f.view(), request_with(high_params));
  CHECK(note_numbers(emitted_notes(*sole_patch(voiced))) == std::vector<int>{60, 64, 67});

  // The count is clamped to what fits: a five-voice request over a one-octave
  // window gets the tones that are in it, not five copies of the lowest.
  ChordToneGeneratorConfig wide;
  wide.voice_count = 5;
  ChordToneGenerator five(nullptr, nullptr, wide);
  const std::string window = "{\"target_clip_id\": " + std::to_string(f.clip_id) +
                             ", \"low_note\": 60, \"high_note\": 71}";
  const std::vector<int> in_window =
      note_numbers(emitted_notes(*sole_patch(five.generate(f.view(), request_with(window)))));
  CHECK(in_window == std::vector<int>{60, 64, 67});

  // An out-of-domain voice count is refused rather than silently clamped.
  ChordToneGeneratorConfig none;
  none.voice_count = 0;
  ChordToneGenerator silent(nullptr, nullptr, none);
  const AssistResult refused = silent.generate(f.view(), request_with(target_params(f.clip_id)));
  CHECK(refused.commands.empty());
  CHECK(reason_mentions(refused.diagnostics.reason, "voice_count"));
  // voice_count is a construction-time config rather than a field of the
  // request's params blob, and it is reported as an empty outcome.
  CHECK(refused.diagnostics.status == AssistStatus::kEmpty);
}

TEST_CASE("the chord tone generator sets velocity from base_velocity", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {}, {chord_symbol(0.0, 960.0, 0, ChordQuality::kMajor)});

  ChordToneGenerator generator;
  const AssistResult defaulted =
      generator.generate(f.view(), request_with(target_params(f.clip_id)));
  for (const EmittedNote& note : emitted_notes(*sole_patch(defaulted))) {
    CHECK(note.velocity == 90);
  }

  const std::string params =
      "{\"target_clip_id\": " + std::to_string(f.clip_id) + ", \"base_velocity\": 40}";
  const AssistResult quiet = generator.generate(f.view(), request_with(params));
  for (const EmittedNote& note : emitted_notes(*sole_patch(quiet))) {
    CHECK(note.velocity == 40);
  }
}

TEST_CASE("a project with no chord annotated comes back empty with a reason", "[assist][modules]") {
  Fixture f = make_fixture();
  ChordToneGenerator generator;

  const AssistResult unannotated =
      generator.generate(f.view(), request_with(target_params(f.clip_id)));
  CHECK(unannotated.commands.empty());
  CHECK(unannotated.diagnostics.status == AssistStatus::kEmpty);
  CHECK(reason_mentions(unannotated.diagnostics.reason, "annotates no chords"));

  // The generator draws the same line the harmonizer does: a request naming no
  // clip is REFUSED, while a project that annotates no chords is merely empty.
  AssistResult no_target;
  REQUIRE_NOTHROW(no_target = generator.generate(f.view(), AssistRequest{}));
  CHECK(no_target.commands.empty());
  CHECK(no_target.diagnostics.status == AssistStatus::kRejected);
  CHECK(reason_mentions(no_target.diagnostics.reason, "target_clip_id"));
  CHECK(no_target.diagnostics.status != unannotated.diagnostics.status);

  // A chord carrying no usable quality is reported per chord, not guessed at.
  ChordSymbol unusable;
  unusable.start_ppq = 0.0;
  unusable.end_ppq = 960.0;
  annotate(&f, {}, {unusable});
  const AssistResult unusable_result =
      generator.generate(f.view(), request_with(target_params(f.clip_id)));
  CHECK(unusable_result.commands.empty());
  CHECK(unusable_result.diagnostics.status == AssistStatus::kEmpty);
  CHECK(reason_mentions(unusable_result.candidate_payload, "no usable root or quality"));
}

TEST_CASE("the chord tone generator is deterministic and leaves the project untouched",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)},
           {chord_symbol(0.0, 960.0, 0, ChordQuality::kMajor),
            chord_symbol(960.0, 1920.0, 7, ChordQuality::kMajor)});
  set_events(&f, c_major_line());

  const std::string before = f.serialized();
  const auto midi_before = f.history.midi_content();

  ChordToneGenerator generator;
  AssistRequest request = request_with(target_params(f.clip_id));
  request.seed = 7;
  const AssistResult first = generator.generate(f.view(), request);
  request.seed = 8;
  const AssistResult second = generator.generate(f.view(), request);

  CHECK(sole_patch(first)->patch().add == sole_patch(second)->patch().add);
  CHECK(first.candidate_payload == second.candidate_payload);

  CHECK(f.serialized() == before);
  CHECK(f.history.midi_content() == midi_before);
}

TEST_CASE("the chord tone generator honours the scope and the iteration budget",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {},
           {chord_symbol(0.0, 960.0, 0, ChordQuality::kMajor),
            chord_symbol(960.0, 1920.0, 7, ChordQuality::kMajor)});
  ChordToneGenerator generator;

  AssistRequest scoped = request_with(target_params(f.clip_id));
  scoped.scope.start_ppq = 960.0;
  CHECK(note_numbers(emitted_notes(*sole_patch(generator.generate(f.view(), scoped)))) ==
        std::vector<int>{38, 43, 47});

  AssistRequest budgeted = request_with(target_params(f.clip_id));
  budgeted.budget.max_iterations = 1;
  const AssistResult truncated = generator.generate(f.view(), budgeted);
  CHECK(truncated.diagnostics.status == AssistStatus::kBudgetTruncated);
  CHECK(truncated.diagnostics.iterations_consumed == 1u);
  CHECK(note_numbers(emitted_notes(*sole_patch(truncated))) == std::vector<int>{36, 40, 43});
}

TEST_CASE("chord acceptance reflects emitted patch notes rather than rejected voices",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {}, {chord_symbol(0.0, 960.0, 0, ChordQuality::kMajor)});
  const AssistRequest request = request_with(target_params(f.clip_id));

  ScriptedJudge all_rejected({false, false, false});
  const AssistResult rejected =
      ChordToneGenerator(nullptr, &all_rejected).generate(f.view(), request);
  CHECK(rejected.diagnostics.status == AssistStatus::kEmpty);
  CHECK(rejected.commands.empty());
  CHECK(decision_acceptances(rejected) == std::vector<bool>{false});

  ScriptedJudge partly_accepted({false, true, false});
  const AssistResult partial =
      ChordToneGenerator(nullptr, &partly_accepted).generate(f.view(), request);
  CHECK(partial.diagnostics.status == AssistStatus::kOk);
  REQUIRE(partial.commands.size() == 1u);
  CHECK(emitted_notes(*sole_patch(partial)).size() == 1u);
  CHECK(decision_acceptances(partial) == std::vector<bool>{true});

  ScriptedJudge all_accepted({true, true, true});
  const AssistResult accepted =
      ChordToneGenerator(nullptr, &all_accepted).generate(f.view(), request);
  CHECK(accepted.diagnostics.status == AssistStatus::kOk);
  REQUIRE(accepted.commands.size() == 1u);
  CHECK(emitted_notes(*sole_patch(accepted)).size() == 3u);
  CHECK(decision_acceptances(accepted) == std::vector<bool>{true});

  // A later chord must derive its own flag from its own patch delta. This
  // catches an implementation that turns every later decision true merely
  // because an earlier chord already emitted notes.
  Fixture mixed_fixture = make_fixture();
  annotate(&mixed_fixture, {},
           {chord_symbol(0.0, 960.0, 0, ChordQuality::kMajor),
            chord_symbol(960.0, 1920.0, 7, ChordQuality::kMajor)});
  ScriptedJudge first_only({true, true, true, false, false, false});
  const AssistResult mixed =
      ChordToneGenerator(nullptr, &first_only)
          .generate(mixed_fixture.view(), request_with(target_params(mixed_fixture.clip_id)));
  CHECK(mixed.diagnostics.status == AssistStatus::kOk);
  REQUIRE(mixed.commands.size() == 1u);
  const std::vector<EmittedNote> mixed_notes = emitted_notes(*sole_patch(mixed));
  REQUIRE(mixed_notes.size() == 3u);
  CHECK(decision_acceptances(mixed) == std::vector<bool>{true, false});
  for (const EmittedNote& note : mixed_notes) CHECK(note.on_ppq == 0.0);

  ChordSymbol unusable;
  unusable.start_ppq = 0.0;
  unusable.end_ppq = 960.0;
  annotate(&f, {}, {unusable});
  const AssistResult no_usable_tones = ChordToneGenerator().generate(f.view(), request);
  CHECK(no_usable_tones.diagnostics.status == AssistStatus::kEmpty);
  CHECK(no_usable_tones.commands.empty());
  CHECK(decision_acceptances(no_usable_tones) == std::vector<bool>{false});
}

TEST_CASE("chord acceptance distinguishes deadline before adoption from partial adoption",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {}, {chord_symbol(0.0, 960.0, 0, ChordQuality::kMajor)});

  AssistRequest before_adoption = request_with(target_params(f.clip_id));
  before_adoption.budget.max_time_ms = 1;
  AdvancingJudge before_judge(std::chrono::milliseconds(3));
  g_assist_test_now = {};
  g_assist_test_reads_until_expiry = -1;
  AssistResult before;
  {
    sonare::midi::assist::detail::NowFnScope clock(&assist_test_now);
    before = ChordToneGenerator(nullptr, &before_judge).generate(f.view(), before_adoption);
  }
  CHECK(before.diagnostics.status == AssistStatus::kBudgetTruncated);
  CHECK(before.diagnostics.iterations_consumed == 1u);
  CHECK(before.commands.empty());
  CHECK(decision_acceptances(before) == std::vector<bool>{false});

  AssistRequest partial_adoption = request_with(target_params(f.clip_id));
  partial_adoption.budget.max_time_ms = 5;
  AdvancingJudge partial_judge(std::chrono::milliseconds(3));
  g_assist_test_now = {};
  g_assist_test_reads_until_expiry = -1;
  AssistResult partial;
  {
    sonare::midi::assist::detail::NowFnScope clock(&assist_test_now);
    partial = ChordToneGenerator(nullptr, &partial_judge).generate(f.view(), partial_adoption);
  }
  CHECK(partial.diagnostics.status == AssistStatus::kBudgetTruncated);
  CHECK(partial.diagnostics.iterations_consumed == 1u);
  REQUIRE(partial.commands.size() == 1u);
  CHECK(emitted_notes(*sole_patch(partial)).size() == 1u);
  CHECK(decision_acceptances(partial) == std::vector<bool>{true});
}

TEST_CASE("the chord budget ignores zero-length and scope-outside chords", "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {},
           {chord_symbol(0.0, 240.0, 7, ChordQuality::kMajor),
            chord_symbol(240.0, 240.0, 5, ChordQuality::kMajor),
            chord_symbol(480.0, 960.0, 0, ChordQuality::kMajor),
            chord_symbol(960.0, 1440.0, 7, ChordQuality::kMajor)});

  AssistRequest request = request_with(target_params(f.clip_id));
  request.scope.start_ppq = 480.0;
  request.scope.end_ppq = 1920.0;
  request.budget.max_iterations = 1;
  ChordToneGeneratorConfig one_voice;
  one_voice.voice_count = 1;

  const AssistResult result =
      ChordToneGenerator(nullptr, nullptr, one_voice).generate(f.view(), request);
  CHECK(result.diagnostics.status == AssistStatus::kBudgetTruncated);
  CHECK(result.diagnostics.iterations_consumed == 1u);
  REQUIRE(result.commands.size() == 1u);
  const std::vector<EmittedNote> notes = emitted_notes(*sole_patch(result));
  REQUIRE(notes.size() == 1u);
  CHECK(notes[0].on_ppq == 480.0);
  CHECK(notes[0].off_ppq == 960.0);
  CHECK(notes[0].note == 36);
}

TEST_CASE("an injected harmony context reaches the generator's own placement rules",
          "[assist][modules]") {
  Fixture f = make_fixture();
  annotate(&f, {key_segment(0.0, 1920.0, 0, KeyMode::kMajor)},
           {chord_symbol(0.0, 960.0, 0, ChordQuality::kMajor)});

  // The module ids are what a host selects a module by, so they are part of the
  // surface rather than an implementation detail.
  ChordToneGenerator generator;
  DiatonicHarmonizer harmonizer;
  CHECK(std::string(generator.module_id()) == "sonare.builtin.chord_tone_generator");
  CHECK(std::string(harmonizer.module_id()) == "sonare.builtin.diatonic_harmonizer");

  // Injecting the harmony context explicitly gives the same voicing as letting
  // the module install its own, because the injected one reaches the judge too.
  const TimelineHarmonyContext harmony;
  ChordToneGenerator injected(&harmony, nullptr);
  CHECK(sole_patch(injected.generate(f.view(), request_with(target_params(f.clip_id))))
            ->patch()
            .add == sole_patch(generator.generate(f.view(), request_with(target_params(f.clip_id))))
                        ->patch()
                        .add);
}
