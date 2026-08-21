// Targeted regression coverage around the insert factory, JSON
// round-tripping, and presets. The data-driven set_parameter_test.cpp already
// exercises the set_parameter contract for every name in insert_factory_names();
// these cases pin the corresponding public behaviors:
//   * the modulation/delay effects are now registered and buildable,
//   * "maximizer.loudnessOptimize" is intentionally NOT a streaming insert,
//   * chain JSON round-trips the full repair.denoise field set,
//   * the streaming preset matches the pop preset.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "analysis/acoustic_analyzer.h"
#include "core/audio.h"
#include "mastering/api/chain.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/eq/dynamic_eq.h"
#include "rt/processor_base.h"
#include "util/exception.h"
#include "util/json.h"

#ifdef SONARE_WITH_FX
#include "effects/reverb/convolution_reverb.h"
#endif

#ifdef SONARE_WITH_ACOUSTIC_SIM
// AirAbsorption carries the ISO reference climate the acoustic ABI's zero
// sentinel resolves to; the tests read it instead of restating the literals.
#include "acoustic/late_reverb.h"
#endif

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

// The base64 helpers exist to feed the convolution reverb an inline IR. Their
// only call site sits inside the room-simulation block further down, so they
// carry both of that site's conditions -- ungated they would have no callers
// whenever either feature is off, which the build rejects as unused functions.
#if defined(SONARE_WITH_FX) && defined(SONARE_WITH_ACOUSTIC_SIM)

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

#endif  // SONARE_WITH_FX && SONARE_WITH_ACOUSTIC_SIM

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

TEST_CASE("acoustic room inserts reject invalid geometry as InvalidParameter",
          "[mastering][insert_factory][effects][acoustic]") {
  // Both geometry-driven inserts must reject the same invalid source placement
  // at construction, instead of RoomReverb silently becoming a dry passthrough.
  for (const char* name : {"effects.reverb.room", "effects.acoustic.roomMorph"}) {
    DYNAMIC_SECTION(name) {
      try {
        auto processor =
            make_insert(name, R"({"lengthM":8,"widthM":6,"heightM":3.5,"sourceX":99,"dryWet":1})");
        (void)processor;
        FAIL("invalid geometry was accepted");
      } catch (const sonare::SonareException& error) {
        REQUIRE(error.code() == sonare::ErrorCode::InvalidParameter);
      }
    }
  }
}

TEST_CASE("acoustic room inserts reject an invalid RIR length cap and air climate",
          "[mastering][insert_factory][effects][acoustic][numeric]") {
  // The geometry is only half the synthesis configuration: an out-of-range
  // maxSeconds or a non-physical air climate also makes RIR synthesis fail,
  // which yields an empty IR and therefore a silently inert (dry passthrough)
  // insert. Both inserts must refuse them at construction instead.
  const char* room = R"({"lengthM":8,"widthM":6,"heightM":3.5,"dryWet":1,)";
  for (const char* tail : {R"("maxSeconds":-1})", R"("maxSeconds":1000})",
                           R"("airAbsorptionEnabled":true,"airTemperatureC":-500})",
                           R"("airAbsorptionEnabled":true,"airHumidityPercent":150})"}) {
    for (const char* name : {"effects.reverb.room", "effects.acoustic.roomMorph"}) {
      const std::string params = std::string(room) + tail;
      DYNAMIC_SECTION(name << " " << tail) {
        try {
          auto processor = make_insert(name, params);
          (void)processor;
          FAIL("an unsynthesizable RIR configuration was accepted");
        } catch (const sonare::SonareException& error) {
          REQUIRE(error.code() == sonare::ErrorCode::InvalidParameter);
        }
      }
    }
  }
}

TEST_CASE("an accepted effects.reverb.room insert is not a dry passthrough",
          "[mastering][insert_factory][effects][acoustic]") {
  // The counterpart of the rejection cases: a configuration the factory accepts
  // must actually convolve. A dry passthrough (empty IR) would leave the impulse
  // exactly where it was written.
  auto processor =
      make_insert("effects.reverb.room",
                  R"({"lengthM":8,"widthM":6,"heightM":3.5,"maxSeconds":0.5,"dryWet":1})");
  REQUIRE(processor != nullptr);

  constexpr int block = 256;
  processor->prepare(48000.0, block);
  std::vector<float> buf(static_cast<size_t>(block) * 8, 0.0f);
  buf[0] = 1.0f;
  for (size_t off = 0; off < buf.size(); off += static_cast<size_t>(block)) {
    float* blk = buf.data() + off;
    processor->process(&blk, 1, block);
  }

  REQUIRE(buf[0] != 1.0f);
  double energy = 0.0;
  for (float sample : buf) energy += static_cast<double>(sample) * sample;
  REQUIRE(energy > 0.0);
}

