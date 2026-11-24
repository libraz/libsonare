/// @file binding_sample_bank_test.cpp
/// @brief Sample-bank C surface: handle lifetime, the argument rejections, the
///        zero-init conventions the header promises, and a bounce that renders
///        host PCM through a sample patch.

#include <sonare/sonare_c.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "support/project_bounce.h"

namespace {

// Bounces with one kind of hosted-instrument list set on a copy of the options.
SonareError bounce_with_synth_instruments(SonareProject* project,
                                          const SonareProjectBounceOptions* options,
                                          const SonareSynthInstrumentBinding* bindings,
                                          size_t count, float** out_interleaved, size_t* out_len,
                                          int* out_num_channels) {
  SonareProjectBounceOptions with_instruments;
  sonare_project_bounce_options_init(&with_instruments);
  if (options != nullptr) with_instruments = *options;
  with_instruments.synth_instruments = bindings;
  with_instruments.synth_instrument_count = count;
  return sonare_test::bounce_interleaved(project, &with_instruments, out_interleaved, out_len,
                                         out_num_channels);
}

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
  return R"({"version":7,"sampleRate":48000,
    "tracks":[{"id":1,"name":"midi","kind":"midi","midiDestinationId":0}],
    "clips":[{"id":1,"trackId":1,"sourceId":1,"startPpq":0,"lengthPpq":960}],
    "sources":[{"id":1,"kind":"midi","name":""}],
    "midiContent":{"1":[{"word0":546323556,"word1":0,"ppq":0},
                     {"word0":545274880,"word1":0,"ppq":480}]}})";
}

}  // namespace

TEST_CASE("a sample bank rejects what it cannot hold", "[project][sample_bank]") {
  SonareSampleBank* bank = nullptr;
  REQUIRE(sonare_sample_bank_create(&bank) == SONARE_OK);
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

TEST_CASE("sample-bank MIDI keys are bounded while zero defaults remain available",
          "[domain-sample-bank]") {
  SonareSampleBank* bank = nullptr;
  REQUIRE(sonare_sample_bank_create(&bank) == SONARE_OK);
  const std::array<float, 2> pcm{};
  SonareSampleDesc sample{};
  uint32_t index = 77;

  sample.root_key = 128;
  CHECK(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &sample, &index) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "desc.root_key must be an integer in [0, 127]");
  CHECK(index == 0);
  size_t sample_count = 9;
  REQUIRE(sonare_sample_bank_sample_count(bank, &sample_count) == SONARE_OK);
  CHECK(sample_count == 0);

  sample.root_key = 0;  // The C zero sentinel still selects middle C.
  REQUIRE(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &sample, &index) ==
          SONARE_OK);
  REQUIRE(index == 0);
  sample.root_key = 127;
  REQUIRE(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &sample, &index) ==
          SONARE_OK);
  CHECK(index == 1);

  SonareSampleZoneDesc zone{};
  zone.sample_index = index;
  const struct {
    const char* subject;
    uint8_t SonareSampleZoneDesc::*field;
  } invalid_fields[] = {
      {"zone.key_lo", &SonareSampleZoneDesc::key_lo},
      {"zone.key_hi", &SonareSampleZoneDesc::key_hi},
      {"zone.vel_lo", &SonareSampleZoneDesc::vel_lo},
      {"zone.vel_hi", &SonareSampleZoneDesc::vel_hi},
  };
  for (const auto& field : invalid_fields) {
    SonareSampleZoneDesc invalid = zone;
    invalid.*(field.field) = 128;
    CHECK(sonare_sample_bank_add_zone(bank, 0, &invalid) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) ==
          std::string(field.subject) + " must be an integer in [0, 127]");
  }
  size_t set_count = 99;
  REQUIRE(sonare_sample_bank_set_count(bank, &set_count) == SONARE_OK);
  CHECK(set_count == 0);

  // Zero keeps each documented default, and a valid rectangle still loads.
  REQUIRE(sonare_sample_bank_add_zone(bank, 0, &zone) == SONARE_OK);
  SonareSampleZoneDesc valid = zone;
  valid.key_lo = 126;
  valid.key_hi = 127;
  valid.vel_lo = 126;
  valid.vel_hi = 127;
  REQUIRE(sonare_sample_bank_add_zone(bank, 0, &valid) == SONARE_OK);

  SonareSampleZoneDesc inverted = zone;
  inverted.key_lo = 127;
  inverted.key_hi = 126;
  CHECK(sonare_sample_bank_add_zone(bank, 0, &inverted) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "zone.key_lo must not exceed zone.key_hi");
  inverted = zone;
  inverted.vel_lo = 127;
  inverted.vel_hi = 126;
  CHECK(sonare_sample_bank_add_zone(bank, 0, &inverted) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "zone.vel_lo must not exceed zone.vel_hi");

  set_count = 99;
  REQUIRE(sonare_sample_bank_set_count(bank, &set_count) == SONARE_OK);
  CHECK(set_count == 1);
  sonare_sample_bank_destroy(bank);
}

