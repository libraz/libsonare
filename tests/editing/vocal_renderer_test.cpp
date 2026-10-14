#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "editing/vocal_edit/render_cache.h"
#include "editing/vocal_edit/renderer.h"
#include "editing/vocal_edit/session.h"
#include "support/alloc_guard.h"

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

namespace {

std::shared_ptr<const RenderArtifact> cache_artifact(std::uint8_t id, std::size_t samples) {
  auto artifact = std::make_shared<RenderArtifact>();
  artifact->key[0] = id;
  artifact->note_id = id;
  artifact->destination_range = {0, static_cast<int64_t>(samples)};
  artifact->samples = std::make_shared<const std::vector<float>>(samples, 0.5f);
  return artifact;
}

}  // namespace

TEST_CASE("vocal render cache renews a republished artifact in place",
          "[vocal_renderer][vocal_cache]") {
  constexpr std::size_t kSamples = 100;
  constexpr std::uint64_t kBudget = 3 * kSamples * sizeof(float);
  VocalRenderCache cache(kBudget);
  const auto a = cache_artifact(1, kSamples);
  const auto b = cache_artifact(2, kSamples);
  const auto c = cache_artifact(3, kSamples);
  cache.publish(a);
  cache.publish(b);
  cache.publish(c);
  REQUIRE(cache.bytes() == kBudget);

  // Republishing what is already resident takes no new storage.
  const auto again = cache_artifact(1, kSamples);
  {
    sonare::test::AllocationGuard guard;
    cache.publish(again);
    CHECK(guard.count() == 0);
  }
  CHECK(cache.bytes() == kBudget);
  CHECK(cache.find(a->key) == again);

  // The renewed entry is now the newest, so the next insertion evicts the oldest remaining one.
  cache.publish(cache_artifact(4, kSamples));
  CHECK(cache.find(b->key) == nullptr);
  CHECK(cache.find(a->key) != nullptr);
  CHECK(cache.find(c->key) != nullptr);
  CHECK(cache.bytes() <= kBudget);
}

namespace {

// One voiced note of 4096 samples followed by `tail_frames` silent, unvoiced frames.
VocalEditSession session_with_silent_tail(std::size_t tail_frames) {
  constexpr int sample_rate = 16000;
  constexpr std::size_t kNoteFrames = 8;
  const std::size_t frames = kNoteFrames + tail_frames;
  VocalAnalysisData analysis = make_analysis(frames, sample_rate);
  for (std::size_t frame = kNoteFrames; frame < frames; ++frame) {
    analysis.f0_hz[frame] = 0.0f;
    analysis.voiced[frame] = 0;
    analysis.amplitude[frame] = 0.0f;
  }
  const Audio note = make_source(kNoteFrames * 512, sample_rate);
  std::vector<float> samples(frames * 512, 0.0f);
  std::copy(note.begin(), note.end(), samples.begin());
  VocalSessionCreateOptions options;
  options.analysis = analysis;
  options.render_settings.edge_fade_ms = 0.0;
  return VocalEditSession::create(Audio::from_vector(std::move(samples), sample_rate), options);
}

std::vector<float> render_pitched_note(VocalEditSession& session) {
  const auto note = session.notes().front();
  auto draft = session.begin_edit(0);
  auto edit = note.edit;
  edit.pitch.target.mode = PitchTargetMode::kCenter;
  edit.pitch.target.center_midi = 64.0;
  edit.pitch.amount = 1.0;
  draft->apply(draft->token().draft_generation, {SetNoteEditOp{note.id, edit}});
  draft->commit(0);
  return render_snapshot(session.capture_render_snapshot(), {{0, 4096}, 31}).samples;
}

}  // namespace

TEST_CASE("vocal pitch edit does not depend on analysis frames beyond the note context",
          "[vocal_renderer][vocal_context_window]") {
  // Few tail frames keep the whole analysis in reach; many leave most of it out of the pitch
  // backend's view.
  auto near_tail = session_with_silent_tail(40);
  auto far_tail = session_with_silent_tail(600);
  const auto expected = render_pitched_note(near_tail);
  const auto actual = render_pitched_note(far_tail);

  // The edit must have changed the note, or the comparison proves nothing.
  const auto original = near_tail.capture_render_snapshot();
  bool changed = false;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    changed = changed || bits(expected[i]) != bits(original->data().source[i]);
  }
  REQUIRE(changed);
  require_same_bits(actual, expected);
}

TEST_CASE("vocal session accepts exactly the formant shifts the renderer can apply",
          "[vocal_renderer][vocal_formant_range]") {
  auto session = make_session(4096);
  const auto capabilities = session.capabilities();
  const auto note = session.notes().front();

  const auto set_shift = [&](double semitones) {
    auto draft = session.begin_edit(session.token().committed_revision);
    auto edit = note.edit;
    edit.formant.mode = FormantMode::kShift;
    edit.formant.shift_semitones = semitones;
    draft->apply(draft->token().draft_generation, {SetNoteEditOp{note.id, edit}});
    draft->commit(session.token().committed_revision);
  };

  // The published bounds are applicable end to end.
  for (const double shift :
       {capabilities.min_formant_shift_semitones, capabilities.max_formant_shift_semitones}) {
    set_shift(shift);
    CHECK_NOTHROW(render_snapshot(session.capture_render_snapshot(), {{0, 4096}, 41}));
  }
  // Past them the edit is refused at the session, never deferred to the render.
  CHECK_THROWS_AS(set_shift(capabilities.max_formant_shift_semitones + 0.01), VocalEditException);
  CHECK_THROWS_AS(set_shift(capabilities.min_formant_shift_semitones - 0.01), VocalEditException);
}

TEST_CASE("vocal session creation refuses a voiced frame the renderer could not repitch",
          "[vocal_renderer][vocal_context_window]") {
  constexpr int sample_rate = 16000;
  for (const float bad : {9000.0f, std::numeric_limits<float>::infinity(), -1.0f}) {
    VocalSessionCreateOptions options;
    options.analysis = make_analysis(8, sample_rate);
    // A frame far from the first note, where a windowed backend would never look.
    options.analysis->f0_hz[7] = bad;
    CHECK_THROWS_AS(VocalEditSession::create(make_source(4096, sample_rate), options),
                    VocalEditException);
  }
}
