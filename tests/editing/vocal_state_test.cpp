#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <locale>
#include <stdexcept>
#include <utility>
#include <vector>

#include "editing/vocal_edit/session.h"
#include "util/sha256.h"

using sonare::Audio;
using sonare::editing::vocal_edit::AnalysisGrid;
using sonare::editing::vocal_edit::measure_vocal_note;
using sonare::editing::vocal_edit::MergeMode;
using sonare::editing::vocal_edit::MergeNotesOp;
using sonare::editing::vocal_edit::Operation;
using sonare::editing::vocal_edit::PitchTargetMode;
using sonare::editing::vocal_edit::PitchTransition;
using sonare::editing::vocal_edit::ResetNotesOp;
using sonare::editing::vocal_edit::SampleRange;
using sonare::editing::vocal_edit::SetNoteEditOp;
using sonare::editing::vocal_edit::SetNoteSourceSpanOp;
using sonare::editing::vocal_edit::SetTransitionOp;
using sonare::editing::vocal_edit::SplitNoteOp;
using sonare::editing::vocal_edit::validate_analysis;
using sonare::editing::vocal_edit::VocalAnalysisData;
using sonare::editing::vocal_edit::VocalEditException;
using sonare::editing::vocal_edit::VocalEditSession;
using sonare::editing::vocal_edit::VocalNoteEdit;
using sonare::editing::vocal_edit::VocalSessionCreateOptions;

namespace {

VocalAnalysisData make_analysis() {
  VocalAnalysisData result;
  result.grid = AnalysisGrid{0.0, 240.0, 512};
  result.f0_hz.assign(12, 261.62555f);
  result.amplitude.assign(12, 0.25f);
  result.voiced.assign(12, 1);
  result.algorithm_id = "host";
  result.algorithm_version = 1;
  return result;
}

Audio make_audio() { return Audio::from_vector(std::vector<float>(2880, 0.0f), 48000); }

VocalSessionCreateOptions options() {
  VocalSessionCreateOptions result;
  result.analysis = make_analysis();
  result.output_length_samples = 2880;
  return result;
}

}  // namespace

TEST_CASE("vocal restore hash diagnostics are independent of the global locale",
          "[vocal_state][vocal_hash_locale]") {
  struct GroupedHex : std::numpunct<char> {
    char do_thousands_sep() const override { return ':'; }
    std::string do_grouping() const override { return "\1"; }
  };
  struct RestoreLocale {
    std::locale previous = std::locale();
    ~RestoreLocale() { std::locale::global(previous); }
  } restore_locale;
  auto session = VocalEditSession::create(make_audio(), options());
  const auto bytes = session.export_state();
  const std::vector<float> original(2880, 0.0f);
  auto changed = original;
  changed[10] = 0.25f;
  const auto expected = sonare::util::sha256_hex(reinterpret_cast<const uint8_t*>(original.data()),
                                                 original.size() * sizeof(float));
  const auto actual = sonare::util::sha256_hex(reinterpret_cast<const uint8_t*>(changed.data()),
                                               changed.size() * sizeof(float));
  std::locale::global(std::locale(restore_locale.previous, new GroupedHex));
  try {
    (void)VocalEditSession::restore(Audio::from_vector(changed, 48000), bytes);
    FAIL("changed source must be rejected");
  } catch (const VocalEditException& error) {
    CHECK(error.expected() == expected);
    CHECK(error.actual() == actual);
  }
}

TEST_CASE("vocal session copies source and exposes immutable analysis", "[vocal_state]") {
  auto samples = std::vector<float>(2880, 0.0f);
  samples[10] = 0.25f;
  auto session = VocalEditSession::create(Audio::from_vector(std::move(samples), 48000), options());

  REQUIRE(session.notes().size() == 1);
  CHECK(session.analysis().f0_hz.size() == 12);
  CHECK(session.token().committed_revision == 0);
  CHECK(session.token().draft_id == 0);
  CHECK(session.source_descriptor().sample_count == 2880);
}