TEST_CASE("a sample bank refuses a value it could not attribute later", "[project][sample_bank]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  SonareSampleBank* bank = nullptr;
  REQUIRE(sonare_sample_bank_create(&bank) == SONARE_OK);
  REQUIRE(bank != nullptr);

  std::vector<float> pcm = tone(261.6256, 0.2);
  REQUIRE(pcm.size() > 1000);

  SECTION("a non-finite frame, whichever kind") {
    for (const float bad : {nan, inf, -inf}) {
      std::vector<float> poisoned = pcm;
      poisoned[500] = bad;
      SonareSampleDesc desc{};
      desc.source_rate = kSourceRate;
      CHECK(sonare_sample_bank_add_sample(bank, poisoned.data(), poisoned.size(), &desc, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    }
  }

  SECTION("a non-finite tuning field, which silences the voice rather than poisoning it") {
    SonareSampleDesc desc{};
    desc.fine_tune_cents = nan;
    CHECK(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &desc, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

    desc = SonareSampleDesc{};
    desc.source_rate = nan;
    CHECK(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &desc, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
    // Rejected rather than read as the zero that means "the render's own rate":
    // a comparison against zero is false for a NaN, so the sentinel would have
    // absorbed a caller's mistake into a value nothing downstream can question.
    desc.source_rate = inf;
    CHECK(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &desc, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  }

  // A refusal leaves the bank alone. Checking the count rather than the return
  // code is what separates refusing from appending and then reporting failure.
  size_t count = 99;
  REQUIRE(sonare_sample_bank_sample_count(bank, &count) == SONARE_OK);
  CHECK(count == 0);

  // The same bank still takes the clean sample, so the refusals above are the
  // values being read and not the bank having been left unusable.
  SonareSampleDesc good{};
  good.source_rate = kSourceRate;
  uint32_t index = 99;
  REQUIRE(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &good, &index) == SONARE_OK);
  CHECK(index == 0);
  REQUIRE(sonare_sample_bank_sample_count(bank, &count) == SONARE_OK);
  CHECK(count == 1);

  sonare_sample_bank_destroy(bank);
}

TEST_CASE("a C sample bank rejects non-finite zone values before changing state",
          "[project][sample_bank]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const float max = std::numeric_limits<float>::max();

  SonareSampleBank* bank = nullptr;
  REQUIRE(sonare_sample_bank_create(&bank) == SONARE_OK);
  REQUIRE(bank != nullptr);
  const std::vector<float> pcm(16, 0.25f);
  SonareSampleDesc desc{};
  uint32_t sample_index = 0;
  REQUIRE(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &desc, &sample_index) ==
          SONARE_OK);

  SonareSampleDesc max_tune_desc{};
  max_tune_desc.fine_tune_cents = max;
  uint32_t overflow_sample_index = 0;
  REQUIRE(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &max_tune_desc,
                                        &overflow_sample_index) == SONARE_OK);

  struct InvalidField {
    float SonareSampleZoneDesc::*field;
    float value;
  };
  const std::array<InvalidField, 9> invalid_fields = {{
      {&SonareSampleZoneDesc::tune_cents, nan},
      {&SonareSampleZoneDesc::tune_cents, inf},
      {&SonareSampleZoneDesc::tune_cents, -inf},
      {&SonareSampleZoneDesc::gain, nan},
      {&SonareSampleZoneDesc::gain, inf},
      {&SonareSampleZoneDesc::gain, -inf},
      {&SonareSampleZoneDesc::pan_units, nan},
      {&SonareSampleZoneDesc::pan_units, inf},
      {&SonareSampleZoneDesc::pan_units, -inf},
  }};

  constexpr uint32_t kAbsentSet = 7;
  for (const InvalidField& invalid : invalid_fields) {
    SonareSampleZoneDesc zone{};
    zone.sample_index = sample_index;
    zone.*(invalid.field) = invalid.value;
    CHECK(sonare_sample_bank_add_zone(bank, kAbsentSet, &zone) == SONARE_ERROR_INVALID_PARAMETER);
    size_t set_count = 99;
    REQUIRE(sonare_sample_bank_set_count(bank, &set_count) == SONARE_OK);
    CHECK(set_count == 0);
  }

  SonareSampleZoneDesc overflow{};
  overflow.sample_index = overflow_sample_index;
  overflow.tune_cents = max;
  CHECK(sonare_sample_bank_add_zone(bank, kAbsentSet, &overflow) == SONARE_ERROR_INVALID_PARAMETER);
  size_t set_count = 99;
  REQUIRE(sonare_sample_bank_set_count(bank, &set_count) == SONARE_OK);
  CHECK(set_count == 0);

  SonareSampleZoneDesc negative{};
  negative.sample_index = sample_index;
  negative.tune_cents = -25.0f;
  negative.gain = -0.5f;
  negative.pan_units = -125.0f;
  REQUIRE(sonare_sample_bank_add_zone(bank, 0, &negative) == SONARE_OK);

  SonareSampleZoneDesc out_of_range_pan{};
  out_of_range_pan.sample_index = sample_index;
  out_of_range_pan.pan_units = 900.0f;
  REQUIRE(sonare_sample_bank_add_zone(bank, 1, &out_of_range_pan) == SONARE_OK);
  REQUIRE(sonare_sample_bank_set_count(bank, &set_count) == SONARE_OK);
  CHECK(set_count == 2);

  sonare_sample_bank_destroy(bank);
}

TEST_CASE("an all-zero zone covers the whole keyboard", "[project][sample_bank]") {
  // The header promises zero-initialize-then-override, and a rectangle nobody
  // filled in must not silently cover nothing.
  SonareSampleBank* bank = nullptr;
  REQUIRE(sonare_sample_bank_create(&bank) == SONARE_OK);
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
  sonare_synth_patch_init(&binding.patch);
  binding.patch.engine_mode = SONARE_SYNTH_ENGINE_SAMPLE;
  binding.sample_bank = bank;

  SonareProjectBounceOptions options;
  sonare_project_bounce_options_init(&options);

  options.total_frames = 48000;
  float* out = nullptr;
  size_t len = 0;
  int bounce_channels1 = 0;
  REQUIRE(bounce_with_synth_instruments(project, &options, &binding, 1, &out, &len,
                                        &bounce_channels1) == SONARE_OK);
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
  SonareSampleBank* bank = nullptr;
  REQUIRE(sonare_sample_bank_create(&bank) == SONARE_OK);
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
  sonare_synth_patch_init(&binding.patch);
  binding.patch.engine_mode = SONARE_SYNTH_ENGINE_SAMPLE;
  binding.sample_bank = bank;

  SonareProjectBounceOptions options;
  sonare_project_bounce_options_init(&options);

  options.total_frames = 48000;
  float* out = nullptr;
  size_t len = 0;
  int bounce_channels2 = 0;
  REQUIRE(bounce_with_synth_instruments(project, &options, &binding, 1, &out, &len,
                                        &bounce_channels2) == SONARE_OK);
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
  sonare_synth_patch_init(&binding.patch);
  binding.patch.engine_mode = SONARE_SYNTH_ENGINE_SAMPLE;
  binding.sample_bank = nullptr;

  SonareProjectBounceOptions options;
  sonare_project_bounce_options_init(&options);

  options.total_frames = 24000;
  float* out = nullptr;
  size_t len = 0;
  int bounce_channels3 = 0;
  REQUIRE(bounce_with_synth_instruments(project, &options, &binding, 1, &out, &len,
                                        &bounce_channels3) == SONARE_OK);
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

TEST_CASE("a synth patch whose struct_size is below sizeof is refused", "[project][sample_bank]") {
  SonareProject* project = nullptr;
  const std::string doc = one_note_project();
  REQUIRE(sonare_project_deserialize(doc.data(), doc.size(), &project, nullptr) == SONARE_OK);

  SonareSynthInstrumentBinding binding{};
  sonare_synth_patch_init(&binding.patch);
  binding.patch.struct_size = sizeof(SonareSynthPatch) - 1;

  SonareProjectBounceOptions options;
  sonare_project_bounce_options_init(&options);

  options.total_frames = 4800;
  float* out = nullptr;
  size_t len = 0;
  int bounce_channels4 = 0;
  CHECK(bounce_with_synth_instruments(project, &options, &binding, 1, &out, &len,
                                      &bounce_channels4) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}
