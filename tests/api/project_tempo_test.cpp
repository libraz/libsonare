#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>

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
