#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "editing/vocal_edit/renderer.h"
#include "editing/vocal_edit/session.h"
#include "util/constants.h"

namespace {
std::uint32_t bits(float value) {
  std::uint32_t result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

using namespace sonare::editing::vocal_edit;

VocalEditSession moved_session() {
  constexpr std::size_t source_count = 4096;
  VocalAnalysisData analysis;
  analysis.grid.samples_per_frame = 512.0;
  analysis.grid.frame_length_samples = 512;
  analysis.f0_hz.assign(source_count / 512, 220.0f);
  analysis.amplitude.assign(analysis.f0_hz.size(), 0.2f);
  analysis.voiced.assign(analysis.f0_hz.size(), 1);
  analysis.algorithm_id = "host";
  analysis.algorithm_version = 1;

  std::vector<float> source(source_count);
  for (std::size_t i = 0; i < source.size(); ++i) source[i] = static_cast<float>(i % 97) / 97.0f;
  VocalSessionCreateOptions options;
  options.analysis = analysis;
  options.output_length_samples = 4608;
  options.render_settings.edge_fade_ms = 0.0;
  auto session =
      VocalEditSession::create(sonare::Audio::from_vector(std::move(source), 16000), options);

  const auto note = session.notes().front();
  auto draft = session.begin_edit(0);
  auto edit = note.edit;
  edit.destination_start_sample = 256;
  edit.destination_length_samples = 4096;
  edit.gain_db = -3.0;
  draft->apply(draft->token().draft_generation, {SetNoteEditOp{note.id, edit}});
  draft->commit(0);
  return session;
}

}  // namespace

TEST_CASE("vocal render ranges equal a full render bit for bit", "[vocal_render_range]") {
  auto session = moved_session();
  const auto snapshot = session.capture_render_snapshot();
  const auto full = render_snapshot(snapshot, {{0, 4608}, 10});
  const std::vector<SampleRange> ranges = {{0, 1024}, {1024, 3072}, {3072, 4096}, {4096, 4608}};

  std::vector<float> tiled;
  for (const auto range : ranges) {
    const auto tile = render_snapshot(snapshot, {range, 11});
    tiled.insert(tiled.end(), tile.samples.begin(), tile.samples.end());
  }
  REQUIRE(tiled.size() == full.samples.size());
  for (std::size_t i = 0; i < full.samples.size(); ++i) {
    CHECK(bits(tiled[i]) == bits(full.samples[i]));
  }
}

namespace {

constexpr int kNeighbourRate = 16000;
constexpr std::size_t kNeighbourFrames = 12;
constexpr std::size_t kNeighbourHop = 512;

// Three pitched regions so segmentation yields three adjacent notes A, B, C.
VocalEditSession three_note_session() {
  const std::array<float, 3> region_hz = {220.0f, 330.0f, 262.0f};
  VocalAnalysisData analysis;
  analysis.grid.samples_per_frame = static_cast<double>(kNeighbourHop);
  analysis.grid.frame_length_samples = static_cast<uint32_t>(kNeighbourHop);
  for (std::size_t frame = 0; frame < kNeighbourFrames; ++frame) {
    analysis.f0_hz.push_back(region_hz[frame / 4]);
  }
  analysis.voiced.assign(kNeighbourFrames, 1);
  analysis.algorithm_id = "host";
  analysis.algorithm_version = 1;
  std::vector<float> source(kNeighbourFrames * kNeighbourHop);
  double phase = 0.0;
  for (std::size_t i = 0; i < source.size(); ++i) {
    phase += sonare::constants::kTwoPiD * region_hz[i / (4 * kNeighbourHop)] / kNeighbourRate;
    source[i] = static_cast<float>(0.3 * std::sin(phase));
  }
  VocalSessionCreateOptions options;
  options.analysis = analysis;
  options.render_settings.edge_fade_ms = 0.0;
  return VocalEditSession::create(sonare::Audio::from_vector(std::move(source), kNeighbourRate),
                                  options);
}

std::vector<float> render_all(const VocalEditSession& session) {
  return render_snapshot(session.capture_render_snapshot(),
                         {{0, session.output_length_samples()}, 1})
      .samples;
}

bool inside(const std::vector<SampleRange>& ranges, int64_t sample) {
  for (const auto range : ranges) {
    if (sample >= range.start && sample < range.end) return true;
  }
  return false;
}

// Every sample that moved beyond float noise must be reported dirty; returns the moved count.
std::size_t require_changes_inside(const std::vector<float>& before,
                                   const std::vector<float>& after,
                                   const std::vector<SampleRange>& dirty, SampleRange probe,
                                   std::size_t* probe_hits) {
  REQUIRE(before.size() == after.size());
  constexpr float kTolerance = 1.0e-6f;
  std::size_t moved = 0;
  std::size_t missed = 0;
  for (std::size_t i = 0; i < before.size(); ++i) {
    if (std::fabs(before[i] - after[i]) <= kTolerance) continue;
    ++moved;
    const auto sample = static_cast<int64_t>(i);
    if (!inside(dirty, sample)) ++missed;
    if (sample >= probe.start && sample < probe.end) ++*probe_hits;
  }
  CHECK(missed == 0);
  return moved;
}

}  // namespace

TEST_CASE("vocal dirty ranges cover the unchanged neighbour bent by a transition",
          "[vocal_render_range][vocal_dirty]") {
  auto session = three_note_session();
  const auto notes = session.notes();
  REQUIRE(notes.size() == 3);
  for (std::size_t i = 0; i + 1 < notes.size(); ++i) {
    REQUIRE(notes[i].edit.destination_start_sample + notes[i].edit.destination_length_samples ==
            notes[i + 1].edit.destination_start_sample);
  }
  const auto& a = notes[0];
  const auto& b = notes[1];
  const auto& c = notes[2];
  const SampleRange b_range{b.edit.destination_start_sample,
                            b.edit.destination_start_sample + b.edit.destination_length_samples};
  const SampleRange c_range{c.edit.destination_start_sample,
                            c.edit.destination_start_sample + c.edit.destination_length_samples};

  // A bends into unedited B; B–C exists so a too-wide answer would also be visible.
  PitchTransition ab{a.id, b.id, 512, 1024, 1.0};
  PitchTransition bc{b.id, c.id, 512, 512, 1.0};
  {
    auto draft = session.begin_edit(session.token().committed_revision);
    draft->apply(draft->token().draft_generation, {SetTransitionOp{ab}, SetTransitionOp{bc}});
    draft->commit(session.token().committed_revision);
  }
  const auto before = render_all(session);

  auto edit = a.edit;
  edit.pitch.transpose_semitones = 3.0;
  auto draft = session.begin_edit(session.token().committed_revision);
  const auto applied = draft->apply(draft->token().draft_generation, {SetNoteEditOp{a.id, edit}});
  const auto committed = draft->commit(session.token().committed_revision);
  CHECK(applied.dirty_ranges.size() == committed.dirty_ranges.size());
  const auto after = render_all(session);

  std::size_t b_hits = 0;
  CHECK(require_changes_inside(before, after, committed.dirty_ranges, b_range, &b_hits) > 0);
  CHECK(b_hits > 0);
  std::size_t c_hits = 0;
  require_changes_inside(before, after, committed.dirty_ranges, c_range, &c_hits);
  CHECK(c_hits == 0);
  CHECK_FALSE(inside(committed.dirty_ranges, c_range.start + c_range.length() / 2));

  const auto undone = session.undo(session.token().committed_revision);
  const auto after_undo = render_all(session);
  b_hits = 0;
  CHECK(require_changes_inside(after, after_undo, undone.dirty_ranges, b_range, &b_hits) > 0);
  CHECK(b_hits > 0);

  const auto redone = session.redo(session.token().committed_revision);
  const auto after_redo = render_all(session);
  b_hits = 0;
  CHECK(require_changes_inside(after_undo, after_redo, redone.dirty_ranges, b_range, &b_hits) > 0);
  CHECK(b_hits > 0);
}

TEST_CASE("vocal commit of a never-applied draft keeps history and redo",
          "[vocal_render_range][vocal_dirty]") {
  auto session = three_note_session();
  const auto note = session.notes().front();
  auto edit = note.edit;
  edit.gain_db = -6.0;
  {
    auto draft = session.begin_edit(session.token().committed_revision);
    draft->apply(draft->token().draft_generation, {SetNoteEditOp{note.id, edit}});
    draft->commit(session.token().committed_revision);
  }
  session.undo(session.token().committed_revision);
  REQUIRE(session.can_redo());
  const auto revision = session.token().committed_revision;

  auto draft = session.begin_edit(revision);
  const auto result = draft->commit(revision);
  CHECK(result.dirty_ranges.empty());
  CHECK(result.token.committed_revision == revision);
  CHECK(session.token().committed_revision == revision);
  CHECK_FALSE(draft->active());
  REQUIRE(session.can_redo());
  session.redo(revision);
  CHECK(session.notes().front().edit.gain_db == -6.0);
}