TEST_CASE("draft batches operations and commit creates one revision", "[vocal_state]") {
  auto session = VocalEditSession::create(make_audio(), options());
  auto draft = session.begin_edit(0);
  auto edit = VocalNoteEdit::identity_for({0, 2880});
  edit.gain_db = -3.0;
  edit.pitch.target.mode = PitchTargetMode::kCenter;
  edit.pitch.target.center_midi = 61.0;
  const auto id = session.notes().front().id;

  auto result =
      draft->apply(draft->token().draft_generation, {SetNoteEditOp{id, edit}, ResetNotesOp{{id}}});
  CHECK(result.token.draft_generation == 2);
  CHECK(draft->notes().front().edit.is_identity({0, 2880}));
  draft->cancel();
  CHECK(session.token().committed_revision == 0);

  auto draft2 = session.begin_edit(0);
  draft2->apply(draft2->token().draft_generation, {SetNoteEditOp{id, edit}});
  const auto committed = draft2->commit(0);
  CHECK(committed.token.committed_revision == 1);
  CHECK(session.notes().front().edit.gain_db == -3.0);
}

TEST_CASE("failed batch rolls back state and does not consume an ID", "[vocal_state]") {
  auto session = VocalEditSession::create(make_audio(), options());
  const auto original_id = session.notes().front().id;
  auto draft = session.begin_edit(0);
  const auto generation = draft->token().draft_generation;
  CHECK_THROWS(
      draft->apply(generation, {SplitNoteOp{original_id, 1200}, SplitNoteOp{original_id, 0}}));
  CHECK(draft->notes().size() == 1);
  draft->cancel();

  auto next = session.begin_edit(0);
  const auto result = next->apply(next->token().draft_generation, {SplitNoteOp{original_id, 1200}});
  REQUIRE(result.id_changes.size() == 1);
  CHECK(result.id_changes.front().new_ids.front() == 2);
}

TEST_CASE("split retires the old ID, preserves curve endpoints, and undo keeps revision monotonic",
          "[vocal_state]") {
  auto session = VocalEditSession::create(make_audio(), options());
  const auto original_id = session.notes().front().id;
  auto draft = session.begin_edit(0);
  auto edit = draft->notes().front().edit;
  edit.pitch.target.mode = PitchTargetMode::kCurve;
  edit.pitch.target.points = {{0.0, 60.0}, {1440.0, 62.0}, {2880.0, 64.0}};
  draft->apply(draft->token().draft_generation, {SetNoteEditOp{original_id, edit}});
  draft->apply(draft->token().draft_generation, {SplitNoteOp{original_id, 1440}});
  const auto committed = draft->commit(0);
  CHECK(committed.token.committed_revision == 1);
  REQUIRE(session.notes().size() == 2);
  CHECK(session.notes()[0].id != original_id);
  CHECK(session.notes()[1].id != original_id);
  CHECK(session.notes()[0].edit.pitch.target.points.back().source_sample == 1440.0);
  CHECK(session.notes()[1].edit.pitch.target.points.front().source_sample == 1440.0);

  CHECK(session.undo(1).token.committed_revision == 2);
  CHECK(session.notes().size() == 1);
  CHECK(session.notes().front().id == original_id);
  CHECK(session.redo(2).token.committed_revision == 3);
  CHECK(session.notes().size() == 2);
}

TEST_CASE("merge preserve edits rejects silent loss and reset is explicit", "[vocal_state]") {
  auto session = VocalEditSession::create(make_audio(), options());
  const auto original_id = session.notes().front().id;
  auto draft = session.begin_edit(0);
  draft->apply(draft->token().draft_generation, {SplitNoteOp{original_id, 1440}});
  const auto split = draft->notes();
  auto left = split[0].edit;
  left.gain_db = -3.0;
  draft->apply(draft->token().draft_generation, {SetNoteEditOp{split[0].id, left}});
  CHECK_THROWS(draft->apply(draft->token().draft_generation,
                            {MergeNotesOp{{split[0].id, split[1].id}, MergeMode::kPreserveEdits}}));
  draft->apply(draft->token().draft_generation,
               {MergeNotesOp{{split[0].id, split[1].id}, MergeMode::kResetEdits}});
  REQUIRE(draft->notes().size() == 1);
  CHECK(draft->notes().front().edit.is_identity(draft->notes().front().source_range));
}

