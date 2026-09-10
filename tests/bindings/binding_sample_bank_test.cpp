/// @file binding_sample_bank_test.cpp
/// @brief Sample-bank C surface: handle lifetime, the argument rejections, the
///        zero-init conventions the header promises, and a bounce that renders
///        host PCM through a sample patch.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kSourceRate = 44100.0;

/// A pitched tone with content across the band, so a filter has something to
/// remove and a render is measurably not silence.
std::vector<float> tone(double hz, double seconds) {
  const size_t n = static_cast<size_t>(seconds * kSourceRate);
  std::vector<float> out(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kSourceRate;
    double v = 0.0;
    for (int h = 1; h <= 24; ++h) {
      const double f = hz * h;
      if (f > 0.45 * kSourceRate) break;
      v += std::sin(2.0 * M_PI * f * t) / h;
    }
    out[i] = static_cast<float>(v * 0.5);
  }
  return out;
}

/// A one-track project with a single sounding note on destination 0.
std::string one_note_project() {
  return R"({"version":1,"sample_rate":48000,
    "tracks":[{"id":1,"name":"midi","kind":1,"midi_destination_id":0}],
    "clips":[{"id":1,"track_id":1,"source_id":1,"start_ppq":0,"length_ppq":960}],
    "sources":[{"id":1,"kind":1,"name":""}],
    "midi_content":{"1":[{"data0":546323556,"data1":0,"ppq":0},
                     {"data0":545274880,"data1":0,"ppq":480}]}})";
}

}  // namespace

TEST_CASE("a sample bank rejects what it cannot hold", "[project][sample_bank]") {
  SonareSampleBank* bank = sonare_sample_bank_create();
  REQUIRE(bank != nullptr);

  const std::vector<float> pcm = tone(261.6256, 0.2);
  SonareSampleDesc desc{};

  CHECK(sonare_sample_bank_add_sample(nullptr, pcm.data(), pcm.size(), &desc, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_sample_bank_add_sample(bank, nullptr, pcm.size(), &desc, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_sample_bank_add_sample(bank, pcm.data(), 0, &desc, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), nullptr, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);

  size_t count = 99;
  REQUIRE(sonare_sample_bank_sample_count(bank, &count) == SONARE_OK);
  CHECK(count == 0);

  uint32_t index = 99;
  REQUIRE(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &desc, &index) == SONARE_OK);
  CHECK(index == 0);
  REQUIRE(sonare_sample_bank_sample_count(bank, &count) == SONARE_OK);
  CHECK(count == 1);

  SonareSampleZoneDesc zone{};
  zone.sample_index = 7;  // not in the bank
  CHECK(sonare_sample_bank_add_zone(bank, 0, &zone) == SONARE_ERROR_INVALID_PARAMETER);
  zone.sample_index = 0;
  CHECK(sonare_sample_bank_add_zone(bank, 4096, &zone) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_sample_bank_add_zone(nullptr, 0, &zone) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_sample_bank_add_zone(bank, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_sample_bank_add_zone(bank, 0, &zone) == SONARE_OK);
  REQUIRE(sonare_sample_bank_set_count(bank, &count) == SONARE_OK);
  CHECK(count == 1);

  // A set index creates the sets below it, so the count follows the highest.
  REQUIRE(sonare_sample_bank_add_zone(bank, 3, &zone) == SONARE_OK);
  REQUIRE(sonare_sample_bank_set_count(bank, &count) == SONARE_OK);
  CHECK(count == 4);

  sonare_sample_bank_destroy(bank);
  sonare_sample_bank_destroy(nullptr);  // documented no-op
}

TEST_CASE("an all-zero zone covers the whole keyboard", "[project][sample_bank]") {
  // The header promises zero-initialize-then-override, and a rectangle nobody
  // filled in must not silently cover nothing.
  SonareSampleBank* bank = sonare_sample_bank_create();
  REQUIRE(bank != nullptr);
  const std::vector<float> pcm = tone(261.6256, 0.2);
  SonareSampleDesc desc{};
  REQUIRE(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &desc, nullptr) == SONARE_OK);

  SonareSampleZoneDesc zone{};
  REQUIRE(sonare_sample_bank_add_zone(bank, 0, &zone) == SONARE_OK);

  SonareProject* project = nullptr;
  const std::string doc = one_note_project();
  REQUIRE(sonare_project_deserialize(doc.data(), doc.size(), &project, nullptr) == SONARE_OK);

  SonareSynthInstrumentBinding binding{};
  binding.destination_id = 0;
  binding.patch.struct_version = 3;
  binding.patch.engine_mode = SONARE_SYNTH_ENGINE_SAMPLE;
  binding.sample_bank = bank;

  SonareProjectBounceOptions options{};
  options.sample_rate = 48000;
  options.total_frames = 48000;
  float* out = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &len) == SONARE_OK);
  REQUIRE(out != nullptr);
  float peak = 0.0f;
  for (size_t i = 0; i < len; ++i) peak = std::max(peak, std::fabs(out[i]));
  CHECK(peak > 0.0f);

  sonare_free_floats(out);
  sonare_project_destroy(project);
  sonare_sample_bank_destroy(bank);
}

