/// @file amp_cab_models_test.cpp
/// @brief Amp cabinet models: four distinct voicings, and the realtime `cab` /
///        `cab_model` controls.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <string>
#include <vector>

#include "mastering/saturation/amp_sim.h"
#include "mastering/saturation/cab_voicing.h"
#include "support/alloc_guard.h"
#include "util/constants.h"

namespace {

namespace sat = sonare::mastering::saturation;
using sat::CabModel;

constexpr double kRate = 48000.0;
constexpr int kImpulseLength = 4096;
constexpr int kBlock = 256;
constexpr int kCentres = 24;
// Two models count as different once some third-octave centre parts by this much.
constexpr double kMinDistinctDb = 1.0;

constexpr std::array<CabModel, 4> kModels = {CabModel::kGuitar4x12, CabModel::kBass8x10,
                                             CabModel::kGuitar1x12Combo, CabModel::kGuitar2x12Open};

double magnitude_db(const std::vector<float>& response, double hz) {
  std::complex<double> sum = 0.0;
  const double w = sonare::constants::kTwoPiD * hz / kRate;
  for (size_t n = 0; n < response.size(); ++n) {
    sum += static_cast<double>(response[n]) * std::polar(1.0, -w * static_cast<double>(n));
  }
  return 20.0 * std::log10(std::abs(sum) + 1e-30);
}

std::vector<double> third_octave_response(CabModel model) {
  const sat::CabDesign design =
      sat::design_cab_stage(model, sat::MicModel::kNone, 0.0f, 0.0f, 0.0f, kRate);
  std::vector<float> impulse(kImpulseLength, 0.0f);
  impulse[0] = 1.0f;
  const std::vector<float> response = sat::render_cab_design(design, impulse);
  std::vector<double> db;
  for (int i = 0; i < kCentres; ++i) {
    db.push_back(magnitude_db(response, 50.0 * std::pow(2.0, i / 3.0)));
  }
  return db;
}

std::vector<float> render(sat::AmpSim& amp, int blocks) {
  std::vector<float> out;
  std::array<float, kBlock> buffer{};
  for (int b = 0; b < blocks; ++b) {
    for (int i = 0; i < kBlock; ++i) {
      buffer[static_cast<size_t>(i)] = 0.3f * std::sin(0.05f * static_cast<float>(b * kBlock + i));
    }
    float* channels[] = {buffer.data()};
    amp.process(channels, 1, kBlock);
    out.insert(out.end(), buffer.begin(), buffer.end());
  }
  return out;
}

}  // namespace

TEST_CASE("the four cabinet models are measurably different", "[amp-cab-models]") {
  REQUIRE(sat::kCabModelCount == static_cast<int>(kModels.size()));
  std::array<std::vector<double>, 4> responses;
  for (size_t m = 0; m < kModels.size(); ++m) responses[m] = third_octave_response(kModels[m]);
  for (size_t a = 0; a < kModels.size(); ++a) {
    for (size_t b = a + 1; b < kModels.size(); ++b) {
      double widest = 0.0;
      for (int i = 0; i < kCentres; ++i) {
        widest = std::max(widest, std::fabs(responses[a][static_cast<size_t>(i)] -
                                            responses[b][static_cast<size_t>(i)]));
      }
      INFO("models " << a << " and " << b << " part by " << widest << " dB");
      CHECK(widest >= kMinDistinctDb);
    }
  }
}

TEST_CASE("the original two cabinet voicings are unchanged", "[amp-cab-models]") {
  const sat::CabVoicing guitar = sat::cab_voicing(CabModel::kGuitar4x12);
  CHECK(guitar.highpass_hz == 75.0f);
  CHECK(guitar.bump_hz == 110.0f);
  CHECK(guitar.bump_db == 2.0f);
  CHECK(guitar.presence_hz == 3800.0f);
  CHECK(guitar.rolloff_hz == 4800.0f);
  const sat::CabVoicing bass = sat::cab_voicing(CabModel::kBass8x10);
  CHECK(bass.highpass_hz == 40.0f);
  CHECK(bass.bump_hz == 80.0f);
  CHECK(bass.bump_db == 3.0f);
  CHECK(bass.presence_hz == 2200.0f);
  CHECK(bass.rolloff_hz == 3500.0f);
}

