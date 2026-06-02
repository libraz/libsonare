#include "acoustic/rir_synthesizer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

#include "acoustic/image_source.h"
#include "acoustic/late_reverb.h"
#include "acoustic/material.h"
#include "acoustic/room_model.h"
#include "analysis/acoustic_analyzer.h"

using Catch::Matchers::WithinRel;
using namespace sonare;
using namespace sonare::acoustic;

namespace {

ShoeboxRoom uniform_room(float length, float width, float height, float absorption) {
  ShoeboxRoom room;
  room.dims = {length, width, height};
  for (Material& w : room.walls) w = uniform_material(absorption, 0.0f);
  return room;
}

bool has_code(const std::vector<Diagnostic>& diags, const std::string& code) {
  return std::any_of(diags.begin(), diags.end(),
                     [&](const Diagnostic& d) { return d.code == code; });
}

}  // namespace

TEST_CASE("synthesize_rir produces a usable mono RIR", "[acoustic][rir]") {
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(7.0f, 5.0f, 3.0f, 0.15f);
  const SourceListener pl{{1.5f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};

  const RirSynthResult res = synthesize_rir(room, pl, sr);
  REQUIRE_FALSE(has_error(res.diagnostics));
  REQUIRE(res.rir.size() > 0);
  REQUIRE(res.rir.sample_rate() == sr);

  // Tail length tracks the longest band RT60 (late tail dominates the length).
  const ReverbTime rt = shoebox_reverb_time(room, ReverbModel::Eyring);
  const float longest = *std::max_element(rt.rt60_bands.begin(), rt.rt60_bands.end());
  REQUIRE(static_cast<float>(res.rir.size()) >= longest * static_cast<float>(sr));
}

TEST_CASE("synthesize_rir is deterministic for a fixed seed", "[acoustic][rir]") {
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(7.0f, 5.0f, 3.0f, 0.15f);
  const SourceListener pl{{1.5f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};

  const Audio a = synthesize_rir(room, pl, sr, {/*ism_order=*/3}).rir;
  const Audio b = synthesize_rir(room, pl, sr, {/*ism_order=*/3}).rir;
  REQUIRE(a.size() == b.size());
  REQUIRE(a.size() > 0);
  for (size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);  // bit-exact
}

TEST_CASE("synthesize_rir reports geometry errors and yields no RIR", "[acoustic][rir]") {
  const ShoeboxRoom room = uniform_room(7.0f, 5.0f, 3.0f, 0.15f);
  // Source outside the room.
  const SourceListener pl{{99.0f, 1.0f, 1.0f}, {5.0f, 4.0f, 1.7f}};

  const RirSynthResult res = synthesize_rir(room, pl, 48000);
  REQUIRE(has_error(res.diagnostics));
  REQUIRE(has_code(res.diagnostics, "acoustic.source_outside_room"));
  REQUIRE(res.rir.empty());
}

TEST_CASE("synthesize_rir clamps length to max_seconds with a diagnostic", "[acoustic][rir]") {
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(10.0f, 8.0f, 4.0f, 0.06f);  // long RT60
  const SourceListener pl{{2.0f, 2.0f, 1.5f}, {8.0f, 6.0f, 2.0f}};

  RirSynthConfig cfg;
  cfg.max_seconds = 0.1f;  // far shorter than the natural tail
  const RirSynthResult res = synthesize_rir(room, pl, sr, cfg);
  REQUIRE_FALSE(has_error(res.diagnostics));
  REQUIRE(has_code(res.diagnostics, "acoustic.rir_length_clamped"));
  REQUIRE(static_cast<int>(res.rir.size()) <= static_cast<int>(std::ceil(0.1f * sr)));

  // The clamp is a real truncation: the unclamped RIR is genuinely longer, and
  // it carries no clamp diagnostic.
  const RirSynthResult full = synthesize_rir(room, pl, sr, {/*ism_order=*/3});
  REQUIRE(full.rir.size() > res.rir.size());
  REQUIRE_FALSE(has_code(full.diagnostics, "acoustic.rir_length_clamped"));
}

TEST_CASE("synthesized RIR renders the direct sound at full level", "[acoustic][rir]") {
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(8.0f, 6.0f, 3.5f, 0.12f);
  const SourceListener pl{{2.0f, 1.5f, 1.5f}, {6.0f, 4.5f, 1.8f}};

  // The direct sound is the loudest, first-arriving image; the crossover must
  // not attenuate it. Compare the RIR's direct-region peak against the
  // early-only IR's direct peak (full level == no fade applied there).
  const std::vector<ImageSource> images = shoebox_image_sources(room, pl, 3);
  const Audio early = synthesize_early_ir(images, sr);
  const Audio rir = synthesize_rir(room, pl, sr, {/*ism_order=*/3}).rir;

  const float direct_dist = length(pl.listener - pl.source);
  const int direct_sample = static_cast<int>(std::lround(direct_dist / kSoundSpeed * sr));
  const int lo = std::max(0, direct_sample - 8);
  const int hi = std::min(static_cast<int>(early.size()), direct_sample + 8);
  REQUIRE(hi > lo);

  float early_peak = 0.0f;
  float rir_peak = 0.0f;
  for (int i = lo; i < hi; ++i) {
    early_peak = std::max(early_peak, std::fabs(early[static_cast<size_t>(i)]));
    rir_peak = std::max(rir_peak, std::fabs(rir[static_cast<size_t>(i)]));
  }
  REQUIRE(early_peak > 0.0f);
  // Full level (within rounding); a faded direct would be visibly lower.
  REQUIRE_THAT(rir_peak, WithinRel(early_peak, 0.02f));
}

TEST_CASE("synthesize_rir honours mixing-time and crossfade overrides", "[acoustic][rir]") {
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(7.0f, 5.0f, 3.0f, 0.15f);
  const SourceListener pl{{1.5f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};

  RirSynthConfig cfg;
  cfg.mixing_time_ms = 40.0f;
  cfg.crossfade_ms = 10.0f;
  const RirSynthResult res = synthesize_rir(room, pl, sr, cfg);
  REQUIRE_FALSE(has_error(res.diagnostics));
  REQUIRE(res.rir.size() > 0);
  // A different seed must yield a different tail (seed is actually used).
  RirSynthConfig other = cfg;
  other.seed = 99u;
  const Audio b = synthesize_rir(room, pl, sr, other).rir;
  bool differs = false;
  for (size_t i = 0; i < res.rir.size() && !differs; ++i) differs = (res.rir[i] != b[i]);
  REQUIRE(differs);
}

TEST_CASE("synthesized RIR round-trips RT60 within 10%", "[acoustic][rir]") {
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(8.0f, 6.0f, 3.5f, 0.12f);
  const SourceListener pl{{2.0f, 1.5f, 1.5f}, {6.0f, 4.5f, 1.8f}};

  const RirSynthResult res = synthesize_rir(room, pl, sr, {/*ism_order=*/3});
  REQUIRE_FALSE(has_error(res.diagnostics));

  // Design target: uniform absorption -> identical per-band RT60.
  const ReverbTime rt = shoebox_reverb_time(room, ReverbModel::Eyring);
  const float target = rt.rt60_bands.front();

  const AcousticParameters params = AcousticAnalyzer::from_impulse_response(res.rir).parameters();
  REQUIRE(std::isfinite(params.rt60));
  REQUIRE_THAT(params.rt60, WithinRel(target, 0.10f));
  // The splice keeps the early energy, so clarity is finite and well-defined.
  REQUIRE(std::isfinite(params.c50));
  REQUIRE(std::isfinite(params.d50));
}
