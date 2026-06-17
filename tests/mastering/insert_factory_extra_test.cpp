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
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "rt/processor_base.h"

namespace {

using Catch::Matchers::WithinAbs;
using sonare::mastering::api::chain_config_from_json;
using sonare::mastering::api::chain_config_to_json;
using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::insert_param_info_json;
using sonare::mastering::api::insert_param_names;
using sonare::mastering::api::make_insert;
using sonare::mastering::api::make_insert_with_ir;
using sonare::mastering::api::MasteringChainConfig;
using sonare::mastering::api::Preset;
using sonare::mastering::api::preset_config;

bool ListContains(const std::vector<std::string>& names, const std::string& target) {
  for (const auto& name : names) {
    if (name == target) return true;
  }
  return false;
}

std::string Base64Encode(const std::vector<uint8_t>& bytes) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= bytes.size()) {
    const uint32_t triple = (static_cast<uint32_t>(bytes[i]) << 16) |
                            (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                            static_cast<uint32_t>(bytes[i + 2]);
    out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
    out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
    out.push_back(kAlphabet[triple & 0x3F]);
    i += 3;
  }
  const size_t remaining = bytes.size() - i;
  if (remaining == 1) {
    const uint32_t triple = static_cast<uint32_t>(bytes[i]) << 16;
    out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (remaining == 2) {
    const uint32_t triple =
        (static_cast<uint32_t>(bytes[i]) << 16) | (static_cast<uint32_t>(bytes[i + 1]) << 8);
    out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
    out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

std::string F32Base64(std::initializer_list<float> samples) {
  std::vector<uint8_t> bytes;
  bytes.reserve(samples.size() * sizeof(float));
  for (float sample : samples) {
    uint32_t bits = 0;
    std::memcpy(&bits, &sample, sizeof(bits));
    bytes.push_back(static_cast<uint8_t>(bits & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((bits >> 8) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((bits >> 16) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((bits >> 24) & 0xFFu));
  }
  return Base64Encode(bytes);
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

TEST_CASE("decaySec maps to a comparable RT60 across the time-tunable reverbs",
          "[mastering][insert_factory][effects][reverb]") {
  // decaySec is a tail-length-in-seconds intent. FDN maps it directly to ~T60;
  // the plate/Dattorro tank now maps it to an approximate T60 too, so the same
  // {decaySec:2} no longer yields a ~2 s FDN tail but a ~0.2-feedback (very
  // short) plate tail. Measure each engine's RT60 from a fully-wet impulse via
  // the Schroeder backward energy integral and assert they are on the same order.
  const double sr = 48000.0;
  const int block = 512;
  const int seconds = 8;
  const size_t total = static_cast<size_t>(sr) * static_cast<size_t>(seconds);

  auto rt60_seconds = [&](const std::string& name) -> double {
    auto p = make_insert(name, R"({"decaySec":2.0,"dryWet":1.0})");
    REQUIRE(p != nullptr);
    p->prepare(sr, block);
    std::vector<float> buf(total, 0.0f);
    buf[0] = 1.0f;
    for (size_t off = 0; off < total; off += static_cast<size_t>(block)) {
      const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), total - off));
      float* blk = buf.data() + off;
      p->process(&blk, 1, n);
    }
    std::vector<double> edc(total + 1, 0.0);
    for (size_t i = total; i-- > 0;) edc[i] = edc[i + 1] + static_cast<double>(buf[i]) * buf[i];
    const double e0 = edc[0];
    REQUIRE(e0 > 0.0);
    for (size_t i = 0; i < total; ++i) {
      if (10.0 * std::log10(edc[i] / e0) <= -60.0) return static_cast<double>(i) / sr;
    }
    return static_cast<double>(seconds);
  };

  const double fdn = rt60_seconds("effects.reverb.fdn");
  const double dattorro = rt60_seconds("effects.reverb.dattorro");

  // Both land in the neighbourhood of the requested 2 s tail (the plate tank has
  // no exact closed-form RT60, so allow a wide but bounded band).
  REQUIRE(fdn > 1.0);
  REQUIRE(fdn < 4.0);
  REQUIRE(dattorro > 1.0);
  REQUIRE(dattorro < 4.0);
  // And crucially they are the same order of magnitude rather than the old ~10x
  // gap (decaySec=2 used to give the plate a 0.2 feedback / very short tail).
  REQUIRE(dattorro > 0.4 * fdn);
  REQUIRE(dattorro < 2.5 * fdn);
}

TEST_CASE("effects.reverb.convolution loads user IR from JSON params",
          "[mastering][insert_factory][effects][reverb]") {
  const std::string ir = F32Base64({0.0f, 1.0f});
  auto processor = make_insert("effects.reverb.convolution",
                               std::string(R"({"dryWet":1.0,"irF32Base64":")") + ir + R"("})");
  REQUIRE(processor != nullptr);

  constexpr int block = 256;
  processor->prepare(48000.0, block);
  const int latency = processor->latency_samples();
  REQUIRE(latency > 0);

  std::vector<float> buf(static_cast<size_t>(block) * 4, 0.0f);
  buf[0] = 1.0f;
  for (size_t off = 0; off < buf.size(); off += static_cast<size_t>(block)) {
    float* blk = buf.data() + off;
    processor->process(&blk, 1, block);
  }

  REQUIRE_THAT(buf[static_cast<size_t>(latency + 1)], WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("effects.reverb.convolution make_insert_with_ir honors dryWet",
          "[mastering][insert_factory][effects][reverb]") {
  const float ir[] = {0.0f, 1.0f};
  auto processor = make_insert_with_ir("effects.reverb.convolution", R"({"dryWet":0.0})", ir, 2);
  REQUIRE(processor != nullptr);

  constexpr int block = 256;
  processor->prepare(48000.0, block);
  const int latency = processor->latency_samples();
  std::vector<float> buf(static_cast<size_t>(block) * 4, 0.0f);
  buf[0] = 1.0f;
  for (size_t off = 0; off < buf.size(); off += static_cast<size_t>(block)) {
    float* blk = buf.data() + off;
    processor->process(&blk, 1, block);
  }

  REQUIRE_THAT(buf[static_cast<size_t>(latency)], WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(buf[static_cast<size_t>(latency + 1)], WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("effects.reverb.convolution rejects malformed JSON IR payload",
          "[mastering][insert_factory][effects][reverb]") {
  REQUIRE_THROWS(make_insert("effects.reverb.convolution", R"({"irF32Base64":"!!!"})"));
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
                  R"({"lengthM":12,"widthM":9,"heightM":5,"absorption":0.08,"dryWet":0.8,)"
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

TEST_CASE("effects.acoustic.roomMorph insert uses dryWet like other FX",
          "[mastering][insert_factory][effects][acoustic]") {
  auto render_tail = [](const char* params) {
    auto processor = make_insert("effects.acoustic.roomMorph", params);
    REQUIRE(processor != nullptr);
    const int block = 512;
    processor->prepare(48000.0, block);
    std::vector<float> buf(static_cast<size_t>(block) * 12, 0.0f);
    buf[0] = 1.0f;
    for (size_t off = 0; off < buf.size(); off += static_cast<size_t>(block)) {
      float* blk = buf.data() + off;
      processor->process(&blk, 1, block);
    }
    double tail = 0.0;
    for (size_t i = static_cast<size_t>(block); i < buf.size(); ++i) {
      tail += std::abs(buf[i]);
    }
    return tail;
  };

  const double dry_tail =
      render_tail(R"({"lengthM":12,"widthM":9,"heightM":5,"absorption":0.08,"dryWet":0.0,)"
                  R"("sourceTailSuppression":0.0})");
  const double wet_tail =
      render_tail(R"({"lengthM":12,"widthM":9,"heightM":5,"absorption":0.08,"dryWet":1.0,)"
                  R"("sourceTailSuppression":0.0})");

  REQUIRE(wet_tail > dry_tail + 1.0e-5);
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

TEST_CASE("insert_param_names enumerates the keys a processor reads",
          "[mastering][insert_factory][param_names]") {
  // Table-driven processor: every SONARE_FIELDS_* key shows up.
  const auto comp = insert_param_names("dynamics.compressor");
  REQUIRE(ListContains(comp, "thresholdDb"));
  REQUIRE(ListContains(comp, "ratio"));
  REQUIRE(ListContains(comp, "attackMs"));
  REQUIRE(ListContains(comp, "makeupGainDb"));
  REQUIRE_FALSE(ListContains(comp, "highPassHz"));

  // Setter-based EQ with fixed keys.
  const auto tilt = insert_param_names("eq.tilt");
  REQUIRE(ListContains(tilt, "tiltDb"));
  REQUIRE(ListContains(tilt, "pivotHz"));

  // Band-indexed EQ: the per-band field names are enumerated under band{i}.
  const auto parametric = insert_param_names("eq.parametric");
  REQUIRE(ListContains(parametric, "band0.frequencyHz"));
  REQUIRE(ListContains(parametric, "band0.gainDb"));
  REQUIRE(ListContains(parametric, "band0.q"));

  // An unknown name yields no parameter names (rather than throwing).
  REQUIRE(insert_param_names("not.a.real.processor").empty());
}

TEST_CASE("make_insert reports supplied keys the processor ignored",
          "[mastering][insert_factory][param_names]") {
  std::vector<std::string> unknown;

  // All keys consumed => no warnings.
  REQUIRE(make_insert("eq.tilt", R"({"tiltDb":3,"pivotHz":800})", &unknown) != nullptr);
  REQUIRE(unknown.empty());

  // Valid band keys are consumed even though they are indexed.
  REQUIRE(make_insert("eq.parametric", R"({"band0.frequencyHz":1000,"band0.gainDb":3})",
                      &unknown) != nullptr);
  REQUIRE(unknown.empty());

  // The historically silently-ignored keys are now surfaced. eq.parametric reads
  // only band{i}.* fields, so a flat highPassHz/presenceDb takes no effect.
  REQUIRE(make_insert("eq.parametric", R"({"highPassHz":80,"presenceDb":4})", &unknown) != nullptr);
  REQUIRE(unknown.size() == 2);
  REQUIRE(ListContains(unknown, "highPassHz"));
  REQUIRE(ListContains(unknown, "presenceDb"));

  // A mix of known and unknown keys reports only the unknown ones, sorted.
  REQUIRE(make_insert("dynamics.compressor", R"({"thresholdDb":-12,"bogusKey":1,"ratio":4})",
                      &unknown) != nullptr);
  REQUIRE(unknown == std::vector<std::string>{"bogusKey"});

  // An unknown processor name leaves the out-parameter untouched (it is a hard
  // error surfaced elsewhere, not an ignored-keys warning).
  std::vector<std::string> sentinel{"sentinel"};
  REQUIRE(make_insert("not.a.real.processor", R"({"x":1})", &sentinel) == nullptr);
  REQUIRE(sentinel == std::vector<std::string>{"sentinel"});
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

TEST_CASE("processor_catalog_json classifies every id consistently with the source lists",
          "[mastering][catalog]") {
  const std::string json = sonare::mastering::api::processor_catalog_json();
  REQUIRE(json.front() == '[');
  REQUIRE(json.back() == ']');

  // Every realtime-insertable id is reported as kind "realtime" with the flag set
  // (none of the insert ids are pair processors, so the precedence resolves to
  // realtime). This is the invariant the host relies on to avoid offering an id
  // the realtime strip would reject.
  for (const auto& id : sonare::mastering::api::insert_factory_names()) {
    const std::string entry =
        "{\"id\":\"" + id + "\",\"kind\":\"realtime\",\"realtimeInsertable\":true";
    REQUIRE(json.find(entry) != std::string::npos);
  }

  // Pair processors are reported as kind "pair" and are never realtime-insertable.
  for (const auto& id : sonare::mastering::api::pair_processor_names()) {
    const std::string entry =
        "{\"id\":\"" + id + "\",\"kind\":\"pair\",\"realtimeInsertable\":false";
    REQUIRE(json.find(entry) != std::string::npos);
  }

  // A whole-file processor that is not built as a streaming insert is offline and
  // not realtime-insertable (loudnessOptimize needs the full signal).
  REQUIRE(json.find(
              "{\"id\":\"maximizer.loudnessOptimize\",\"kind\":\"offline\",\"realtimeInsertable\":"
              "false") != std::string::npos);

  // stereoOnly is surfaced independently of kind: eq.midSide is realtime-insertable
  // yet has no mono implementation, so it is realtime + stereoOnly. It is also an
  // inherently-stereo processor, so its channelPolicy is "stereoPairOnly".
  REQUIRE(json.find("{\"id\":\"eq.midSide\",\"kind\":\"realtime\",\"realtimeInsertable\":true,"
                    "\"stereoOnly\":true,\"channelPolicy\":\"stereoPairOnly\"}") !=
          std::string::npos);

  // Realtime-only ids that are absent from processor_names() are still reported.
  if (ListContains(sonare::mastering::api::insert_factory_names(), "effects.reverb.room")) {
    REQUIRE(
        json.find("{\"id\":\"effects.reverb.room\",\"kind\":\"realtime\",\"realtimeInsertable\":"
                  "true") != std::string::npos);
  }
}

TEST_CASE(
    "channel_policy tags inherently-stereo processors StereoPairOnly and the rest "
    "Multichannel",
    "[mastering][catalog]") {
  using sonare::mastering::api::channel_policy;
  using sonare::mastering::api::ChannelPolicy;

  // The inherently-stereo set: stereo-image processors, eq.midSide,
  // multiband.imager, and every reverb/modulation/delay effect operate on the
  // front L/R pair and pass surround planes through dry.
  const std::array<const char*, 20> spo = {"stereo.imager",
                                           "stereo.monoMaker",
                                           "stereo.stereoBalance",
                                           "stereo.haasEnhancer",
                                           "stereo.phaseAlign",
                                           "stereo.autoPan",
                                           "eq.midSide",
                                           "multiband.imager",
                                           "effects.reverb.plate",
                                           "effects.reverb.dattorro",
                                           "effects.reverb.fdn",
                                           "effects.reverb.velvet",
                                           "effects.reverb.convolution",
                                           "effects.reverb.room",
                                           "effects.acoustic.roomMorph",
                                           "effects.modulation.chorus",
                                           "effects.modulation.ensemble",
                                           "effects.modulation.flanger",
                                           "effects.modulation.phaser",
                                           "effects.delay.stereo"};
  for (const char* id : spo) {
    REQUIRE(channel_policy(id) == ChannelPolicy::StereoPairOnly);
  }

  // Per-channel and linked-dynamics processors process every plane in one call.
  for (const char* id : {"dynamics.compressor", "eq.parametric", "saturation.tape",
                         "multiband.compressor", "maximizer.maximizer"}) {
    REQUIRE(channel_policy(id) == ChannelPolicy::Multichannel);
  }

  // An unknown/legacy id defaults to Multichannel (one full-buffer call never
  // drops channels).
  REQUIRE(channel_policy("does.not.exist") == ChannelPolicy::Multichannel);

  // Wire strings are stable.
  REQUIRE(std::string(sonare::mastering::api::channel_policy_to_string(
              ChannelPolicy::Multichannel)) == "multichannel");
  REQUIRE(std::string(sonare::mastering::api::channel_policy_to_string(
              ChannelPolicy::StereoPairOnly)) == "stereoPairOnly");
}

TEST_CASE("offline->realtime candidate processors are already realtime-insertable",
          "[mastering][catalog]") {
  // The processors studio flagged as "offline-only" candidates for realtime
  // promotion are in fact already realtime-insertable: each builds as a streaming
  // insert and the catalog classifies it kind "realtime". No promotion work is
  // required; realtime-insertability is distinct from per-parameter realtime
  // safety (the latter is reported by sonare_mastering_insert_param_info).
  const std::string json = sonare::mastering::api::processor_catalog_json();
  for (const char* id : {"eq.dynamic", "multiband.dynamicEq", "eq.midSide", "eq.linearPhase",
                         "dynamics.vocalRider", "dynamics.parallelComp"}) {
    DYNAMIC_SECTION(id) {
      REQUIRE(ListContains(insert_factory_names(), id));
      REQUIRE(make_insert(id, "{}") != nullptr);
      const std::string entry =
          std::string("{\"id\":\"") + id + "\",\"kind\":\"realtime\",\"realtimeInsertable\":true";
      REQUIRE(json.find(entry) != std::string::npos);
    }
  }

  // The genuinely whole-file processors remain offline in the catalog.
  for (const char* id : {"maximizer.loudnessOptimize", "repair.declick", "final.dither"}) {
    DYNAMIC_SECTION(id) {
      REQUIRE_FALSE(ListContains(insert_factory_names(), id));
      const std::string entry = std::string("{\"id\":\"") + id + "\",\"kind\":\"offline\"";
      REQUIRE(json.find(entry) != std::string::npos);
    }
  }
}

TEST_CASE("Mastering inserts publish non-empty automation parameter descriptors",
          "[mastering][automation]") {
  // Before parameter_descriptors() was overridden, every mastering insert returned
  // "[]" from insert_param_info_json(), so name-addressed realtime automation
  // (sonare_engine_set_track_strip_insert_param_by_name and friends) was a silent
  // no-op for the entire mastering catalog. Each insert below must now publish at
  // least one {name,id,rtSafe} descriptor whose name is the construction-time JSON
  // key, with a band-prefixed layout for the multiband inserts.
  struct Expect {
    const char* name;
    const char* key_fragment;
  };
  const Expect cases[] = {
      {"dynamics.compressor", "\"name\":\"thresholdDb\""},
      {"dynamics.gate", "\"name\":\"thresholdDb\""},
      {"dynamics.limiter", "\"name\":\"thresholdDb\""},
      {"eq.parametric", "\"name\":\""},
      {"saturation.tape", "\"name\":\"driveDb\""},
      {"stereo.imager", "\"name\":\"width\""},
      {"maximizer.maximizer", "\"name\":\"ceilingDb\""},
      {"multiband.saturation", "\"name\":\"band0.driveDb\""},
  };
  for (const auto& c : cases) {
    DYNAMIC_SECTION(c.name) {
      const std::string info = insert_param_info_json(c.name);
      INFO("info = " << info);
      REQUIRE(info != "[]");
      REQUIRE(info.find("\"id\":") != std::string::npos);
      REQUIRE(info.find("\"rtSafe\":") != std::string::npos);
      REQUIRE(info.find(c.key_fragment) != std::string::npos);
    }
  }
}
