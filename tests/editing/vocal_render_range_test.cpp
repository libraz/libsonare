#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

#include "editing/vocal_edit/renderer.h"
#include "editing/vocal_edit/session.h"

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