TEST_CASE("effects.acoustic.roomMorph insert reaches acoustic facade options",
          "[mastering][insert_factory][effects][acoustic]") {
  const auto names = insert_param_names("effects.acoustic.roomMorph");
  for (const char* key : {"bandAbsorption", "bandScattering", "materialPreset", "preferEyring",
                          "mixingTimeMs", "crossfadeMs"}) {
    DYNAMIC_SECTION(key) { REQUIRE(ListContains(names, key)); }
  }

  const auto render = [](const char* params) {
    auto processor = make_insert("effects.acoustic.roomMorph", params);
    REQUIRE(processor != nullptr);
    constexpr int block = 512;
    std::vector<float> buf(static_cast<size_t>(block) * 24, 0.0f);
    buf[0] = 1.0f;
    processor->prepare(48000.0, block);
    for (size_t off = 0; off < buf.size(); off += static_cast<size_t>(block)) {
      float* blk = buf.data() + off;
      processor->process(&blk, 1, block);
    }
    return buf;
  };
  const auto differs = [](const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return true;
    for (size_t i = 0; i < a.size(); ++i) {
      if (std::abs(a[i] - b[i]) > 1e-7f) return true;
    }
    return false;
  };

  constexpr const char* base =
      R"({"lengthM":12,"widthM":9,"heightM":5,"absorption":0.4,"dryWet":1,"sourceTailSuppression":0,"maxSeconds":0.3,"seed":7})";
  const std::vector<float> baseline = render(base);

  REQUIRE(differs(
      baseline,
      render(
          R"({"lengthM":12,"widthM":9,"heightM":5,"absorption":0.4,"dryWet":1,"sourceTailSuppression":0,"maxSeconds":0.3,"seed":7,"preferEyring":false})")));
  REQUIRE(differs(
      baseline,
      render(
          R"({"lengthM":12,"widthM":9,"heightM":5,"absorption":0.4,"dryWet":1,"sourceTailSuppression":0,"maxSeconds":0.3,"seed":7,"mixingTimeMs":60})")));
  REQUIRE(differs(
      baseline,
      render(
          R"({"lengthM":12,"widthM":9,"heightM":5,"absorption":0.4,"dryWet":1,"sourceTailSuppression":0,"maxSeconds":0.3,"seed":7,"crossfadeMs":35})")));
  REQUIRE(differs(
      baseline,
      render(
          R"({"lengthM":12,"widthM":9,"heightM":5,"dryWet":1,"sourceTailSuppression":0,"maxSeconds":0.3,"seed":7,"bandAbsorption":[0.75,0.75,0.75,0.75,0.75,0.75]})")));
  REQUIRE(differs(
      baseline,
      render(
          R"({"lengthM":12,"widthM":9,"heightM":5,"dryWet":1,"sourceTailSuppression":0,"maxSeconds":0.3,"seed":7,"bandAbsorption":[0.4,0.4,0.4,0.4,0.4,0.4],"bandScattering":[0.8,0.8,0.8,0.8,0.8,0.8]})")));
  REQUIRE(differs(
      baseline,
      render(
          R"({"lengthM":12,"widthM":9,"heightM":5,"dryWet":1,"sourceTailSuppression":0,"maxSeconds":0.3,"seed":7,"materialPreset":3})")));
}

TEST_CASE("effects.reverb.room and effects.acoustic.roomMorph reach air absorption options",
          "[mastering][insert_factory][effects][acoustic]") {
  for (const char* name : {"effects.reverb.room", "effects.acoustic.roomMorph"}) {
    CAPTURE(name);
    const auto names = insert_param_names(name);
    REQUIRE(ListContains(names, "airAbsorptionEnabled"));
    REQUIRE(ListContains(names, "airTemperatureC"));
    REQUIRE(ListContains(names, "airHumidityPercent"));
  }
}

