#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "editing/vocal_edit/renderer.h"
#include "editing/vocal_edit/session.h"
#include "support/alloc_guard.h"
#include "util/constants.h"

namespace {
using namespace sonare::editing::vocal_edit;

constexpr std::size_t kAssemblyTileSamples = 4096;
constexpr std::size_t kSourceSamples = 3 * kAssemblyTileSamples;
constexpr int64_t kOutputSamples = static_cast<int64_t>(kSourceSamples + 257);
constexpr int kSampleRate = 32000;

std::uint32_t bits(float value) {
  std::uint32_t result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

VocalEditSession make_session() {
  VocalAnalysisData analysis;
  analysis.grid = {0.0, 512.0, 512};
  analysis.f0_hz.assign(kSourceSamples / 512, 220.0f);
  analysis.amplitude.assign(analysis.f0_hz.size(), 0.25f);
  analysis.voiced.assign(analysis.f0_hz.size(), 1);
  analysis.algorithm_id = "host";
  analysis.algorithm_version = 1;

  std::vector<float> samples(kSourceSamples);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double phase = sonare::constants::kTwoPiD * 220.0 * static_cast<double>(i) /
                         static_cast<double>(kSampleRate);
    samples[i] = static_cast<float>(0.25 * std::sin(phase));
  }
  samples[17] = -0.0f;

  VocalSessionCreateOptions options;
  options.analysis = std::move(analysis);
  options.output_length_samples = kOutputSamples;
  options.render_settings.edge_fade_ms = 0.0;
  return VocalEditSession::create(sonare::Audio::from_vector(std::move(samples), kSampleRate),
                                  options);
}

std::size_t allocations_for_next(VocalRenderJob& job) {
  bool advanced = false;
  std::size_t allocations = 0;
  {
    sonare::test::AllocationGuard guard;
    advanced = job.next();
    allocations = guard.count();
  }
  REQUIRE(advanced);
  return allocations;
}

}  // namespace

TEST_CASE("vocal assembly reuses full tiles and keeps identity extension exact",
          "[vocal_assembly_alloc]") {
  auto session = make_session();
  const auto snapshot = session.capture_render_snapshot();
  VocalRenderJob job(snapshot, {{0, kOutputSamples}, 1});

  // Render the note and reserve the result before measuring assembly work.
  REQUIRE(job.next());
  REQUIRE(job.progress().state == VocalRenderJobState::kAssembling);
  REQUIRE(job.progress().total_units == 5);

  const auto first_full_tile_allocations = allocations_for_next(job);
  INFO("first full assembly tile allocations: " << first_full_tile_allocations);
  CHECK(first_full_tile_allocations > 0);
  CHECK(allocations_for_next(job) == 0);
  CHECK(allocations_for_next(job) == 0);

  // The fourth tile is shorter; its final cache publication is outside the guard.
  REQUIRE(job.next());
  REQUIRE(job.progress().state == VocalRenderJobState::kComplete);
  const auto rendered = job.finalize();
  REQUIRE(rendered.samples.size() == static_cast<std::size_t>(kOutputSamples));
  for (std::size_t i = 0; i < kSourceSamples; ++i) {
    CHECK(bits(rendered.samples[i]) == bits(snapshot->data().source[i]));
  }
  for (std::size_t i = kSourceSamples; i < rendered.samples.size(); ++i) {
    CHECK(bits(rendered.samples[i]) == bits(0.0f));
  }
}

TEST_CASE("vocal assembly preserves moved and gained range crops", "[vocal_assembly_alloc]") {
  auto session = make_session();
  const auto note = session.notes().front();
  auto edit = note.edit;
  edit.destination_start_sample = 257;
  edit.gain_db = -3.0;
  auto draft = session.begin_edit(0);
  draft->apply(draft->token().draft_generation, {SetNoteEditOp{note.id, edit}});
  draft->commit(0);

  const auto snapshot = session.capture_render_snapshot();
  const auto full = render_snapshot(snapshot, {{0, kOutputSamples}, 2});
  const SampleRange crop_range{1024, kOutputSamples - 123};
  const auto crop = render_snapshot(snapshot, {crop_range, 3});
  REQUIRE(full.samples.size() == static_cast<std::size_t>(kOutputSamples));
  REQUIRE(crop.samples.size() == static_cast<std::size_t>(crop_range.length()));

  std::size_t changed_samples = 0;
  for (std::size_t i = 0; i < full.samples.size(); ++i) {
    const float original = i < snapshot->data().source.size() ? snapshot->data().source[i] : 0.0f;
    if (bits(full.samples[i]) != bits(original)) ++changed_samples;
  }
  CHECK(changed_samples > 0);
  for (std::size_t i = 0; i < crop.samples.size(); ++i) {
    const auto full_index = static_cast<std::size_t>(crop_range.start) + i;
    CHECK(bits(crop.samples[i]) == bits(full.samples[full_index]));
  }
}
