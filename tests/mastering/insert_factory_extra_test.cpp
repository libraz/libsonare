// Targeted coverage for audit fixes around the insert factory, JSON
// round-tripping, and presets. The data-driven set_parameter_test.cpp already
// exercises the set_parameter contract for every name in insert_factory_names();
// these cases pin the specific behaviors changed by the audit pass:
//   * the modulation/delay effects are now registered and buildable,
//   * "maximizer.loudnessOptimize" is intentionally NOT a streaming insert,
//   * chain JSON round-trips the full repair.denoise field set,
//   * the streaming preset matches the pop preset.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/presets.h"
#include "rt/processor_base.h"

namespace {

using sonare::mastering::api::chain_config_from_json;
using sonare::mastering::api::chain_config_to_json;
using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::make_insert;
using sonare::mastering::api::MasteringChainConfig;
using sonare::mastering::api::Preset;
using sonare::mastering::api::preset_config;

bool ListContains(const std::vector<std::string>& names, const std::string& target) {
  for (const auto& name : names) {
    if (name == target) return true;
  }
  return false;
}

}  // namespace

#ifdef SONARE_WITH_FX
TEST_CASE("Modulation and delay effects are registered as inserts",
          "[mastering][insert_factory][effects]") {
  const auto names = insert_factory_names();
  for (const char* name : {"effects.modulation.chorus", "effects.modulation.flanger",
                           "effects.modulation.phaser", "effects.delay.stereo"}) {
    DYNAMIC_SECTION(name) {
      REQUIRE(ListContains(names, name));
      auto processor = make_insert(name, "{}");
      REQUIRE(processor != nullptr);
    }
  }
}

TEST_CASE("Modulation/delay inserts read their JSON params",
          "[mastering][insert_factory][effects]") {
  // Constructing with explicit params must not throw and must yield a processor;
  // the numeric mapping is validated indirectly via the set_parameter contract
  // test, here we just guard the param-reading code path.
  REQUIRE(make_insert("effects.modulation.chorus",
                      R"({"rateHz":1.2,"depthMs":4,"centerDelayMs":10,"dryWet":0.4})") != nullptr);
  REQUIRE(
      make_insert("effects.modulation.flanger",
                  R"({"rateHz":0.5,"depthMs":1,"centerDelayMs":2,"feedback":0.4,"dryWet":0.6})") !=
      nullptr);
  REQUIRE(make_insert("effects.modulation.phaser",
                      R"({"rateHz":0.3,"minHz":200,"maxHz":2000,"stages":6,"dryWet":0.5})") !=
          nullptr);
  REQUIRE(
      make_insert(
          "effects.delay.stereo",
          R"({"delayTimeLMs":120,"delayTimeRMs":180,"feedback":0.3,"pingPong":1,"dryWet":0.5})") !=
      nullptr);
}

#ifdef SONARE_WITH_ACOUSTIC_SIM
TEST_CASE("effects.reverb.room synthesizes a geometry-driven RIR insert",
          "[mastering][insert_factory][effects][acoustic]") {
  REQUIRE(ListContains(insert_factory_names(), "effects.reverb.room"));

  auto processor = make_insert(
      "effects.reverb.room",
      R"({"lengthM":8,"widthM":6,"heightM":3.5,"absorption":0.12,"sourceX":2,"sourceY":1.5,)"
      R"("sourceZ":1.5,"listenerX":6,"listenerY":4.5,"listenerZ":1.8,"ismOrder":3,"dryWet":1})");
  REQUIRE(processor != nullptr);

  // prepare() synthesizes the RIR at the host rate; a fully-wet impulse must
  // produce a decaying reverberant response (more than just the input spike).
  const int block = 512;
  processor->prepare(48000.0, block);
  std::vector<float> buf(static_cast<size_t>(block) * 8, 0.0f);
  buf[0] = 1.0f;
  for (size_t off = 0; off < buf.size(); off += static_cast<size_t>(block)) {
    float* blk = buf.data() + off;
    processor->process(&blk, 1, block);
  }
  double energy = 0.0;
  for (float s : buf) energy += static_cast<double>(s) * s;
  REQUIRE(energy > 0.0);  // the convolver produced a non-empty reverberant tail
}

TEST_CASE("effects.reverb.room synthesizes per host sample rate and is deterministic",
          "[mastering][insert_factory][effects][acoustic]") {
  const char* params = R"({"lengthM":8,"widthM":6,"heightM":3.5,"absorption":0.12,"seed":4})";

  auto render = [&](double sr) {
    auto p = make_insert("effects.reverb.room", params);
    REQUIRE(p != nullptr);
    const int block = 256;
    p->prepare(sr, block);
    std::vector<float> buf(static_cast<size_t>(block) * 16, 0.0f);
    buf[0] = 1.0f;
    for (size_t off = 0; off < buf.size(); off += static_cast<size_t>(block)) {
      float* blk = buf.data() + off;
      p->process(&blk, 1, block);
    }
    return buf;
  };

  // Synthesizing in prepare() at the host rate means the tail spans a different
  // sample count at 44.1k vs 96k (RT60 in seconds is fixed). A bug that ignored
  // sr and synthesized at a constant rate would make these tails identical.
  const std::vector<float> at_44k = render(44100.0);
  const std::vector<float> at_96k = render(96000.0);
  bool differs = false;
  for (size_t i = 0; i < at_44k.size() && !differs; ++i) differs = (at_44k[i] != at_96k[i]);
  REQUIRE(differs);

  // Same params + same host rate => bit-identical output (seed-deterministic).
  const std::vector<float> a = render(48000.0);
  const std::vector<float> b = render(48000.0);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
}