TEST_CASE("cab and cab_model are realtime parameters", "[amp-cab-models]") {
  sat::AmpSim amp;
  amp.prepare(kRate, kBlock);

  bool has_cab = false;
  bool has_model = false;
  for (const auto& d : amp.parameter_descriptors()) {
    if (d.key == "cab") has_cab = d.id == 17;
    if (d.key == "cabModel") has_model = d.id == 18;
  }
  CHECK(has_cab);
  CHECK(has_model);

  // Whole numbers below the model count are accepted; anything else is refused.
  for (int id : {0, 1, 2, 3}) CHECK(amp.set_parameter(18, static_cast<float>(id)));
  CHECK_FALSE(amp.set_parameter(18, 4.0f));
  CHECK_FALSE(amp.set_parameter(18, -1.0f));
  CHECK_FALSE(amp.set_parameter(18, 1.5f));
  CHECK_FALSE(amp.set_parameter(18, std::nanf("")));
  CHECK(amp.amp_config().cab_model == CabModel::kGuitar2x12Open);
  CHECK(amp.set_parameter(17, 0.0f));
  CHECK_FALSE(amp.amp_config().cab);
  CHECK(amp.set_parameter(17, 1.0f));
  CHECK_FALSE(amp.set_parameter(17, 0.5f));
  CHECK(amp.amp_config().cab);
}

TEST_CASE("editing cab and cab_model does not allocate on the audio thread", "[amp-cab-models]") {
  sat::AmpSimConfig config;
  config.mic_model = sat::MicModel::kDynamic;
  config.mic_blend = 0.5f;
  // Start with the cab off: switching it on must find its mic delay lines already sized.
  config.cab = false;
  sat::AmpSim amp(config);
  amp.prepare(kRate, kBlock);
  std::array<float, kBlock> buffer{};
  buffer.fill(0.1f);
  float* channels[] = {buffer.data()};
  {
    sonare::test::AllocationGuard guard;
    for (int step = 0; step < 8; ++step) {
      amp.set_parameter(17, step % 2 == 0 ? 1.0f : 0.0f);
      amp.set_parameter(18, static_cast<float>(step % 4));
      amp.process(channels, 1, kBlock);
    }
    amp.set_parameter(17, 1.0f);
    amp.process(channels, 1, kBlock);
    REQUIRE(guard.count() == 0);
  }
  for (float v : buffer) REQUIRE(std::isfinite(v));
}

TEST_CASE("a cab_model edit is accepted and ignored while a cab IR is loaded", "[amp-cab-models]") {
  sat::AmpSim amp;
  const std::vector<float> ir = {1.0f, 0.5f, 0.25f};
  amp.load_cab_ir(ir);
  amp.prepare(kRate, kBlock);
  REQUIRE(amp.has_cab_ir());
  CHECK(amp.set_parameter(18, 2.0f));
  CHECK(amp.amp_config().cab_model == CabModel::kGuitar4x12);
  // Range checking still applies.
  CHECK_FALSE(amp.set_parameter(18, 9.0f));
}

TEST_CASE("switching cab_model changes the render and switching back restores it",
          "[amp-cab-models]") {
  sat::AmpSim amp;
  amp.prepare(kRate, kBlock);
  const std::vector<float> base = render(amp, 4);
  amp.reset();
  REQUIRE(amp.set_parameter(18, 2.0f));
  const std::vector<float> combo = render(amp, 4);
  CHECK(combo != base);
  amp.reset();
  REQUIRE(amp.set_parameter(18, 0.0f));
  CHECK(render(amp, 4) == base);
}