TEST_CASE("acoustic inserts resolve a zero air climate to the ISO reference climate",
          "[mastering][insert_factory][effects][acoustic][numeric]") {
  // INVARIANT (a): airTemperatureC / airHumidityPercent follow the acoustic
  // ABI's "0 selects the library value" rule on the streaming inserts too, so
  // the same option bag describes the same room here as on the offline facade.
  // The expected climate is read from the library default rather than written
  // as a literal, which is exactly what the offline facade substitutes for 0.
  const sonare::acoustic::AirAbsorption iso{};
  constexpr int block = 256;
  const auto render = [](const std::string& name, const std::string& params) {
    auto processor = make_insert(name, params);
    REQUIRE(processor != nullptr);
    std::vector<float> buf(static_cast<size_t>(block) * 12, 0.0f);
    buf[0] = 1.0f;
    processor->prepare(48000.0, block);
    for (size_t off = 0; off < buf.size(); off += static_cast<size_t>(block)) {
      float* blk = buf.data() + off;
      processor->process(&blk, 1, block);
    }
    return buf;
  };
  const auto same = [](const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (a[i] != b[i]) return false;
    }
    return true;
  };

  constexpr const char* geometry =
      R"("lengthM":14,"widthM":10,"heightM":5,"absorption":0.15,"sourceX":2,"sourceY":2,)"
      R"("sourceZ":1.5,"listenerX":9,"listenerY":7,"listenerZ":1.7,"maxSeconds":0.4,"seed":5,)"
      R"("dryWet":1,"sourceTailSuppression":0,"airAbsorptionEnabled":1)";
  const std::string zero_climate =
      std::string("{") + geometry + R"(,"airTemperatureC":0,"airHumidityPercent":0})";
  const std::string iso_climate = std::string("{") + geometry + R"(,"airTemperatureC":)" +
                                  std::to_string(iso.temperature_c) + R"(,"airHumidityPercent":)" +
                                  std::to_string(iso.humidity_percent) + "}";
  // A humidity that is not the sentinel must reach the synthesizer, which keeps
  // the equality above from passing for the trivial reason that the insert
  // ignores the climate entirely.
  const std::string other_climate =
      std::string("{") + geometry + R"(,"airTemperatureC":0,"airHumidityPercent":1})";

  for (const char* name : {"effects.reverb.room", "effects.acoustic.roomMorph"}) {
    DYNAMIC_SECTION(name) {
      REQUIRE(same(render(name, zero_climate), render(name, iso_climate)));
      REQUIRE_FALSE(same(render(name, zero_climate), render(name, other_climate)));
    }
  }
}

TEST_CASE("effects.acoustic.roomMorph rejects a crossfade the caller cannot see ignored",
          "[mastering][insert_factory][effects][acoustic][numeric]") {
  // INVARIANT (b): a timing value is applied or rejected. Only the documented
  // sentinel resolves to the library default, so a negative crossfade cannot
  // reach the synthesizer as the default and report success.
  constexpr const char* geometry =
      R"("lengthM":12,"widthM":9,"heightM":5,"absorption":0.2,"dryWet":1,)"
      R"("sourceTailSuppression":0,"maxSeconds":0.3,"seed":7)";
  for (const char* crossfade :
       {R"("crossfadeMs":-5)", R"("crossfadeMs":-0.001)", R"("crossfadeMs":100000)"}) {
    DYNAMIC_SECTION(crossfade) {
      const std::string params = std::string("{") + geometry + "," + crossfade + "}";
      try {
        auto processor = make_insert("effects.acoustic.roomMorph", params);
        (void)processor;
        FAIL("an out-of-range crossfade was accepted");
      } catch (const sonare::SonareException& error) {
        REQUIRE(error.code() == sonare::ErrorCode::InvalidParameter);
      }
    }
  }

  // The sentinel keeps its meaning: 0 is the library default, not a request for
  // a zero-width crossfade, so it renders exactly like omitting the key.
  const auto render = [](const std::string& params) {
    auto processor = make_insert("effects.acoustic.roomMorph", params);
    REQUIRE(processor != nullptr);
    constexpr int block = 256;
    std::vector<float> buf(static_cast<size_t>(block) * 12, 0.0f);
    buf[0] = 1.0f;
    processor->prepare(48000.0, block);
    for (size_t off = 0; off < buf.size(); off += static_cast<size_t>(block)) {
      float* blk = buf.data() + off;
      processor->process(&blk, 1, block);
    }
    return buf;
  };
  REQUIRE(render(std::string("{") + geometry + "}") ==
          render(std::string("{") + geometry + R"(,"crossfadeMs":0})"));
}

