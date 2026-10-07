#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "editing/vocal_edit/renderer.h"
#include "editing/vocal_edit/session.h"

using Catch::Matchers::WithinAbs;

namespace {
std::uint32_t bits(float value) {
  std::uint32_t result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

using sonare::Audio;
using namespace sonare::editing::vocal_edit;

VocalAnalysisData make_analysis(std::size_t frame_count, int sample_rate) {
  VocalAnalysisData analysis;
  analysis.grid.frame_origin_sample = 0.0;
  analysis.grid.samples_per_frame = 512.0;
  analysis.grid.frame_length_samples = 512;
  analysis.f0_hz.assign(frame_count, 220.0f);
  analysis.amplitude.assign(frame_count, 0.25f);
  analysis.voiced.assign(frame_count, 1);
  analysis.algorithm_id = "host";
  analysis.algorithm_version = 1;
  static_cast<void>(sample_rate);
  return analysis;
}

Audio make_source(std::size_t count, int sample_rate) {
  std::vector<float> samples(count);
  for (std::size_t i = 0; i < count; ++i) {
    samples[i] = 0.25f * std::sin(static_cast<float>(i) * 0.03125f);
  }
  samples[17] = -0.0f;
  return Audio::from_vector(std::move(samples), sample_rate);
}

VocalEditSession make_session(std::size_t source_count, int64_t output_count = 0) {
  constexpr int sample_rate = 16000;
  VocalSessionCreateOptions options;
  options.analysis = make_analysis(source_count / 512, sample_rate);
  options.output_length_samples = output_count;
  options.render_settings.edge_fade_ms = 0.0;
  return VocalEditSession::create(make_source(source_count, sample_rate), options);
}

void require_same_bits(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  REQUIRE(lhs.size() == rhs.size());
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    CHECK(bits(lhs[i]) == bits(rhs[i]));
  }
}

}  // namespace

TEST_CASE("vocal renderer preserves identity source bits and warms complete-note cache",
          "[vocal_renderer]") {
  auto session = make_session(4096);
  const auto snapshot = session.capture_render_snapshot();
  const auto first = render_snapshot(snapshot, {{0, 4096}, 1});
  const auto second = render_snapshot(snapshot, {{0, 4096}, 2});

  std::vector<float> source(snapshot->data().source.begin(), snapshot->data().source.end());
  require_same_bits(first.samples, source);
  require_same_bits(second.samples, source);
  CHECK(second.cache_hit_units >= 1);
}

TEST_CASE("vocal render job cancellation never publishes a partial result", "[vocal_renderer]") {
  auto session = make_session(4096);
  const auto snapshot = session.capture_render_snapshot();
  VocalRenderJob job(snapshot, {{0, 4096}, 3});
  CHECK_FALSE(job.next([] { return true; }));
  CHECK(job.progress().state == VocalRenderJobState::kAborted);
  CHECK_THROWS(render_snapshot(snapshot, {{0, 4096}, 4}, [] { return true; }));
}

TEST_CASE("vocal renderer applies pitch, gain and mute on complete note units",
          "[vocal_renderer]") {
  auto session = make_session(4096);
  const auto note = session.notes().front();
  auto draft = session.begin_edit(0);
  auto edit = note.edit;
  edit.pitch.target.mode = PitchTargetMode::kCenter;
  edit.pitch.target.center_midi = 64.0;
  edit.pitch.amount = 1.0;
  edit.gain_db = -6.0;
  draft->apply(draft->token().draft_generation, {SetNoteEditOp{note.id, edit}});
  draft->commit(0);
  const auto snapshot = session.capture_render_snapshot();
  const auto rendered = render_snapshot(snapshot, {{0, 4096}, 5});

  REQUIRE(rendered.samples.size() == 4096);
  bool changed = false;
  for (std::size_t i = 0; i < rendered.samples.size(); ++i) {
    changed = changed || bits(rendered.samples[i]) != bits(snapshot->data().source[i]);
  }
  CHECK(changed);
}

TEST_CASE("vocal renderer reproduces the source across an in-place edit seam", "[vocal_renderer]") {
  constexpr int sample_rate = 16000;
  constexpr std::size_t kCount = 4096;
  VocalSessionCreateOptions options;
  options.analysis = make_analysis(kCount / 512, sample_rate);
  auto session = VocalEditSession::create(make_source(kCount, sample_rate), options);
  const auto note = session.notes().front();
  auto draft = session.begin_edit(0);
  auto edit = note.edit;
  edit.amplitude_envelope = {1.0f};  // non-identity, yet the segment equals the source
  draft->apply(draft->token().draft_generation, {SetNoteEditOp{note.id, edit}});
  draft->commit(0);
  const auto snapshot = session.capture_render_snapshot();
  const auto rendered = render_snapshot(snapshot, {{0, static_cast<int64_t>(kCount)}, 6});

  REQUIRE(rendered.samples.size() == kCount);
  for (std::size_t i = 0; i < kCount; ++i) {
    CHECK_THAT(rendered.samples[i], WithinAbs(snapshot->data().source[i], 1.0e-5f));
  }
}
