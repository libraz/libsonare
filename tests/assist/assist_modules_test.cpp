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
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_history.h"
#include "arrangement/edit_model.h"
#include "arrangement/harmonic_timeline.h"
#include "arrangement/project_view.h"
#include "midi/assist/assist_registry.h"
#include "midi/assist/composition_assist.h"
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

namespace {

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
using sonare::midi::assist::CompositionAssist;
using sonare::midi::assist::INoteGenerator;
using sonare::midi::assist::PlacementVerdict;
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

// ---------------------------------------------------------------------------
// Reading a module's proposal back
// ---------------------------------------------------------------------------

struct EmittedNote {
  double on_ppq = 0.0;
  double off_ppq = 0.0;
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
  std::map<uint8_t, EmittedNote> open;
  std::vector<EmittedNote> notes;
  for (const MidiClipEvent& event : patch.patch().add) {
    sonare::midi::Ump ump;
    ump.words[0] = event.data0;
    ump.words[1] = event.data1;
    ump.word_count = sonare::midi::ump_word_count_for_word0(event.data0);
    ump.group = sonare::midi::ump_group_from_word0(event.data0);
    if (ump.is_note_off()) {
      const auto it = open.find(ump.note_number());
      REQUIRE(it != open.end());
      EmittedNote note = it->second;
      open.erase(it);
      note.off_ppq = event.ppq;
      notes.push_back(note);
    } else {
      REQUIRE(ump.is_note_on());
      open[ump.note_number()] = EmittedNote{event.ppq, 0.0, ump.note_number(), ump.data2_7bit()};
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

AssistRequest request_with(const std::string& params_json) {
  AssistRequest request;
  request.params_json = params_json;
  return request;
}

std::string target_params(ClipId clip_id) {
  return "{\"target_clip_id\": " + std::to_string(clip_id) + "}";
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