TEST_CASE("effects.reverb.room air absorption shortens the round-tripped high-band RT60",
          "[.][slow][mastering][insert_factory][effects][acoustic]") {
  // Reachability proof: this drives the insert through make_insert()/JSON
  // params (the same surface every host uses to build a named processor), not
  // a directly-constructed RoomReverbConfig, and measures the rendered output
  // through the acoustic analyzer rather than asserting on the internal RIR.
  using sonare::AcousticConfig;
  using sonare::AcousticParameters;
  using sonare::analyze_impulse_response;
  using sonare::Audio;

  constexpr double kSampleRate = 48000.0;
  constexpr int kBlock = 512;
  constexpr int kSeconds = 6;
  const size_t total = static_cast<size_t>(kSampleRate) * static_cast<size_t>(kSeconds);

  // A large hall with moderate absorption: big enough for the ISO 9613-1 air
  // term to bias the high bands clearly, with an RT60 short enough that the
  // per-band decay analysis below stays inside its reliable operating range.
  const auto render = [&](const char* params) {
    auto processor = make_insert("effects.reverb.room", params);
    REQUIRE(processor != nullptr);
    processor->prepare(kSampleRate, kBlock);
    std::vector<float> buf(total, 0.0f);
    buf[0] = 1.0f;
    for (size_t off = 0; off < total; off += static_cast<size_t>(kBlock)) {
      const int n = static_cast<int>(std::min<size_t>(kBlock, total - off));
      float* blk = buf.data() + off;
      processor->process(&blk, 1, n);
    }
    AcousticConfig ac;
    ac.mode = AcousticConfig::Mode::ImpulseResponse;
    return analyze_impulse_response(
        Audio::from_vector(std::move(buf), static_cast<int>(kSampleRate)), ac);
  };

  constexpr const char* kDry =
      R"({"lengthM":30,"widthM":24,"heightM":15,"absorption":0.2,"sourceX":3,"sourceY":3,)"
      R"("sourceZ":1.5,"listenerX":10,"listenerY":8,"listenerZ":1.7,"ismOrder":3,"dryWet":1,)"
      R"("seed":3})";
  constexpr const char* kWet =
      R"({"lengthM":30,"widthM":24,"heightM":15,"absorption":0.2,"sourceX":3,"sourceY":3,)"
      R"("sourceZ":1.5,"listenerX":10,"listenerY":8,"listenerZ":1.7,"ismOrder":3,"dryWet":1,)"
      R"("seed":3,"airAbsorptionEnabled":1,"airTemperatureC":20,"airHumidityPercent":50})";

  const AcousticParameters dry = render(kDry);
  const AcousticParameters humid = render(kWet);
  REQUIRE(dry.rt60_bands.size() == humid.rt60_bands.size());
  REQUIRE(dry.rt60_bands.size() >= 6);
  for (float v : dry.rt60_bands) REQUIRE(v > 1.0f);
  for (float v : humid.rt60_bands) REQUIRE(v > 0.5f);

  // Non-vacuity: at the ISO reference climate (the default a caller gets by
  // only setting airAbsorptionEnabled) this is already a large, unmistakable
  // effect; no sweep was needed to find a defensible threshold. The bottom
  // band is barely touched, confirming the shortening is the frequency-
  // dependent air term reaching the insert and not a broadband regression.
  const float low_ratio = humid.rt60_bands.front() / dry.rt60_bands.front();
  const float high_ratio = humid.rt60_bands.back() / dry.rt60_bands.back();
  CAPTURE(dry.rt60_bands.front(), humid.rt60_bands.front(), dry.rt60_bands.back(),
          humid.rt60_bands.back());
  REQUIRE(high_ratio < 0.80f);  // top band RT60 shortens by at least 20%
  REQUIRE(low_ratio > 0.95f);   // bottom band RT60 shifts by less than 5%
  REQUIRE(high_ratio < low_ratio);
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

  const auto catalog = sonare::util::json::parse(json);
  REQUIRE(catalog.is_array());
#ifdef SONARE_WITH_FX
  int convolution_engine_inserts = 0;
#endif
  for (const auto& entry : catalog.as_array()) {
    REQUIRE(entry.contains("id"));
    REQUIRE(entry.contains("realtimeInsertable"));
    REQUIRE(entry.contains("latencySamples"));
    REQUIRE(entry.contains("tailSamples"));
    REQUIRE(entry.contains("realtimeCost"));
    REQUIRE(entry.contains("category"));
    REQUIRE(entry["category"].is_string());
    REQUIRE(entry.contains("params"));
    REQUIRE(entry["params"].is_array());
    for (const auto& param : entry["params"].as_array()) {
      REQUIRE(param.contains("name"));
      REQUIRE(param.contains("type"));
      REQUIRE(param.contains("min"));
      REQUIRE(param.contains("max"));
      REQUIRE(param.contains("default"));
      REQUIRE(param.contains("unit"));
    }
    const int latency = entry["latencySamples"].as_int();
    const int tail = entry["tailSamples"].as_int();
    REQUIRE(latency >= 0);
    REQUIRE(tail >= 0);

    if (entry["realtimeInsertable"].as_bool()) {
      REQUIRE(entry["realtimeCost"].is_string());
      const std::string cost = entry["realtimeCost"].as_string();
      REQUIRE((cost == "low" || cost == "moderate" || cost == "high"));
      auto processor = make_insert(entry["id"].as_string(), "{}");
      REQUIRE(processor != nullptr);
#ifdef SONARE_WITH_FX
      // Structural guard on the engine rather than on a list of ids: whichever
      // id wraps the partitioned-convolution reverb, it is not a lightweight
      // insert. A future ConvolutionReverb subclass therefore cannot be
      // published as "low" by falling through the classifier's default.
      if (dynamic_cast<sonare::effects::reverb::ConvolutionReverb*>(processor.get()) != nullptr) {
        INFO("convolution engine insert: " << entry["id"].as_string());
        REQUIRE(cost != "low");
        ++convolution_engine_inserts;
      }
#endif  // SONARE_WITH_FX
      processor->prepare(48000.0, 512);
      REQUIRE(latency == std::max(0, processor->latency_samples()));
      REQUIRE(tail == std::max(0, processor->tail_samples()));
    } else {
      REQUIRE(entry["realtimeCost"].is_null());
      REQUIRE(latency == 0);
      REQUIRE(tail == 0);
    }
  }
#ifdef SONARE_WITH_FX
  // The guard above is only worth anything if the engine it names is actually
  // reachable from the catalog: "effects.reverb.convolution" always is when the
  // FX suite is built, and the room reverb joins it when acoustics are on.
  REQUIRE(convolution_engine_inserts >= 1);
#endif  // SONARE_WITH_FX

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
  REQUIRE(
      json.find("{\"id\":\"eq.midSide\",\"kind\":\"realtime\",\"realtimeInsertable\":true,"
                "\"stereoOnly\":true,\"latencySamples\":0,\"tailSamples\":0,"
                "\"realtimeCost\":\"low\",\"channelPolicy\":\"stereoPairOnly\",\"category\":\"eq\","
                "\"params\":") != std::string::npos);

  // Delay-like stereo tools publish their prepared audible tail through the
  // same probe used for latency. Default Haas is 12 ms at 48 kHz; default
  // PhaseAlign has no delay.
  REQUIRE(json.find("{\"id\":\"stereo.haasEnhancer\",\"kind\":\"realtime\","
                    "\"realtimeInsertable\":true,\"stereoOnly\":true,"
                    "\"latencySamples\":0,\"tailSamples\":576,\"realtimeCost\":\"low\",") !=
          std::string::npos);
  REQUIRE(json.find("{\"id\":\"stereo.phaseAlign\",\"kind\":\"realtime\","
                    "\"realtimeInsertable\":true,\"stereoOnly\":true,"
                    "\"latencySamples\":0,\"tailSamples\":0,\"realtimeCost\":\"low\",") !=
          std::string::npos);

#ifdef SONARE_WITH_FX
  // Velvet's bounded multi-tap work is explicitly distinguishable from a
  // conventional FDN reverb, so hosts can budget a live processor chain. Both
  // are FX-suite processors, so they are only in the catalog when it is built.
  const auto velvet = std::find_if(
      catalog.as_array().begin(), catalog.as_array().end(),
      [](const auto& entry) { return entry["id"].as_string() == "effects.reverb.velvet"; });
  REQUIRE(velvet != catalog.as_array().end());
  REQUIRE((*velvet)["realtimeCost"].as_string() == "high");
  const auto fdn = std::find_if(
      catalog.as_array().begin(), catalog.as_array().end(),
      [](const auto& entry) { return entry["id"].as_string() == "effects.reverb.fdn"; });
  REQUIRE(fdn != catalog.as_array().end());
  REQUIRE((*fdn)["realtimeCost"].as_string() == "moderate");
#endif  // SONARE_WITH_FX

  // These tiers are qualitative policy for the default `{}` configuration,
  // rather than numeric benchmark results.
  const auto tube =
      std::find_if(catalog.as_array().begin(), catalog.as_array().end(),
                   [](const auto& entry) { return entry["id"].as_string() == "saturation.tube"; });
  REQUIRE(tube != catalog.as_array().end());
  REQUIRE((*tube)["realtimeCost"].as_string() == "high");
  const auto true_peak = std::find_if(
      catalog.as_array().begin(), catalog.as_array().end(),
      [](const auto& entry) { return entry["id"].as_string() == "maximizer.truePeakLimiter"; });
  REQUIRE(true_peak != catalog.as_array().end());
  REQUIRE((*true_peak)["realtimeCost"].as_string() == "moderate");

  const auto cost_of = [&catalog](const std::string& id) {
    const auto found =
        std::find_if(catalog.as_array().begin(), catalog.as_array().end(),
                     [&id](const auto& entry) { return entry["id"].as_string() == id; });
    REQUIRE(found != catalog.as_array().end());
    return (*found)["realtimeCost"].as_string();
  };

  // An id whose engine embeds one that is already tiered up inherits that tier
  // rather than falling through to "low": ampSim owns the same 4x oversampled
  // triode as saturation.tube (its 12-sample latency is that oversampler's).
  REQUIRE(cost_of("saturation.ampSim") == cost_of("saturation.tube"));

  // The linear-phase EQ runs a 513-tap FIR (the 256-sample latency is that
  // kernel's group delay) through the same partitioned convolver the
  // convolution reverb uses; it is not a biquad-chain EQ.
  REQUIRE(cost_of("eq.linearPhase") == "moderate");

  // The air band resamples its harmonic path 4x on every block -- its latency is
  // that oversampler's round trip -- so it shares the true-peak limiter's tier.
  // The exciter and presence enhancer hold the same oversampler but default it
  // off (zero latency), which is the configuration these tiers describe.
  REQUIRE(cost_of("spectral.airBand") == cost_of("maximizer.truePeakLimiter"));
  REQUIRE(cost_of("saturation.exciter") == "low");
  REQUIRE(cost_of("spectral.presenceEnhancer") == "low");

#ifdef SONARE_WITH_FX
  // The plate tank is a delay network like the FDN, so the two reverb families
  // share a tier. "effects.reverb.plate" is an alias for the same processor.
  REQUIRE(cost_of("effects.reverb.dattorro") == cost_of("effects.reverb.fdn"));
  REQUIRE(cost_of("effects.reverb.plate") == cost_of("effects.reverb.dattorro"));

  // The geometry-driven room reverb IS the convolution reverb (it derives from
  // it and inherits process()), so it carries that engine's tier exactly. The
  // in-loop dynamic_cast guard above keeps any other id wrapping that engine
  // off the "low" tier.
  if (ListContains(sonare::mastering::api::insert_factory_names(), "effects.reverb.room")) {
    REQUIRE(cost_of("effects.reverb.room") == cost_of("effects.reverb.convolution"));
  }
#endif  // SONARE_WITH_FX

  // The room-simulation reverb is only built when the acoustic feature is on, so
  // it is reported exactly when the insert factory can construct it.
  if (ListContains(sonare::mastering::api::insert_factory_names(), "effects.reverb.room")) {
    REQUIRE(
        json.find("{\"id\":\"effects.reverb.room\",\"kind\":\"realtime\",\"realtimeInsertable\":"
                  "true") != std::string::npos);
  }
}