TEST_CASE("merge preserve edits canonicalizes different pitch controls into one curve",
          "[vocal_state]") {
  auto session = VocalEditSession::create(make_audio(), options());
  const auto original_id = session.notes().front().id;
  auto draft = session.begin_edit(0);
  draft->apply(draft->token().draft_generation, {SplitNoteOp{original_id, 1440}});
  const auto split = draft->notes();
  REQUIRE(split.size() == 2);

  auto left_edit = split[0].edit;
  left_edit.pitch.target.mode = PitchTargetMode::kCenter;
  left_edit.pitch.target.center_midi = 62.0;
  left_edit.pitch.amount = 1.0;
  left_edit.pitch.speed_ms = 0.0;
  auto right_edit = split[1].edit;
  right_edit.pitch.target.mode = PitchTargetMode::kCenter;
  right_edit.pitch.target.center_midi = 64.0;
  right_edit.pitch.amount = 1.0;
  right_edit.pitch.speed_ms = 20.0;
  draft->apply(draft->token().draft_generation,
               {SetNoteEditOp{split[0].id, left_edit}, SetNoteEditOp{split[1].id, right_edit}});
  const auto before = draft->evaluate_pitch({split[0].id, split[1].id});
  REQUIRE(before.size() == 2);

  // A preserve merge must retain the effective pitch trajectory even when
  // the two source notes used different target/speed controls.
  draft->apply(draft->token().draft_generation,
               {MergeNotesOp{{split[0].id, split[1].id}, MergeMode::kPreserveEdits}});
  const auto merged = draft->notes();
  REQUIRE(merged.size() == 1);
  const auto& pitch = merged.front().edit.pitch;
  CHECK(pitch.target.mode == PitchTargetMode::kCurve);
  CHECK(pitch.amount == 1.0);
  CHECK(pitch.speed_ms == 0.0);
  CHECK(pitch.transpose_semitones == 0.0);
  CHECK(pitch.drift_scale == 1.0);
  CHECK(pitch.vibrato_scale == 1.0);
  CHECK(pitch.max_correction_semitones > 0.0);

  const auto after = draft->evaluate_pitch({merged.front().id});
  REQUIRE(after.size() == 1);
  REQUIRE(after.front().points.size() == before[0].points.size() + before[1].points.size());
  for (size_t i = 0; i < before[0].points.size(); ++i) {
    CHECK(after.front().points[i].effective_midi ==
          Catch::Approx(before[0].points[i].effective_midi).margin(1.0e-6));
  }
  for (size_t i = 0; i < before[1].points.size(); ++i) {
    CHECK(after.front().points[before[0].points.size() + i].effective_midi ==
          Catch::Approx(before[1].points[i].effective_midi).margin(1.0e-6));
  }
}

TEST_CASE("note MIDI measurements use the fixed A440 reference", "[vocal_state]") {
  auto track = make_analysis();
  track.settings.reference_hz = 220.0;
  track.f0_hz.assign(track.f0_hz.size(), 440.0f);
  const auto measured = measure_vocal_note(make_audio(), track, 1, {0, 2880});
  CHECK(measured.centre_midi == Catch::Approx(69.0).margin(1.0e-9));
  CHECK(measured.median_hz == Catch::Approx(440.0).margin(1.0e-6));
}

TEST_CASE("revision and generation conflicts leave committed state untouched", "[vocal_state]") {
  auto session = VocalEditSession::create(make_audio(), options());
  CHECK_THROWS(session.begin_edit(1));
  auto draft = session.begin_edit(0);
  CHECK_THROWS(draft->apply(0, {}));
  CHECK_THROWS(draft->commit(1));
  draft->cancel();
  CHECK(session.token().committed_revision == 0);
}

TEST_CASE("source-span expansion requires and preserves a companion curve edit", "[vocal_state]") {
  auto session = VocalEditSession::create(make_audio(), options());
  const auto id = session.notes().front().id;
  auto draft = session.begin_edit(0);
  auto curve = draft->notes().front().edit;
  curve.pitch.target.mode = PitchTargetMode::kCurve;
  curve.pitch.target.points = {{0.0, 60.0}, {2880.0, 64.0}};
  draft->apply(draft->token().draft_generation, {SetNoteEditOp{id, curve}});

  draft->apply(draft->token().draft_generation,
               {SetNoteSourceSpanOp{id, {480, 2400}, 480, 1920},
                SetNoteEditOp{id, [&] {
                                auto cropped = curve;
                                cropped.destination_start_sample = 480;
                                cropped.destination_length_samples = 1920;
                                cropped.pitch.target.points = {{480.0, 60.6666667},
                                                               {2400.0, 63.3333333}};
                                return cropped;
                              }()}});
  CHECK(draft->notes().front().edit.pitch.target.points.front().source_sample == 480.0);

  CHECK_THROWS(
      draft->apply(draft->token().draft_generation, {SetNoteSourceSpanOp{id, {0, 2880}, 0, 2880}}));
  auto expanded = curve;
  expanded.pitch.target.points = {{0.0, 60.0}, {2880.0, 64.0}};
  draft->apply(draft->token().draft_generation,
               {SetNoteSourceSpanOp{id, {0, 2880}, 0, 2880}, SetNoteEditOp{id, expanded}});
  CHECK(draft->notes().front().edit.pitch.target.points.front().source_sample == 0.0);
}

