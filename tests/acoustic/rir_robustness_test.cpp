#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "acoustic/image_source.h"
#include "acoustic/late_reverb.h"
#include "acoustic/material.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"

using namespace sonare;
using namespace sonare::acoustic;

namespace {

bool has_code(const std::vector<Diagnostic>& diags, const std::string& code) {
  return std::any_of(diags.begin(), diags.end(),
                     [&](const Diagnostic& d) { return d.code == code; });
}

// A room whose walls share one uniform material.
ShoeboxRoom uniform_room(float length, float width, float height, float absorption,
                         float scattering = 0.0f) {
  ShoeboxRoom room;
  room.dims = {length, width, height};
  for (Material& w : room.walls) w = uniform_material(absorption, scattering);
  return room;
}

}  // namespace

// Unbounded room dimensions must not overflow the area/volume products to
// infinity, which would turn the RIR into NaN while still reporting success.
TEST_CASE("extreme room dimensions never yield a NaN RIR", "[acoustic][rir]") {
  const int sr = 48000;
  const SourceListener pl{{1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 2.0f}};

  SECTION("huge finite dimensions are rejected with an error, not a NaN RIR") {
    // Far beyond the overflow-safe bound: the area/volume products would run to
    // infinity. Validation must flag it and return an empty RIR.
    const ShoeboxRoom room = uniform_room(1.0e30f, 1.0e30f, 1.0e30f, 0.3f);
    const RirSynthResult res = synthesize_rir(room, pl, sr);
    REQUIRE(has_error(res.diagnostics));
    REQUIRE(has_code(res.diagnostics, "acoustic.invalid_dimensions"));
    REQUIRE(res.rir.empty());

    // The scattering helper (called independently of validation) stays finite.
    const float s = shoebox_mean_scattering(room);
    REQUIRE(std::isfinite(s));
  }

  SECTION("a large-but-valid room synthesizes a finite RIR") {
    // Right at the generous upper bound: still allowed, and every product stays
    // inside the finite float range. Cap the length so the test stays cheap.
    const ShoeboxRoom room = uniform_room(9000.0f, 9000.0f, 9000.0f, 0.3f);
    // Close together so the direct arrival lands inside the capped window.
    const SourceListener inside{{100.0f, 100.0f, 100.0f}, {110.0f, 105.0f, 102.0f}};
    RirSynthConfig cfg;
    cfg.max_seconds = 0.25f;  // keep the buffer small/fast
    const RirSynthResult res = synthesize_rir(room, inside, sr, cfg);
    REQUIRE_FALSE(has_error(res.diagnostics));
    REQUIRE(res.rir.size() > 0);
    for (size_t i = 0; i < res.rir.size(); ++i) REQUIRE(std::isfinite(res.rir[i]));
  }
}

TEST_CASE("RIR synthesis rejects non-finite material and timing inputs",
          "[acoustic][rir][numeric]") {
  const SourceListener placement{{1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 2.0f}};

  ShoeboxRoom room = uniform_room(7.0f, 5.0f, 3.0f, 0.3f);
  room.walls[0].absorption[0] = std::numeric_limits<float>::quiet_NaN();
  auto result = synthesize_rir(room, placement, 48000);
  REQUIRE(has_error(result.diagnostics));
  REQUIRE(has_code(result.diagnostics, "acoustic.invalid_absorption"));
  REQUIRE(result.rir.empty());

  room = uniform_room(7.0f, 5.0f, 3.0f, 0.3f);
  room.walls[0].scattering[0] = std::numeric_limits<float>::infinity();
  result = synthesize_rir(room, placement, 48000);
  REQUIRE(has_error(result.diagnostics));
  REQUIRE(has_code(result.diagnostics, "acoustic.invalid_scattering"));
  REQUIRE(result.rir.empty());

  RirSynthConfig config;
  config.max_seconds = std::numeric_limits<float>::infinity();
  result = synthesize_rir(room, placement, 48000, config);
  REQUIRE(has_error(result.diagnostics));
  REQUIRE(has_code(result.diagnostics, "acoustic.invalid_rir_config"));
  REQUIRE(result.rir.empty());

  config = {};
  result = synthesize_rir(room, placement, std::numeric_limits<int>::max(), config);
  REQUIRE(has_error(result.diagnostics));
  REQUIRE(has_code(result.diagnostics, "acoustic.invalid_sample_rate"));
  REQUIRE(result.rir.empty());
}