// stereo_processor_names() asserts a capability: "this id has no mono form".
// The list is derived from the offline dispatch, and this pins that derivation
// against the behaviour a mono caller actually sees, for every registered id.
// A processor that gains a mono branch but stays on the list, or one that loses
// its mono branch without joining it, fails here rather than reaching a caller
// as a wrong diagnostic.
TEST_CASE("stereo_processor_names matches what the mono entry point does with every id",
          "[mastering][named_processor][channels]") {
  using sonare::mastering::api::apply_named_processor;
  using sonare::mastering::api::processor_names;
  using sonare::mastering::api::stereo_processor_names;

  constexpr int kSampleRate = 48000;
  std::vector<float> signal(4096, 0.0f);
  for (std::size_t index = 0; index < signal.size(); ++index) {
    signal[index] = 0.5f * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(index) /
                                    static_cast<float>(kSampleRate));
  }

  const auto declared = stereo_processor_names();
  std::vector<std::string> observed;
  for (const std::string& id : processor_names()) {
    INFO("processor " << id);
    try {
      apply_named_processor(id, signal.data(), signal.size(), kSampleRate, {});
    } catch (const sonare::SonareException& error) {
      const std::string message = error.what();
      // The only tolerated mono failure is the stereo-only diagnostic; an
      // "unknown processor" here would mean a registered id no entry point can
      // reach.
      REQUIRE(message.find("stereo-only") != std::string::npos);
      observed.push_back(id);
    }
  }
  std::sort(observed.begin(), observed.end());
  CHECK(observed == declared);

  // The five band-splitting dynamics processors are channel-generic and must
  // NOT be on the list: MasteringChain runs the same classes over a mono
  // buffer, so rejecting them from the mono one-shot API locked mono users out
  // of DSP that was already there.
  for (const char* id : {"multiband.compressor", "multiband.expander", "multiband.limiter",
                         "multiband.saturation", "multiband.dynamicEq"}) {
    INFO(id);
    CHECK(!ListContains(declared, id));
  }
  // multiband.imager works on the mid/side decomposition and stays stereo-only.
  CHECK(ListContains(declared, "multiband.imager"));
}