TEST_CASE("source-span expansion rejects a gain-only companion for a pitch curve",
          "[vocal_state]") {
  auto session = VocalEditSession::create(make_audio(), options());
  const auto id = session.notes().front().id;
  auto draft = session.begin_edit(0);
  auto curve = draft->notes().front().edit;
  curve.pitch.target.mode = PitchTargetMode::kCurve;
  curve.pitch.target.points = {{0.0, 60.0}, {2880.0, 64.0}};
  draft->apply(draft->token().draft_generation, {SetNoteEditOp{id, curve}});

  auto cropped = curve;
  cropped.destination_start_sample = 480;
  cropped.destination_length_samples = 1920;
  cropped.pitch.target.points = {{480.0, 60.6666667}, {2400.0, 63.3333333}};
  draft->apply(draft->token().draft_generation,
               {SetNoteSourceSpanOp{id, {480, 2400}, 480, 1920}, SetNoteEditOp{id, cropped}});

  auto gain_only = VocalNoteEdit::identity_for({0, 2880});
  gain_only.gain_db = -3.0;
  CHECK_THROWS(
      draft->apply(draft->token().draft_generation,
                   {SetNoteSourceSpanOp{id, {0, 2880}, 0, 2880}, SetNoteEditOp{id, gain_only}}));
  // The same invalid batch must remain invalid when the edit is staged first;
  // validation is against the batch's final candidate, not visitor order.
  CHECK_THROWS(
      draft->apply(draft->token().draft_generation,
                   {SetNoteEditOp{id, gain_only}, SetNoteSourceSpanOp{id, {0, 2880}, 0, 2880}}));
}

TEST_CASE("splitting a note rejects implicit transition window shrinkage", "[vocal_state]") {
  auto session = VocalEditSession::create(make_audio(), options());
  const auto original_id = session.notes().front().id;
  auto draft = session.begin_edit(0);
  draft->apply(draft->token().draft_generation, {SplitNoteOp{original_id, 1440}});
  const auto split = draft->notes();
  REQUIRE(split.size() == 2);
  draft->apply(draft->token().draft_generation,
               {SetTransitionOp{PitchTransition{split[0].id, split[1].id, 1200, 1200, 1.0}}});
  CHECK_THROWS(draft->apply(draft->token().draft_generation, {SplitNoteOp{split[0].id, 720}}));
}

TEST_CASE("analysis rejects huge frame origins before synthesizing amplitude", "[vocal_state]") {
  for (const double origin :
       {std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()}) {
    auto track = make_analysis();
    track.grid.frame_origin_sample = origin;
    track.amplitude.clear();
    CHECK_THROWS_AS(validate_analysis(make_audio(), track), VocalEditException);
  }
}

TEST_CASE("zero history byte budget keeps the current state but drops undo retention",
          "[vocal_state]") {
  auto constrained = options();
  constrained.limits.max_history_bytes = 0;
  auto session = VocalEditSession::create(make_audio(), constrained);
  const auto id = session.notes().front().id;
  auto draft = session.begin_edit(0);
  auto edit = draft->notes().front().edit;
  edit.gain_db = -3.0;
  draft->apply(draft->token().draft_generation, {SetNoteEditOp{id, edit}});
  draft->commit(0);
  CHECK_FALSE(session.can_undo());
  CHECK(session.notes().front().edit.gain_db == -3.0);
}

TEST_CASE("render snapshots retain immutable state after session destruction", "[vocal_state]") {
  std::shared_ptr<const sonare::editing::vocal_edit::VocalRenderSnapshot> snapshot;
  {
    auto session = VocalEditSession::create(make_audio(), options());
    snapshot = session.capture_render_snapshot();
  }
  REQUIRE(snapshot);
  CHECK(snapshot->valid());
  CHECK(snapshot->data().source.size() == 2880);
  CHECK(snapshot->data().state->notes.size() == 1);
}
