#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "transport/tempo_map.h"

namespace {

struct OwnedTimeSignatureSegments {
  SonareProjectTimeSignatureList value{};
  ~OwnedTimeSignatureSegments() { sonare_free_project_time_signature_list(&value); }
};

void check_bar_grid_origin(double origin, sonare::transport::TimeSignature preceding_signature) {
  using namespace sonare::transport;

  TempoMap map;
  map.prepare(48000.0);
  map.set_time_signatures({{0.0, preceding_signature}, {origin, {4, 4}}});

  const double length = bar_length_ppq({4, 4});
  for (int local_bar = 0; local_bar <= 8; ++local_bar) {
    const int64_t bar_index = local_bar + 1;
    const double expected = origin + static_cast<double>(local_bar) * length;
    const double start = map.bar_index_start_ppq(bar_index);

    REQUIRE(start == expected);
    CHECK(map.bar_start_ppq(start) == start);

    const BarBeat at_start = map.ppq_to_bar_beat(start);
    CHECK(at_start.bar == bar_index);
    CHECK(at_start.beat == 1);
    CHECK(at_start.beat_fraction == 0.0);

    const double before_start = std::nextafter(start, -std::numeric_limits<double>::infinity());
    const BarBeat before = map.ppq_to_bar_beat(before_start);
    CHECK(before.bar == bar_index - 1);
    if (local_bar > 0) {
      CHECK(before.beat == 4);
      CHECK(before.beat_fraction > 1.0 - 1.0e-12);
      CHECK(before.beat_fraction < 1.0);
    }
  }
}

}  // namespace

TEST_CASE("TempoMap bar indexes invert fractional signature starts", "[transport][tempo]") {
  check_bar_grid_origin(0.1, {3, 4});
  check_bar_grid_origin(1.0 / 3.0, {3, 4});
  check_bar_grid_origin(2.74, {4, 4});
}

TEST_CASE("TempoMap preserves exact zero-origin bar grid indexes", "[transport][tempo]") {
  using namespace sonare::transport;

  TempoMap map;
  map.prepare(48000.0);
  map.set_time_signatures({{0.0, {4, 4}}});

  for (int64_t bar = 0; bar <= 8; ++bar) {
    const double expected = static_cast<double>(bar) * bar_length_ppq({4, 4});
    const double start = map.bar_index_start_ppq(bar);
    CHECK(start == expected);
    CHECK(map.bar_start_ppq(start) == expected);
    const BarBeat at_start = map.ppq_to_bar_beat(start);
    CHECK(at_start.bar == bar);
    CHECK(at_start.beat == 1);
    CHECK(at_start.beat_fraction == 0.0);
    if (bar > 0) {
      const double before_start = std::nextafter(start, -std::numeric_limits<double>::infinity());
      CHECK(map.ppq_to_bar_beat(before_start).bar == bar - 1);
    }
  }
}

TEST_CASE("a sub-epsilon signature pickup shares the next bar index", "[transport][tempo]") {
  constexpr double kChangePpq = 4.0 + 1.0e-10;
  const SonareProjectTimeSignatureSegment segments[] = {{0.0, 4, 4}, {kChangePpq, 3, 4}};
  SonareProject* raw_project = nullptr;
  REQUIRE(sonare_project_create(&raw_project) == SONARE_OK);
  const std::unique_ptr<SonareProject, decltype(&sonare_project_destroy)> project(
      raw_project, &sonare_project_destroy);
  REQUIRE(sonare_project_set_time_signatures(project.get(), segments, 2) == SONARE_OK);

  OwnedTimeSignatureSegments stored_segments;
  REQUIRE(sonare_project_time_signatures(project.get(), &stored_segments.value) == SONARE_OK);
  std::vector<sonare::transport::TimeSignatureSegment> signatures;
  for (size_t i = 0; i < stored_segments.value.count; ++i) {
    const SonareProjectTimeSignatureSegment& stored = stored_segments.value.items[i];
    signatures.push_back({stored.start_ppq, {stored.numerator, stored.denominator}});
  }

  sonare::transport::TempoMap map;
  map.prepare(48000.0);
  map.set_time_signatures(signatures);
  const double before_change = std::nextafter(kChangePpq, 0.0);
  CHECK(map.ppq_to_bar_beat(before_change).bar == 1);
  CHECK(map.ppq_to_bar_beat(kChangePpq).bar == 1);
  CHECK(map.bar_start_ppq(before_change) == 4.0);
  CHECK(map.bar_index_start_ppq(1) == kChangePpq);
}