// A processor whose detector links its channels must produce one gain envelope
// for the pair. The one-shot stereo API used to run the mono processor twice,
// so an asymmetric input came back with two independent envelopes - audible as
// the stereo image moving on every transient, and different from what
// MasteringChain produces from the same settings.
TEST_CASE("apply_named_processor_stereo keeps a linked detector linked",
          "[mastering][named_processor][channels]") {
  using sonare::mastering::api::apply_named_processor_stereo;
  using sonare::mastering::api::Param;

  constexpr int kSampleRate = 48000;
  constexpr std::size_t kLength = 4096;
  std::vector<float> left(kLength, 0.0f);
  std::vector<float> right(kLength, 0.0f);
  for (std::size_t index = 0; index < kLength; ++index) {
    const float phase =
        2.0f * 3.14159265f * 220.0f * static_cast<float>(index) / static_cast<float>(kSampleRate);
    left[index] = 0.9f * std::sin(phase);
    right[index] = 0.2f * std::sin(phase);  // ~13 dB quieter
  }

  const std::vector<Param> params{
      {"thresholdDb", -30.0}, {"ratio", 8.0}, {"attackMs", 1.0}, {"releaseMs", 50.0}};
  const auto result = apply_named_processor_stereo("dynamics.compressor", left.data(), right.data(),
                                                   kLength, kSampleRate, params);
  REQUIRE(result.left.size() == kLength);

  float worst_gain_delta = 0.0f;
  int compared = 0;
  for (std::size_t index = 0; index < kLength; ++index) {
    // Skip near-zero-crossing samples, where the per-sample gain ratio is
    // numerically meaningless.
    if (std::abs(left[index]) < 0.05f || std::abs(right[index]) < 0.05f) continue;
    const float gain_left = result.left[index] / left[index];
    const float gain_right = result.right[index] / right[index];
    worst_gain_delta = std::max(worst_gain_delta, std::abs(gain_left - gain_right));
    ++compared;
  }
  REQUIRE(compared > 1000);
  INFO("max per-sample |gainL - gainR| = " << worst_gain_delta);
  CHECK(worst_gain_delta < 1.0e-5f);
}

