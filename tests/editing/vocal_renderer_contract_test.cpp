#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <vector>

#include "editing/vocal_edit/render_cache.h"
#include "editing/vocal_edit/renderer.h"
#include "editing/vocal_edit/session.h"
#include "support/alloc_guard.h"
#include "util/constants.h"
#include "util/numeric_validation.h"

namespace {
using namespace sonare::editing::vocal_edit;
VocalEditSession tone_session(int64_t output_length = 16000, uint32_t max_jobs = 4) {
  std::vector<float> samples(16000);
  for (size_t i = 0; i < samples.size(); ++i)
    samples[i] = static_cast<float>(0.2 * std::sin(sonare::constants::kTwoPiD * 440 * i / 16000));
  VocalAnalysisData analysis;
  analysis.grid = {0, 160, 320};
  analysis.f0_hz.assign(100, 440);
  analysis.voiced.assign(100, 1);
  analysis.algorithm_id = "host";
  analysis.algorithm_version = 1;
  VocalSessionCreateOptions options;
  options.analysis = analysis;
  options.output_length_samples = output_length;
  options.limits.max_render_jobs = max_jobs;
  return VocalEditSession::create(sonare::Audio::from_vector(std::move(samples), 16000), options);
}
double frequency(const std::vector<float>& samples, size_t begin, size_t end) {
  size_t crossings = 0;
  for (size_t i = begin + 1; i < end; ++i)
    if (samples[i - 1] <= 0 && samples[i] > 0) ++crossings;
  return static_cast<double>(crossings) * 16000 / static_cast<double>(end - begin);
}
}  // namespace

TEST_CASE("vocal duration edits preserve tone pitch", "[vocal_renderer_contract]") {
  auto session = tone_session(32000);
  const auto note = session.notes().front();
  auto edit = note.edit;
  edit.destination_length_samples = 32000;
  auto draft = session.begin_edit(0);
  draft->apply(draft->token().draft_generation, {SetNoteEditOp{note.id, edit}});
  draft->commit(0);
  auto rendered = render_snapshot(session.capture_render_snapshot(), {{0, 32000}, 1});
  REQUIRE(rendered.samples.size() == 32000);
  const double measured = frequency(rendered.samples, 8000, 24000);
  INFO("measured stretched tone Hz: " << measured);
  CHECK(measured > 435);
  CHECK(measured < 445);
}

TEST_CASE("zero to zero transitions preserve original PCM bits", "[vocal_renderer_contract]") {
  auto session = tone_session();
  const auto original = session.capture_render_snapshot();
  auto draft = session.begin_edit(0);
  draft->apply(draft->token().draft_generation, {SplitNoteOp{session.notes().front().id, 8000}});
  const auto notes = draft->notes();
  REQUIRE(notes.size() == 2);
  PitchTransition transition;
  transition.left_note_id = notes[0].id;
  transition.right_note_id = notes[1].id;
  transition.left_window_samples = 1600;
  transition.right_window_samples = 1600;
  transition.strength = 1;
  draft->apply(draft->token().draft_generation, {SetTransitionOp{transition}});
  draft->commit(0);
  auto rendered = render_snapshot(session.capture_render_snapshot(), {{0, 16000}, 2});
  CHECK(std::memcmp(rendered.samples.data(), original->data().source.data(),
                    16000 * sizeof(float)) == 0);
}

TEST_CASE("vocal render jobs require all units before finalization",
          "[vocal_c_api][vocal_renderer_contract]") {
  auto session = tone_session();
  VocalRenderJob job(session.capture_render_snapshot(), {{0, 16000}, 3});
  CHECK(job.next());
  CHECK(job.progress().state != VocalRenderJobState::kComplete);
  CHECK_THROWS(job.finalize());
  size_t calls = 1;
  while (job.progress().state != VocalRenderJobState::kComplete) {
    REQUIRE(job.next());
    REQUIRE(++calls < 100);
  }
  CHECK(calls >= 5);  // one note and four bounded assembly tiles
  REQUIRE(job.finalize().samples.size() == 16000);
  CHECK_THROWS(job.finalize());
}