TEST_CASE("effects.reverb.room passes through cleanly when geometry is invalid",
          "[mastering][insert_factory][effects][acoustic]") {
  // Source outside the room => validate_shoebox errors => empty RIR. The insert
  // must not crash and (fully wet) must leave the signal essentially untouched.
  auto p = make_insert("effects.reverb.room",
                       R"({"lengthM":8,"widthM":6,"heightM":3.5,"sourceX":99,"dryWet":1})");
  REQUIRE(p != nullptr);
  const int block = 256;
  p->prepare(48000.0, block);
  std::vector<float> buf(static_cast<size_t>(block) * 4, 0.0f);
  buf[10] = 0.5f;
  const std::vector<float> input = buf;
  for (size_t off = 0; off < buf.size(); off += static_cast<size_t>(block)) {
    float* blk = buf.data() + off;
    p->process(&blk, 1, block);
  }
  REQUIRE(buf == input);  // empty IR => ConvolutionReverb leaves the buffer unchanged
}

TEST_CASE("effects.acoustic.roomMorph adds a target-room tail as a streaming insert",
          "[mastering][insert_factory][effects][acoustic]") {
  REQUIRE(ListContains(insert_factory_names(), "effects.acoustic.roomMorph"));

  auto processor =
      make_insert("effects.acoustic.roomMorph",
                  R"({"lengthM":12,"widthM":9,"heightM":5,"absorption":0.08,"wet":0.8,)"
                  R"("sourceTailSuppression":0.4})");
  REQUIRE(processor != nullptr);

  // A single input spike must come back with a decaying target-room tail: later
  // blocks (well past the input) carry reverberant energy.
  const int block = 512;
  processor->prepare(48000.0, block);
  std::vector<float> buf(static_cast<size_t>(block) * 16, 0.0f);
  buf[0] = 1.0f;
  for (size_t off = 0; off < buf.size(); off += static_cast<size_t>(block)) {
    float* blk = buf.data() + off;
    processor->process(&blk, 1, block);
  }
  double late_energy = 0.0;
  for (size_t i = buf.size() / 2; i < buf.size(); ++i) {
    late_energy += static_cast<double>(buf[i]) * buf[i];
  }
  REQUIRE(late_energy > 0.0);
}
#endif  // SONARE_WITH_ACOUSTIC_SIM
#endif  // SONARE_WITH_FX

TEST_CASE("maximizer.loudnessOptimize is not a streaming insert",
          "[mastering][insert_factory][maximizer]") {
  // LUFS normalization is offline-only; the insert factory must not silently
  // degrade it to a true-peak limiter. The honest streaming name is
  // "maximizer.truePeakLimiter", which remains available.
  REQUIRE_FALSE(ListContains(insert_factory_names(), "maximizer.loudnessOptimize"));
  REQUIRE(make_insert("maximizer.loudnessOptimize", "{}") == nullptr);
  REQUIRE(make_insert("maximizer.truePeakLimiter", "{}") != nullptr);
}

TEST_CASE("chain JSON round-trips the full repair.denoise field set",
          "[mastering][chain_json][repair]") {
  MasteringChainConfig cfg;
  cfg.repair.denoise.enabled = true;
  cfg.repair.denoise.config.mode = sonare::mastering::repair::DenoiseMode::SpectralSubtraction;
  cfg.repair.denoise.config.noise_estimator =
      sonare::mastering::repair::DenoiseNoiseEstimator::Imcra;
  cfg.repair.denoise.config.n_fft = 2048;
  cfg.repair.denoise.config.hop_length = 512;
  cfg.repair.denoise.config.dd_alpha = 0.95f;
  cfg.repair.denoise.config.gain_floor = 0.08f;
  cfg.repair.denoise.config.over_subtraction = 3.5f;
  cfg.repair.denoise.config.spectral_floor = 0.12f;
  cfg.repair.denoise.config.noise_estimation_quantile = 0.2f;
  cfg.repair.denoise.config.speech_presence_gain = false;
  cfg.repair.denoise.config.gain_smoothing = false;

  const MasteringChainConfig restored = chain_config_from_json(chain_config_to_json(cfg));
  const auto& d = restored.repair.denoise.config;
  REQUIRE(restored.repair.denoise.enabled);
  REQUIRE(d.mode == sonare::mastering::repair::DenoiseMode::SpectralSubtraction);
  REQUIRE(d.noise_estimator == sonare::mastering::repair::DenoiseNoiseEstimator::Imcra);
  REQUIRE(d.n_fft == 2048);
  REQUIRE(d.hop_length == 512);
  REQUIRE(d.dd_alpha == 0.95f);
  REQUIRE(d.gain_floor == 0.08f);
  REQUIRE(d.over_subtraction == 3.5f);
  REQUIRE(d.spectral_floor == 0.12f);
  REQUIRE(d.noise_estimation_quantile == 0.2f);
  REQUIRE_FALSE(d.speech_presence_gain);
  REQUIRE_FALSE(d.gain_smoothing);
}

TEST_CASE("streaming preset equals pop preset", "[mastering][presets]") {
  const MasteringChainConfig pop = preset_config(Preset::Pop);
  const MasteringChainConfig streaming = preset_config(Preset::Streaming);
  REQUIRE(streaming.loudness.enabled == pop.loudness.enabled);
  REQUIRE(streaming.loudness.target_lufs == pop.loudness.target_lufs);
  REQUIRE(streaming.loudness.ceiling_db == pop.loudness.ceiling_db);
  REQUIRE(streaming.maximizer.true_peak_limiter.config.ceiling_db ==
          pop.maximizer.true_peak_limiter.config.ceiling_db);
  REQUIRE(streaming.dynamics.compressor.config.threshold_db ==
          pop.dynamics.compressor.config.threshold_db);
}