// The mono entry point and the stereo entry point are the same dispatch, so a
// duplicated-channel stereo run must reproduce the mono run exactly. This is
// what makes "the multiband stages now work in mono" a routing fix rather than
// a second implementation.
TEST_CASE("mono and duplicated-stereo named-processor runs agree",
          "[mastering][named_processor][channels]") {
  using sonare::mastering::api::apply_named_processor;
  using sonare::mastering::api::apply_named_processor_stereo;

  constexpr int kSampleRate = 48000;
  std::vector<float> signal(4096, 0.0f);
  for (std::size_t index = 0; index < signal.size(); ++index) {
    signal[index] = 0.5f * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(index) /
                                    static_cast<float>(kSampleRate));
  }

  for (const char* id : {"multiband.compressor", "multiband.expander", "multiband.limiter",
                         "multiband.saturation", "multiband.dynamicEq", "dynamics.compressor"}) {
    INFO(id);
    const auto mono = apply_named_processor(id, signal.data(), signal.size(), kSampleRate, {});
    const auto stereo = apply_named_processor_stereo(id, signal.data(), signal.data(),
                                                     signal.size(), kSampleRate, {});
    REQUIRE(mono.samples.size() == stereo.left.size());
    float worst = 0.0f;
    for (std::size_t index = 0; index < mono.samples.size(); ++index) {
      worst = std::max(worst, std::abs(mono.samples[index] - stereo.left[index]));
      // A duplicated input must stay duplicated on the way out.
      CHECK(stereo.left[index] == stereo.right[index]);
    }
    INFO("max |mono - stereoL| = " << worst);
    CHECK(worst < 1.0e-6f);
  }
}

// The catalog's `type` comes from the declared C++ type of the config field a
// parameter writes, not from the key's spelling. autoMakeup is the standing
// counter-example to the old "key ends in Enabled" heuristic: it is a
// CompressorConfig `bool` that a host must draw as a toggle.
TEST_CASE("processor catalog publishes a boolean parameter as boolean",
          "[mastering][catalog][params]") {
  const std::string info = insert_param_info_json("dynamics.compressor");
  const auto params = sonare::util::json::parse(info);
  REQUIRE(params.is_array());
  bool saw_auto_makeup = false;
  bool saw_a_number = false;
  for (const auto& param : params.as_array()) {
    const std::string name = param["name"].as_string();
    const std::string type = param["type"].as_string();
    REQUIRE((type == "boolean" || type == "number"));
    if (name == "autoMakeup") {
      saw_auto_makeup = true;
      CHECK(type == "boolean");
    }
    if (name == "thresholdDb" || name == "ratio" || name == "sidechainHpfHz") {
      saw_a_number = true;
      CHECK(type == "number");
    }
    // An enum-valued field is still a number: only a C++ bool is a toggle.
    if (name == "detector") CHECK(type == "number");
  }
  CHECK(saw_auto_makeup);
  CHECK(saw_a_number);
}