TEST_CASE("vocal render job rejects recursive next callbacks",
          "[vocal_renderer_contract][vocal_reentry]") {
  auto session = tone_session();
  const auto snapshot = session.capture_render_snapshot();
  const VocalRenderRequest request{{0, 16000}, 101};
  const auto expected = render_snapshot(snapshot, request);
  VocalRenderJob job(session.capture_render_snapshot(), request);
  VocalRenderJob* active_job = &job;
  size_t callback_count = 0;
  size_t rejected_recursive_calls = 0;
  auto cancel = [&] {
    ++callback_count;
    try {
      (void)active_job->next();
    } catch (const VocalEditException& error) {
      if (error.reason() == VocalReason::kInvalidState) ++rejected_recursive_calls;
    }
    return false;
  };

  const std::vector<VocalRenderProgress> expected_progress = {
      {VocalRenderJobState::kAssembling, 1, 5}, {VocalRenderJobState::kAssembling, 2, 5},
      {VocalRenderJobState::kAssembling, 3, 5}, {VocalRenderJobState::kAssembling, 4, 5},
      {VocalRenderJobState::kComplete, 5, 5},
  };
  for (const auto& expected_step : expected_progress) {
    REQUIRE(job.next(cancel));
    CHECK(job.progress().state == expected_step.state);
    CHECK(job.progress().completed_units == expected_step.completed_units);
    CHECK(job.progress().total_units == expected_step.total_units);
  }
  REQUIRE(job.progress().state == VocalRenderJobState::kComplete);
  const auto actual = job.finalize();
  CHECK(actual.samples == expected.samples);
  CHECK(callback_count > 0);
  CHECK(rejected_recursive_calls == callback_count);
}

TEST_CASE("vocal render job rejects recursive finalize callbacks",
          "[vocal_renderer_contract][vocal_reentry]") {
  auto session = tone_session();
  const VocalRenderRequest request{{0, 16000}, 102};
  VocalRenderJob job(session.capture_render_snapshot(), request);
  while (job.progress().state != VocalRenderJobState::kComplete) REQUIRE(job.next());

  VocalRenderJob* active_job = &job;
  bool recursive_finalize_rejected = false;
  auto cancel = [&] {
    try {
      (void)active_job->finalize();
    } catch (const VocalEditException& error) {
      recursive_finalize_rejected = error.reason() == VocalReason::kInvalidState;
    }
    return false;
  };
  const auto result = job.finalize(cancel);
  CHECK(result.samples.size() == 16000);
  CHECK(recursive_finalize_rejected);
  CHECK_THROWS_AS(job.finalize(), VocalEditException);
}

TEST_CASE("vocal render finalize abort callback cancels without returning partial output",
          "[vocal_renderer_contract][vocal_reentry]") {
  auto session = tone_session();
  VocalRenderJob job(session.capture_render_snapshot(), {{0, 16000}, 104});
  while (job.progress().state != VocalRenderJobState::kComplete) REQUIRE(job.next());

  VocalReason reason = VocalReason::kNone;
  try {
    (void)job.finalize([&] {
      job.abort();
      return false;
    });
  } catch (const VocalEditException& error) {
    reason = error.reason();
  }
  CHECK(reason == VocalReason::kCancelled);
  CHECK(job.progress().state == VocalRenderJobState::kAborted);
  CHECK_THROWS_AS(job.finalize(), VocalEditException);
}

TEST_CASE("vocal render job abort from callback leaves no partial output",
          "[vocal_renderer_contract][vocal_reentry]") {
  auto session = tone_session();
  const auto note = session.notes().front();
  auto draft = session.begin_edit(0);
  draft->apply(draft->token().draft_generation, {SplitNoteOp{note.id, 8000}});
  draft->commit(0);

  VocalRenderJob job(session.capture_render_snapshot(), {{0, 16000}, 103});
  size_t probe_count = 0;
  auto cancel = [&] {
    ++probe_count;
    if (probe_count == 16) job.abort();
    return false;
  };
  for (size_t step = 0; step < 5; ++step) REQUIRE(job.next(cancel));
  CHECK_FALSE(job.next(cancel));
  CHECK(probe_count == 16);
  CHECK(job.progress().state == VocalRenderJobState::kAborted);
  CHECK(job.progress().completed_units == 5);
  CHECK_THROWS_AS(job.finalize(), VocalEditException);
}

TEST_CASE("vocal render job limits are shared and released on abort", "[vocal_renderer_contract]") {
  auto session = tone_session(16000, 1);
  auto snapshot = session.capture_render_snapshot();
  VocalRenderJob first(snapshot, {{0, 16000}, 1});
  CHECK_THROWS(VocalRenderJob(snapshot, {{0, 16000}, 2}));
  first.abort();
  CHECK_NOTHROW(VocalRenderJob(snapshot, {{0, 16000}, 3}));
}