TEST_CASE("narrowing one range leaves the other covering everything", "[project][sample_bank]") {
  // A zone given only a key range must not come back muted by an empty
  // velocity range it never asked for.
  SonareSampleBank* bank = sonare_sample_bank_create();
  REQUIRE(bank != nullptr);
  const std::vector<float> pcm = tone(261.6256, 0.2);
  SonareSampleDesc desc{};
  REQUIRE(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &desc, nullptr) == SONARE_OK);

  // Only a lower key bound: the upper bound and both velocity bounds must each
  // default on their own rather than collapsing the zone.
  SonareSampleZoneDesc zone{};
  zone.key_lo = 48;
  REQUIRE(sonare_sample_bank_add_zone(bank, 0, &zone) == SONARE_OK);

  // The mirror image, so a regression that collapses either axis fails.
  SonareSampleZoneDesc velocity_only{};
  velocity_only.vel_lo = 64;
  REQUIRE(sonare_sample_bank_add_zone(bank, 1, &velocity_only) == SONARE_OK);

  SonareProject* project = nullptr;
  const std::string doc = one_note_project();  // sounds note 60 at velocity 100
  REQUIRE(sonare_project_deserialize(doc.data(), doc.size(), &project, nullptr) == SONARE_OK);

  SonareSynthInstrumentBinding binding{};
  binding.patch.struct_version = 3;
  binding.patch.engine_mode = SONARE_SYNTH_ENGINE_SAMPLE;
  binding.sample_bank = bank;

  SonareProjectBounceOptions options{};
  options.sample_rate = 48000;
  options.total_frames = 48000;
  float* out = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &len) == SONARE_OK);
  REQUIRE(out != nullptr);
  float peak = 0.0f;
  for (size_t i = 0; i < len; ++i) peak = std::max(peak, std::fabs(out[i]));
  CHECK(peak > 0.0f);

  sonare_free_floats(out);
  sonare_project_destroy(project);
  sonare_sample_bank_destroy(bank);
}

TEST_CASE("a sample patch without a bank renders silence, not an error", "[project][sample_bank]") {
  SonareProject* project = nullptr;
  const std::string doc = one_note_project();
  REQUIRE(sonare_project_deserialize(doc.data(), doc.size(), &project, nullptr) == SONARE_OK);

  SonareSynthInstrumentBinding binding{};
  binding.patch.struct_version = 3;
  binding.patch.engine_mode = SONARE_SYNTH_ENGINE_SAMPLE;
  binding.sample_bank = nullptr;

  SonareProjectBounceOptions options{};
  options.sample_rate = 48000;
  options.total_frames = 24000;
  float* out = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &len) == SONARE_OK);
  REQUIRE(out != nullptr);
  float peak = 0.0f;
  for (size_t i = 0; i < len; ++i) peak = std::max(peak, std::fabs(out[i]));
  CHECK(peak == 0.0f);

  sonare_free_floats(out);
  sonare_project_destroy(project);
}

TEST_CASE("the sample engine appears in the engine-mode names", "[project][sample_bank]") {
  const std::string names = sonare_synth_enum_names(SONARE_SYNTH_ENUM_ENGINE_MODE);
  CHECK(names.find("\nsample") != std::string::npos);
  // The list is "default" plus one name per mode, so its length pins the enum.
  size_t lines = 1;
  for (char c : names) {
    if (c == '\n') ++lines;
  }
  CHECK(lines == SONARE_SYNTH_ENGINE_MODE_COUNT);
}

TEST_CASE("a struct_version past the newest is refused", "[project][sample_bank]") {
  SonareProject* project = nullptr;
  const std::string doc = one_note_project();
  REQUIRE(sonare_project_deserialize(doc.data(), doc.size(), &project, nullptr) == SONARE_OK);

  SonareSynthInstrumentBinding binding{};
  binding.patch.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION + 1;

  SonareProjectBounceOptions options{};
  options.sample_rate = 48000;
  options.total_frames = 4800;
  float* out = nullptr;
  size_t len = 0;
  CHECK(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out, &len) ==
        SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}