TEST_CASE("processor_names() lists every effects insert the apply path dispatches",
          "[mastering][named_processor][effects]") {
  const auto inserts = insert_factory_names();
  const auto named = sonare::mastering::api::processor_names();

  // apply_named_processor() routes every "effects." id through the insert
  // factory, so an effect the factory builds but the registry omits still runs
  // when its name is typed by hand while being unreachable from the documented
  // name list and absent from the published processor-name types.
  std::size_t effects_listed = 0;
  for (const auto& id : inserts) {
    if (id.rfind("effects.", 0) != 0) continue;
    INFO("insert: " << id);
    REQUIRE(ListContains(named, id));
    ++effects_listed;
  }

  // The converse: the registry must not advertise an effect this build
  // configuration cannot construct, which would fail at apply time.
  for (const auto& id : named) {
    if (id.rfind("effects.", 0) != 0) continue;
    INFO("processor: " << id);
    REQUIRE(ListContains(inserts, id));
  }

#ifdef SONARE_WITH_FX
  REQUIRE(effects_listed > 0);
#else
  REQUIRE(effects_listed == 0);
#endif
}

#ifdef SONARE_WITH_FX
TEST_CASE("velvet insert factory bounds excessive reverb time",
          "[mastering][insert_factory][reverb]") {
  auto processor =
      make_insert("effects.reverb.velvet", R"({"reverbTimeS":40,"densityHz":3000,"decay":1})");
  REQUIRE(processor != nullptr);
  processor->prepare(48000.0, 512);
  REQUIRE(processor->tail_samples() <= 18 * 48000);
}
#endif  // SONARE_WITH_FX

TEST_CASE(
    "channel_policy tags inherently-stereo processors StereoPairOnly and the rest "
    "Multichannel",
    "[mastering][catalog]") {
  using sonare::mastering::api::channel_policy;
  using sonare::mastering::api::ChannelPolicy;

  // The inherently-stereo set: stereo-image processors, eq.midSide,
  // multiband.imager, and every reverb/modulation/delay effect operate on the
  // front L/R pair and pass surround planes through dry.
  const std::array<const char*, 24> spo = {"stereo.imager",
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
                                           "effects.modulation.wah",
                                           "effects.modulation.autoWah",
                                           "effects.modulation.rotary",
                                           "effects.modulation.pitchShifter",
                                           "effects.delay.stereo"};
  for (const char* id : spo) {
    REQUIRE(channel_policy(id) == ChannelPolicy::StereoPairOnly);
  }

  // Per-channel and linked-dynamics processors process every plane in one call.
  for (const char* id : {"dynamics.compressor", "eq.parametric", "saturation.tape",
                         "multiband.compressor", "maximizer.maximizer", "spectral.lowEndFocus"}) {
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

TEST_CASE("eq.dynamic accepts detectorDelayMs and its former lookaheadMs spelling",
          "[mastering][insert_factory][eq]") {
  // detectorDelayMs is the corrected name (see dynamic_eq.h); lookaheadMs is
  // kept accepted through the flat param-map construction path so a stored
  // config using the old spelling is not silently dropped.
  using sonare::mastering::eq::DynamicEq;

  auto with_new =
      make_insert("eq.dynamic", R"({"band0.frequencyHz":1000,"band0.detectorDelayMs":12})");
  REQUIRE(with_new != nullptr);
  auto* dyn_new = dynamic_cast<DynamicEq*>(with_new.get());
  REQUIRE(dyn_new != nullptr);
  REQUIRE(dyn_new->band(0).detector_delay_ms == 12.0f);

  auto with_old_camel =
      make_insert("eq.dynamic", R"({"band0.frequencyHz":1000,"band0.lookaheadMs":8})");
  auto* dyn_old_camel = dynamic_cast<DynamicEq*>(with_old_camel.get());
  REQUIRE(dyn_old_camel != nullptr);
  REQUIRE(dyn_old_camel->band(0).detector_delay_ms == 8.0f);

  // The canonical spelling wins when both are present.
  auto both =
      make_insert("eq.dynamic",
                  R"({"band0.frequencyHz":1000,"band0.lookaheadMs":8,"band0.detectorDelayMs":12})");
  auto* dyn_both = dynamic_cast<DynamicEq*>(both.get());
  REQUIRE(dyn_both != nullptr);
  REQUIRE(dyn_both->band(0).detector_delay_ms == 12.0f);
}