// When the late tail is shorter than the mixing time, a crossfade to late-only
// exactly where the tail is zero would silence the early reflections. The
// synthesizer must instead preserve the geometric energy.
TEST_CASE("short late tail keeps early reflections (no fade-to-zero dip)", "[acoustic][rir]") {
  const int sr = 48000;
  // Highly absorptive room => very short RT60 => short late tail. A mixing time
  // pushed past the end of that tail would, before the fix, fade the early
  // reflections toward silence beyond the crossover.
  const ShoeboxRoom room = uniform_room(8.0f, 6.0f, 3.5f, 0.99f);
  const SourceListener pl{{2.0f, 1.5f, 1.5f}, {6.0f, 4.5f, 1.8f}};

  RirSynthConfig cfg;
  cfg.ism_order = 3;
  cfg.mixing_time_ms = 65.0f;  // beyond the short late tail

  const RirSynthResult res = synthesize_rir(room, pl, sr, cfg);
  REQUIRE_FALSE(has_error(res.diagnostics));
  REQUIRE(res.rir.size() > 0);
  REQUIRE(has_code(res.diagnostics, "acoustic.no_late_tail"));

  // No NaN/Inf anywhere in the output.
  for (size_t i = 0; i < res.rir.size(); ++i) REQUIRE(std::isfinite(res.rir[i]));

  // Reference early-only IR. The tail cannot span the crossover, so the RIR is
  // the early reflections unmodified — never faded toward silence.
  const std::vector<ImageSource> images = shoebox_image_sources(room, pl, 3);
  const Audio early = synthesize_early_ir(images, sr);

  double early_energy = 0.0;
  for (size_t i = 0; i < early.size(); ++i) {
    early_energy += static_cast<double>(early[i]) * early[i];
  }
  REQUIRE(early_energy > 0.0);  // the early reflections carry real energy

  const size_t n = std::min(res.rir.size(), early.size());
  REQUIRE(n > 0);
  bool matches_early = true;
  for (size_t i = 0; i < n && matches_early; ++i) {
    matches_early = std::fabs(res.rir[i] - early[i]) < 1e-6f;
  }
  REQUIRE(matches_early);

  // The energy past the would-be crossover is preserved (no dip): it equals the
  // early-only energy there rather than collapsing to zero.
  const int t_mix = static_cast<int>(std::lround(65.0 * 0.001 * sr));
  double rir_post = 0.0;
  double early_post = 0.0;
  for (size_t i = static_cast<size_t>(t_mix); i < n; ++i) {
    rir_post += static_cast<double>(res.rir[i]) * res.rir[i];
    early_post += static_cast<double>(early[i]) * early[i];
  }
  REQUIRE(rir_post >= early_post * 0.999);
}

// A near-rigid room's effectively unbounded RT60 must be clamped before sizing
// the auto tail, so it cannot request a runaway allocation.
TEST_CASE("unbounded RT60 clamps the auto late-tail length", "[acoustic][late_reverb]") {
  const int sr = 8000;  // low rate keeps the (clamped) buffer cheap
  ReverbTime rt;
  rt.rt60_bands = {1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f};

  // Auto sizing (no max_samples): the length is bounded by the RT60 clamp, far
  // below the kMaxAutoSamples ceiling that an unclamped RT60 would otherwise hit.
  const Audio tail = synthesize_late_tail(rt, sr);
  REQUIRE(tail.size() > 0);
  REQUIRE(static_cast<int>(tail.size()) < kMaxAutoSamples);
  for (size_t i = 0; i < tail.size(); ++i) REQUIRE(std::isfinite(tail[i]));
}