TEST_CASE("vocal cache allocation failures preserve discoverable byte accounting",
          "[vocal_cache_allocation]") {
  size_t failures = 0;
  for (size_t prefills = 0; prefills < 96; ++prefills) {
    for (size_t fail_at = 1; fail_at <= 4; ++fail_at) {
      VocalRenderCache cache(512);
      auto artifact_for = [](size_t id) {
        auto artifact = std::make_shared<RenderArtifact>();
        artifact->key[0] = static_cast<uint8_t>(id);
        artifact->destination_range = {0, 1};
        artifact->samples = std::make_shared<const std::vector<float>>(1, 0.25f);
        return artifact;
      };
      for (size_t id = 1; id <= prefills; ++id) cache.publish(artifact_for(id));
      auto artifact = artifact_for(prefills + 1);
      bool failed = false;
      {
        sonare::test::AllocationFailureAtGuard guard(fail_at);
        try {
          cache.publish(artifact);
        } catch (const std::bad_alloc&) {
          failed = true;
        }
      }
      if (failed) ++failures;
      uint64_t discoverable = 0;
      for (size_t id = 1; id <= prefills + 1; ++id) {
        Sha256Digest key{};
        key[0] = static_cast<uint8_t>(id);
        if (auto retained = cache.find(key))
          discoverable += retained->samples->size() * sizeof(float);
      }
      INFO("prefills=" << prefills << " fail_at=" << fail_at);
      CHECK(cache.bytes() == discoverable);
      cache.publish(artifact);
      CHECK(cache.bytes() <= cache.max_bytes());
      CHECK(cache.find(artifact->key) != nullptr);
    }
  }
  CHECK(failures > 0);
}

TEST_CASE("vocal job rejects a default constructed snapshot", "[vocal_invalid_snapshot]") {
  REQUIRE_THROWS_AS(VocalRenderJob(std::make_shared<VocalRenderSnapshot>(), {{0, 1}, 0}),
                    VocalEditException);
}

TEST_CASE("vocal stretch adapter fits multiple rounded tail samples", "[vocal_stretch_rounding]") {
  constexpr size_t desired = 50000029;
  const float rate = static_cast<float>(4096.0 / desired);
  size_t projected = 0;
  REQUIRE(
      sonare::numeric::checked_projected_count(size_t{4096}, rate, size_t{100000000}, &projected));
  REQUIRE(projected == 50000027);
  const size_t missing = desired - projected;
  const std::vector<float> finite_tail = {0.25f, -0.5f, 0.75f};
  const auto fitted = detail::fit_stretched_note_output(finite_tail, finite_tail.size() + missing);
  REQUIRE(fitted.size() == 5);
  CHECK(fitted[0] == 0.25f);
  CHECK(fitted[1] == -0.5f);
  CHECK(fitted[2] == 0.75f);
  CHECK(fitted[3] == 0.75f);
  CHECK(fitted[4] == 0.75f);
}

TEST_CASE("vocal stretch rate minimizes checked projected length error",
          "[vocal_stretch_rounding]") {
  const float rate = detail::stretch_rate_for_length(4096, 5000);
  size_t projected = 0;
  REQUIRE(sonare::numeric::checked_projected_count(size_t{4096}, rate, size_t{10000}, &projected));
  CHECK(projected == 5000);
}

TEST_CASE("cached vocal note work does not allocate for unrelated notes", "[vocal_unit_work]") {
  auto allocations_for_first_note = [](size_t note_count) {
    auto session = tone_session();
    auto draft = session.begin_edit(0);
    for (size_t i = 1; i < note_count; ++i)
      draft->apply(draft->token().draft_generation,
                   {SplitNoteOp{draft->notes().back().id, static_cast<int64_t>(i * 1600)}});
    draft->commit(0);
    auto snapshot = session.capture_render_snapshot();
    (void)render_snapshot(snapshot, {{0, 16000}, 1});
    VocalRenderJob job(snapshot, {{0, 16000}, 2});
    size_t allocations = 0;
    bool advanced = false;
    {
      sonare::test::AllocationGuard guard;
      advanced = job.next();
      allocations = guard.count();
    }
    REQUIRE(advanced);
    REQUIRE(job.progress().completed_units == 1);
    return allocations;
  };
  const auto small = allocations_for_first_note(2);
  const auto large = allocations_for_first_note(10);
  INFO("cached first-note allocations: " << small << " vs " << large);
  CHECK(large == small);
}
