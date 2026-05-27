/// @file acoustic_dataset_test.cpp
/// @brief Optional external acoustic RT60 fixture checks.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "analysis/acoustic_analyzer.h"
#include "core/audio.h"

using Catch::Matchers::WithinAbs;

namespace sonare {
namespace {

std::filesystem::path fixture_root() {
  if (const char* root = std::getenv("SONARE_ACOUSTIC_FIXTURE_ROOT")) {
    if (root[0] != '\0') {
      return std::filesystem::path(root);
    }
  }
  return "tests/fixtures/acoustic";
}

struct AcousticFixtureRow {
  std::string dataset;
  std::string filename;
  std::string mode;
  float expected_rt60 = 0.0f;
  float tolerance_sec = 0.0f;
  int n_octave_bands = 6;
  bool report_only = false;
  float expected_edt = std::numeric_limits<float>::quiet_NaN();
  float edt_tolerance_sec = std::numeric_limits<float>::quiet_NaN();
  float expected_c50 = std::numeric_limits<float>::quiet_NaN();
  float c50_tolerance_db = std::numeric_limits<float>::quiet_NaN();
  float expected_c80 = std::numeric_limits<float>::quiet_NaN();
  float c80_tolerance_db = std::numeric_limits<float>::quiet_NaN();
  float expected_d50 = std::numeric_limits<float>::quiet_NaN();
  float d50_tolerance = std::numeric_limits<float>::quiet_NaN();
};

bool parse_optional_value(const std::string& token, const std::string& prefix, float& value) {
  if (token.rfind(prefix, 0) != 0) {
    return false;
  }
  value = std::stof(token.substr(prefix.size()));
  return true;
}

std::vector<AcousticFixtureRow> read_manifest(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::vector<AcousticFixtureRow> rows;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;

    std::istringstream stream(line);
    std::vector<std::string> fields;
    std::string field;
    while (std::getline(stream, field, '\t')) {
      fields.push_back(field);
    }
    if (fields.size() < 6) continue;

    AcousticFixtureRow row;
    row.dataset = fields[0];
    row.filename = fields[1];
    row.mode = fields[2];
    row.expected_rt60 = std::stof(fields[3]);
    row.tolerance_sec = std::stof(fields[4]);
    row.n_octave_bands = std::stoi(fields[5]);
    row.report_only = std::find(fields.begin() + 6, fields.end(), "report_only") != fields.end();
    for (size_t i = 6; i < fields.size(); ++i) {
      parse_optional_value(fields[i], "edt=", row.expected_edt);
      parse_optional_value(fields[i], "edt_tol=", row.edt_tolerance_sec);
      parse_optional_value(fields[i], "c50=", row.expected_c50);
      parse_optional_value(fields[i], "c50_tol=", row.c50_tolerance_db);
      parse_optional_value(fields[i], "c80=", row.expected_c80);
      parse_optional_value(fields[i], "c80_tol=", row.c80_tolerance_db);
      parse_optional_value(fields[i], "d50=", row.expected_d50);
      parse_optional_value(fields[i], "d50_tol=", row.d50_tolerance);
    }
    rows.push_back(row);
  }
  return rows;
}

bool has_expected(float value, float tolerance) {
  return std::isfinite(value) && std::isfinite(tolerance) && tolerance >= 0.0f;
}

}  // namespace

TEST_CASE("Acoustic optional external RT60 fixtures", "[acoustic_analyzer][dataset]") {
  const std::filesystem::path root = fixture_root();
  const std::filesystem::path manifest = root / "manifest.tsv";
  if (!std::filesystem::exists(manifest)) {
    SKIP("Acoustic fixture manifest is not present");
  }

  const auto rows = read_manifest(manifest);
  if (rows.empty()) {
    SKIP("No acoustic fixture rows are configured");
  }

  size_t checked = 0;
  for (const auto& row : rows) {
    const std::filesystem::path audio_path = root / row.filename;
    if (!std::filesystem::exists(audio_path)) continue;

    INFO("Acoustic fixture: " << row.dataset << " / " << row.filename);
    AcousticConfig config;
    config.n_octave_bands = row.n_octave_bands;
    const Audio audio = Audio::from_file(audio_path.string());

    AcousticParameters params;
    if (row.mode == "ir") {
      config.mode = AcousticConfig::Mode::ImpulseResponse;
      params = analyze_impulse_response(audio, config);
    } else if (row.mode == "blind") {
      config.mode = AcousticConfig::Mode::Blind;
      params = detect_acoustic(audio, config);
    } else {
      FAIL("Unknown acoustic fixture mode: " << row.mode);
    }

    if (row.report_only) {
      WARN("Report-only acoustic fixture "
           << row.dataset << "/" << row.filename << " measured RT60 " << params.rt60
           << "s; expected " << row.expected_rt60 << "s +/- " << row.tolerance_sec << "s; mode "
           << row.mode);
      if (has_expected(row.expected_edt, row.edt_tolerance_sec)) {
        WARN("Report-only acoustic fixture "
             << row.dataset << "/" << row.filename << " measured EDT " << params.edt
             << "s; expected " << row.expected_edt << "s +/- " << row.edt_tolerance_sec << "s");
      }
      if (has_expected(row.expected_c50, row.c50_tolerance_db)) {
        WARN("Report-only acoustic fixture "
             << row.dataset << "/" << row.filename << " measured C50 " << params.c50
             << "dB; expected " << row.expected_c50 << "dB +/- " << row.c50_tolerance_db << "dB");
      }
      if (has_expected(row.expected_c80, row.c80_tolerance_db)) {
        WARN("Report-only acoustic fixture "
             << row.dataset << "/" << row.filename << " measured C80 " << params.c80
             << "dB; expected " << row.expected_c80 << "dB +/- " << row.c80_tolerance_db << "dB");
      }
      if (has_expected(row.expected_d50, row.d50_tolerance)) {
        WARN("Report-only acoustic fixture " << row.dataset << "/" << row.filename
                                             << " measured D50 " << params.d50 << "; expected "
                                             << row.expected_d50 << " +/- " << row.d50_tolerance);
      }
    } else {
      REQUIRE(std::isfinite(params.rt60));
      REQUIRE_THAT(params.rt60, WithinAbs(row.expected_rt60, row.tolerance_sec));
      if (has_expected(row.expected_edt, row.edt_tolerance_sec)) {
        REQUIRE(std::isfinite(params.edt));
        REQUIRE_THAT(params.edt, WithinAbs(row.expected_edt, row.edt_tolerance_sec));
      }
      if (has_expected(row.expected_c50, row.c50_tolerance_db)) {
        REQUIRE(std::isfinite(params.c50));
        REQUIRE_THAT(params.c50, WithinAbs(row.expected_c50, row.c50_tolerance_db));
      }
      if (has_expected(row.expected_c80, row.c80_tolerance_db)) {
        REQUIRE(std::isfinite(params.c80));
        REQUIRE_THAT(params.c80, WithinAbs(row.expected_c80, row.c80_tolerance_db));
      }
      if (has_expected(row.expected_d50, row.d50_tolerance)) {
        REQUIRE(std::isfinite(params.d50));
        REQUIRE_THAT(params.d50, WithinAbs(row.expected_d50, row.d50_tolerance));
      }
    }
    ++checked;
  }

  if (checked == 0) {
    SKIP("No acoustic audio fixtures are present");
  }
}

}  // namespace sonare
