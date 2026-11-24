#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "support/alloc_guard.h"
#include "util/constants.h"

namespace {

struct ProjectHandle {
  SonareProject* value = nullptr;

  ProjectHandle() { REQUIRE(sonare_project_create(&value) == SONARE_OK); }
  ~ProjectHandle() { sonare_project_destroy(value); }
  ProjectHandle(const ProjectHandle&) = delete;
  ProjectHandle& operator=(const ProjectHandle&) = delete;
};

double ppq_to_sec(const SonareProject* project, double ppq) {
  double sec = -1.0;
  REQUIRE(sonare_project_ppq_to_sec(project, ppq, &sec) == SONARE_OK);
  return sec;
}

double sec_to_ppq(const SonareProject* project, double sec) {
  double ppq = -1.0;
  REQUIRE(sonare_project_sec_to_ppq(project, sec, &ppq) == SONARE_OK);
  return ppq;
}

}  // namespace

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("tempo bridge reads nonzero flags and validates inactive intervals",
          "[c_api][project][tempo]") {
  ProjectHandle project;
  const float sample = 0.0f;
  SonareProjectTempoOptions options{};
  sonare_project_tempo_options_init(&options);
  options.adaptive_tempo = 2;
  options.include_octave_candidates = 2;
  SonareProjectTempoCandidate candidates[SONARE_PROJECT_MAX_TEMPO_CANDIDATES]{};
  size_t count = 0;
  CHECK(sonare_project_analyze_tempo(project.value, &sample, 1, 22050, &options, candidates,
                                     SONARE_PROJECT_MAX_TEMPO_CANDIDATES, &count) == SONARE_OK);
  CHECK(count > 0);

  options.adaptive_tempo = 0;
  options.tempo_update_interval_beats = 0;
  count = 42;
  CHECK(sonare_project_analyze_tempo(project.value, &sample, 1, 22050, &options, candidates,
                                     SONARE_PROJECT_MAX_TEMPO_CANDIDATES,
                                     &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) ==
        "options.tempo_update_interval_beats must be an integer in [1, 2147483647]");
  CHECK(count == 0);
}
#endif

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("project snap-to-grid clears failure output and validates by argument order",
          "[c_api][project][tempo]") {
  ProjectHandle project;
  SonareProjectSnapToGridOptions options{};
  sonare_project_snap_to_grid_options_init(&options);
  double out_ppq = 42.0;

  options.struct_size = 0;
  CHECK(sonare_project_snap_to_grid(project.value, -1.0, 2.0, &options, &out_ppq) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out_ppq == 0.0);
  CHECK(std::string(sonare_last_error_message()) == "ppq must be finite and in [0, 1e12]");
  CHECK(sonare_project_snap_to_grid(project.value, -1.0, 0.5, nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "ppq must be finite and in [0, 1e12]");

  out_ppq = 42.0;
  CHECK(sonare_project_snap_to_grid(project.value, 1.0, 2.0, &options, &out_ppq) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out_ppq == 0.0);
  CHECK(std::string(sonare_last_error_message()) == "strength must be finite and in [0, 1]");
  CHECK(sonare_project_snap_to_grid(project.value, 1.0, 2.0, nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "strength must be finite and in [0, 1]");

  sonare_project_snap_to_grid_options_init(&options);
  options.division = -1;
  out_ppq = 42.0;
  CHECK(sonare_project_snap_to_grid(project.value, 1.0, 0.5, &options, &out_ppq) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(out_ppq == 0.0);
  CHECK(std::string(sonare_last_error_message()) ==
        "options.division must be an integer in [0, 1024]");
  CHECK(sonare_project_snap_to_grid(project.value, 1.0, 0.5, &options, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) ==
        "options.division must be an integer in [0, 1024]");
  sonare_project_snap_to_grid_options_init(&options);
  CHECK(sonare_project_snap_to_grid(project.value, 1.0, 0.5, &options, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "out_ppq must not be NULL");

  out_ppq = 42.0;
  CHECK(sonare_project_snap_to_grid(project.value, 1.25, 0.0, nullptr, &out_ppq) == SONARE_OK);
  CHECK(out_ppq == 1.25);
}
#endif

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_MIXING)
TEST_CASE("take alignment returns identity for rounding-level perturbations",
          "[c_api][project][alignment]") {
  constexpr int kRate = 48000;
  constexpr int kHop = 512;
  std::vector<float> reference(kRate / 2);
  for (size_t i = 0; i < reference.size(); ++i) {
    const double t = static_cast<double>(i) / kRate;
    reference[i] = static_cast<float>(0.34 * std::sin(sonare::constants::kTwoPiD * 257.0 * t) +
                                      0.17 * std::sin(sonare::constants::kTwoPiD * 1373.0 * t));
  }
  auto take = reference;
  SECTION("with exact samples") {}
  SECTION("with rounding-level sample perturbations") {
    for (size_t i = 0; i < take.size(); ++i) {
      take[i] = static_cast<float>(take[i] + (i % 2 == 0 ? 1e-8 : -1e-8));
    }
    REQUIRE(take != reference);
  }
  SonareProjectWarpAnchor* raw = nullptr;
  size_t count = 0;
  SonareTakeAlignment alignment{};
  REQUIRE(sonare_align_take_to_reference(reference.data(), reference.size(), kRate, take.data(),
                                         take.size(), kRate, nullptr, &raw, &count,
                                         &alignment) == SONARE_OK);
  const std::unique_ptr<SonareProjectWarpAnchor, decltype(&sonare_free_project_warp_anchors)>
      anchors(raw, sonare_free_project_warp_anchors);
  REQUIRE(anchors != nullptr);
  REQUIRE(count == 47);
  CHECK(alignment.reference_frames == 47);
  CHECK(alignment.take_frames == 47);
  CHECK(std::abs(alignment.mean_residual_frames) < 5e-7f);
  for (size_t i = 0; i < count; ++i) {
    CHECK(raw[i].warp_sample == static_cast<double>(i * kHop));
    CHECK(raw[i].source_sample == static_cast<double>(i * kHop));
  }
}

TEST_CASE("take alignment leaves diagnostics empty when output allocation fails",
          "[c_api][project][allocation]") {
  constexpr int kSampleRate = 22050;
  std::vector<float> audio(2u * static_cast<size_t>(kSampleRate));
  double phase = 0.0;
  for (size_t i = 0; i < audio.size(); ++i) {
    const double fraction = static_cast<double>(i) / static_cast<double>(audio.size());
    const double frequency = 220.0 + 440.0 * fraction;
    phase += 2.0 * sonare::constants::kPiD * frequency / static_cast<double>(kSampleRate);
    audio[i] = static_cast<float>(0.4 * std::sin(phase) + 0.2 * std::sin(2.0 * phase));
  }

  const auto align = [&](SonareProjectWarpAnchor** anchors, size_t* count,
                         SonareTakeAlignment* alignment) {
    return sonare_align_take_to_reference(audio.data(), audio.size(), kSampleRate, audio.data(),
                                          audio.size(), kSampleRate, nullptr, anchors, count,
                                          alignment);
  };

  SonareProjectWarpAnchor* anchors = nullptr;
  size_t count = 0;
  SonareTakeAlignment alignment{};
  REQUIRE(align(&anchors, &count, &alignment) == SONARE_OK);
  sonare_free_project_warp_anchors(anchors);

  size_t allocation_count = 0;
  SonareError measured_error;
  {
    sonare::test::AllocationFailureAtGuard count_allocations(std::numeric_limits<size_t>::max());
    measured_error = align(&anchors, &count, &alignment);
    allocation_count = sonare::test::g_allocation_attempts.load(std::memory_order_relaxed);
  }
  REQUIRE(measured_error == SONARE_OK);
  REQUIRE(anchors != nullptr);
  REQUIRE(count >= 2);
  REQUIRE(alignment.reference_frames > 0);
  sonare_free_project_warp_anchors(anchors);
  REQUIRE(allocation_count > 0);

  SonareProjectWarpAnchor prior_anchor{};
  anchors = &prior_anchor;
  count = 42;
  alignment = SonareTakeAlignment{1.0f, 42, 43};
  SonareError error;
  {
    sonare::test::AllocationFailureAtGuard fail(allocation_count);
    error = align(&anchors, &count, &alignment);
  }
  CHECK(error == SONARE_ERROR_OUT_OF_MEMORY);
  CHECK(anchors == nullptr);
  CHECK(count == 0);
  CHECK(alignment.mean_residual_frames == 0.0f);
  CHECK(alignment.reference_frames == 0);
  CHECK(alignment.take_frames == 0);
  if (anchors != nullptr && anchors != &prior_anchor) sonare_free_project_warp_anchors(anchors);
}
#endif

TEST_CASE("project PPQ and seconds conversions round-trip through tempo ramps",
          "[c_api][project][tempo]") {
  ProjectHandle at_44100;
  ProjectHandle at_48000;
  REQUIRE(sonare_project_set_sample_rate(at_44100.value, 44100) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(at_48000.value, 48000) == SONARE_OK);

  const SonareProjectTempoSegment tempos[] = {
      {0.0, 120.0, 0.0}, {4.0, 120.0, 90.0}, {16.0, 90.0, 0.0}};
  REQUIRE(sonare_project_set_tempo_segments(at_44100.value, tempos, 3) == SONARE_OK);
  REQUIRE(sonare_project_set_tempo_segments(at_48000.value, tempos, 3) == SONARE_OK);

  for (double ppq : {0.0, 4.0, 10.0, 16.0, 32.0, 1.0e12}) {
    const double sec = ppq_to_sec(at_44100.value, ppq);
    const double tolerance = std::max(1.0e-9, 1.0e-9 * std::abs(ppq));
    CHECK(sec_to_ppq(at_44100.value, sec) == Catch::Approx(ppq).margin(tolerance));
    CHECK(ppq_to_sec(at_48000.value, ppq) == Catch::Approx(sec).margin(1.0e-12 * sec));
  }

  const double max_sec = ppq_to_sec(at_44100.value, 1.0e12);
  CHECK(sec_to_ppq(at_44100.value, max_sec) == 1.0e12);
}

TEST_CASE("project tempo conversions name refused arguments", "[c_api][project][tempo]") {
  ProjectHandle project;
  double result = 123.0;

  CHECK(sonare_project_ppq_to_sec(project.value, -1.0, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(result == 0.0);
  CHECK(std::string(sonare_last_error_message()).find("ppq") != std::string::npos);

  result = 123.0;
  CHECK(sonare_project_sec_to_ppq(project.value, -1.0, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(result == 0.0);
  CHECK(std::string(sonare_last_error_message()).find("sec") != std::string::npos);

  const double max_sec = ppq_to_sec(project.value, 1.0e12);
  CHECK(sonare_project_sec_to_ppq(project.value,
                                  std::nextafter(max_sec, std::numeric_limits<double>::infinity()),
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("sec") != std::string::npos);
}
