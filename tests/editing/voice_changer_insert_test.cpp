/// @file voice_changer_insert_test.cpp
/// @brief The voice changer as a streaming insert: equivalence with the low-level class, the
///        published descriptors, construction-time refusal and refused automation.

#include "editing/voice_changer/voice_changer_insert.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "editing/voice_changer/realtime.h"
#include "effects/formant_warp.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "rt/processor_base.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/json.h"

namespace {

namespace json = sonare::util::json;
using sonare::ErrorCode;
using sonare::SonareException;
using sonare::editing::voice_changer::PitchEngine;
using sonare::editing::voice_changer::RealtimeVoiceChanger;
using sonare::editing::voice_changer::RealtimeVoiceChangerConfig;
using sonare::editing::voice_changer::VoiceChangerInsert;
using sonare::mastering::api::insert_param_info_json;
using sonare::mastering::api::make_insert;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

std::vector<float> tone(int samples, float amplitude, int channel) {
  std::vector<float> out(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    const double t = static_cast<double>(i) / kRate;
    out[static_cast<size_t>(i)] =
        amplitude *
        static_cast<float>(std::sin(sonare::constants::kTwoPiD * (220.0 + 90.0 * channel) * t) +
                           0.3 * std::sin(sonare::constants::kTwoPiD * 1370.0 * t));
  }
  return out;
}

RealtimeVoiceChangerConfig character_config() {
  RealtimeVoiceChangerConfig config;
  config.retune.semitones = 4.0f;
  config.formant.factor = 1.18f;
  config.formant.body = 0.2f;
  config.eq.presence_db = 3.0f;
  config.compressor.threshold_db = -30.0f;
  config.reverb.mix = 0.1f;
  return config;
}

// Runs @p channels through the insert in blocks of kBlock.
std::vector<std::vector<float>> run_insert(VoiceChangerInsert& insert,
                                           std::vector<std::vector<float>> channels) {
  const int total = static_cast<int>(channels[0].size());
  insert.prepare(kRate, kBlock, static_cast<int>(channels.size()));
  for (int pos = 0; pos < total; pos += kBlock) {
    const int length = std::min(kBlock, total - pos);
    std::vector<float*> planes;
    for (auto& channel : channels) planes.push_back(channel.data() + pos);
    insert.process(planes.data(), static_cast<int>(planes.size()), length);
  }
  return channels;
}

std::vector<std::vector<float>> run_class(RealtimeVoiceChanger& changer,
                                          std::vector<std::vector<float>> channels) {
  const int total = static_cast<int>(channels[0].size());
  changer.prepare(kRate, kBlock, static_cast<int>(channels.size()));
  for (int pos = 0; pos < total; pos += kBlock) {
    const int length = std::min(kBlock, total - pos);
    std::vector<float*> planes;
    for (auto& channel : channels) planes.push_back(channel.data() + pos);
    changer.process_block(planes.data(), static_cast<int>(planes.size()), length);
  }
  return channels;
}

json::Array catalog_params() {
  const json::Value parsed = json::parse_strict(insert_param_info_json("voice.changer"));
  REQUIRE(parsed.is_array());
  return parsed.as_array();
}

unsigned int descriptor_id(const VoiceChangerInsert& insert, const std::string& key) {
  for (const auto& descriptor : insert.parameter_descriptors()) {
    if (descriptor.key == key) return descriptor.id;
  }
  FAIL("no descriptor for " << key);
  return 0;
}

}  // namespace

TEST_CASE("voice.changer insert output is the low-level class output", "[voice_changer][insert]") {
  const RealtimeVoiceChangerConfig config = character_config();
  for (int channel_count : {1, 2}) {
    CAPTURE(channel_count);
    std::vector<std::vector<float>> input;
    for (int ch = 0; ch < channel_count; ++ch) input.push_back(tone(8192, 0.4f, ch));

    VoiceChangerInsert insert(config);
    RealtimeVoiceChanger changer(config);
    const auto from_insert = run_insert(insert, input);
    const auto from_class = run_class(changer, input);
    REQUIRE(from_insert == from_class);
    REQUIRE(insert.latency_samples() == changer.latency_samples());
    REQUIRE(insert.latency_samples() > 0);
    // The chain did something: the output is not the input.
    REQUIRE(from_insert[0] != input[0]);
  }
}

TEST_CASE("voice.changer dry path is delayed by the reported latency", "[voice_changer][insert]") {
  for (bool isp : {true, false}) {
    CAPTURE(isp);
    RealtimeVoiceChangerConfig config;
    config.wet_mix = 0.0f;
    config.limiter.enable_isp_limiter = isp;
    VoiceChangerInsert insert(config);
    const std::vector<std::vector<float>> input = {tone(6000, 0.25f, 0), tone(6000, 0.25f, 1)};
    const auto output = run_insert(insert, input);
    const int latency = insert.latency_samples();
    REQUIRE(latency > 0);
    for (int ch = 0; ch < 2; ++ch) {
      double worst = 0.0;
      for (size_t i = static_cast<size_t>(latency); i < output[0].size(); ++i) {
        worst =
            std::max(worst, static_cast<double>(std::fabs(
                                output[static_cast<size_t>(ch)][i] -
                                input[static_cast<size_t>(ch)][i - static_cast<size_t>(latency)])));
      }
      CHECK(worst < 1.0e-3);
    }
  }
}

TEST_CASE("voice.changer latency counts the limiter look-ahead only when it is on",
          "[voice_changer][insert]") {
  RealtimeVoiceChangerConfig with_isp;
  RealtimeVoiceChangerConfig without_isp;
  without_isp.limiter.enable_isp_limiter = false;
  VoiceChangerInsert on(with_isp);
  VoiceChangerInsert off(without_isp);
  REQUIRE(on.latency_samples() == 0);
  on.prepare(kRate, kBlock);
  off.prepare(kRate, kBlock);
  REQUIRE(on.latency_samples() > off.latency_samples());
  REQUIRE(off.latency_samples() > 0);
}

TEST_CASE("voice.changer refuses out-of-domain fields at construction", "[voice_changer][insert]") {
  const auto refused = [](RealtimeVoiceChangerConfig config) {
    try {
      VoiceChangerInsert insert(config);
    } catch (const SonareException& error) {
      return error.code() == ErrorCode::InvalidParameter;
    }
    return false;
  };
  RealtimeVoiceChangerConfig config;
  REQUIRE_NOTHROW(VoiceChangerInsert(config));

  config = {};
  config.retune.semitones = 24.5f;
  CHECK(refused(config));
  config = {};
  config.formant.factor = 0.5f;
  CHECK(refused(config));
  config = {};
  config.reverb.mix = 0.5f;
  CHECK(refused(config));
  config = {};
  config.retune.grain_size = 9000;
  CHECK(refused(config));
  config = {};
  config.limiter.ceiling_db = -0.5f;
  CHECK(refused(config));
  config = {};
  config.wet_mix = std::numeric_limits<float>::quiet_NaN();
  CHECK(refused(config));
  config = {};
  config.deesser.frequency_hz = std::numeric_limits<float>::infinity();
  CHECK(refused(config));

  // The edges themselves are accepted.
  config = {};
  config.retune.semitones = -24.0f;
  config.formant.factor = 1.65f;
  config.limiter.ceiling_db = -1.0f;
  config.retune.grain_size = 8192;
  CHECK_NOTHROW(VoiceChangerInsert(config));
}

TEST_CASE("voice.changer publishes real bounds and says what is construction-only",
          "[voice_changer][insert][catalog]") {
  const json::Array params = catalog_params();
  REQUIRE(params.size() == 71);

  const std::vector<std::string> construction_only = {
      "retuneGrainSize",   "formantMode",        "reverbSeed",   "limiterEnableIspLimiter",
      "voiceEngineEngine", "voiceEngineF0MinHz", "vocoderBands", "vocoderCarrierSource"};
  const std::vector<std::string> enums = {
      "formantMode",          "voiceEngineEngine",        "pitchCorrectionMode",
      "pitchCorrectionScale", "vocoderCarrierSource",     "vocoderCarrierWave",
      "characterType",        "reverbVariationVariation", "reverbVariationDivision",
      "midiRelease"};
  VoiceChangerInsert probe;
  size_t automatable = 0;
  for (const json::Value& param : params) {
    const std::string name = param.find("name")->as_string();
    CAPTURE(name);
    const bool only = std::find(construction_only.begin(), construction_only.end(), name) !=
                      construction_only.end();
    CHECK(param.find("rtSafe")->as_bool() == !only);
    CHECK(param.find("id")->is_null() == only);
    if (!only) ++automatable;
    const std::string type = param.find("type")->as_string();
    if (type == "boolean") continue;
    if (type == "enum") {
      CHECK(std::find(enums.begin(), enums.end(), name) != enums.end());
      continue;
    }
    CHECK_FALSE(param.find("unit")->is_null());
    if (name == "reverbSeed") continue;
    // The bounds are measured from the refusal above, so every one is finite.
    CHECK_FALSE(param.find("min")->is_null());
    CHECK_FALSE(param.find("max")->is_null());
    CHECK_FALSE(param.find("uiMin")->is_null());
    CHECK_FALSE(param.find("uiMax")->is_null());
    CHECK(param.find("uiMin")->as_number() >= param.find("min")->as_number());
    CHECK(param.find("uiMax")->as_number() <= param.find("max")->as_number());
  }
  REQUIRE(automatable == probe.parameter_descriptors().size());
}

TEST_CASE("voice.changer factory refuses a published bound's far side",
          "[voice_changer][insert][catalog]") {
  for (const json::Value& param : catalog_params()) {
    const std::string name = param.find("name")->as_string();
    if (param.find("min")->is_null() || param.find("max")->is_null()) continue;
    CAPTURE(name);
    const double min = param.find("min")->as_number();
    const double max = param.find("max")->as_number();
    const double span = max - min;
    const auto build = [&](double value) {
      json::Object params;
      params.emplace(name, json::Value(value));
      return make_insert("voice.changer", json::dump(json::Value(std::move(params))));
    };
    CHECK(build(min) != nullptr);
    CHECK(build(max) != nullptr);
    CHECK_THROWS_AS(build(min - 0.01 * span), SonareException);
    CHECK_THROWS_AS(build(max + 0.01 * span), SonareException);
  }
}

TEST_CASE(
    "voice.changer realtime parameters reach the field a key names and refuse out-of-range values",
    "[voice_changer][insert]") {
  // Whole-number parameters take the middle rounded to a whole number.
  const std::vector<std::string> integers = {"pitchCorrectionKeyRoot", "pitchCorrectionRobotOctave",
                                             "harmonyVoices",          "harmonyStep1",
                                             "harmonyStep2",           "harmonyStep3"};
  for (const json::Value& param : catalog_params()) {
    if (param.find("id")->is_null()) continue;
    const std::string name = param.find("name")->as_string();
    CAPTURE(name);
    // A switch is driven on; a choice takes its last value; a number its middle.
    double min = 0.0;
    double max = 1.0;
    double middle = 1.0;
    if (param.find("type")->as_string() != "boolean") {
      if (!param.find("choices")->is_null()) {
        const json::Array& choices = param.find("choices")->as_array();
        REQUIRE_FALSE(choices.empty());
        min = choices.front().find("value")->as_number();
        max = choices.back().find("value")->as_number();
        middle = max;
      } else {
        min = param.find("min")->as_number();
        max = param.find("max")->as_number();
        middle = 0.5 * (min + max);
        if (std::find(integers.begin(), integers.end(), name) != integers.end()) {
          middle = std::round(middle);
        }
      }
    }

    json::Object params;
    params.emplace(name, json::Value(middle));
    auto built = make_insert("voice.changer", json::dump(json::Value(std::move(params))));
    auto* constructed = dynamic_cast<VoiceChangerInsert*>(built.get());
    REQUIRE(constructed != nullptr);

    VoiceChangerInsert automated;
    const unsigned int id = descriptor_id(automated, name);
    CHECK(automated.parameter_is_realtime_safe(id));
    REQUIRE(automated.set_parameter(id, static_cast<float>(middle)));
    // Same field, same value as construction: the two paths agree on the config.
    const RealtimeVoiceChangerConfig& a = automated.config();
    const RealtimeVoiceChangerConfig& b = constructed->config();
    CHECK(a.input_gain_db == b.input_gain_db);
    CHECK(a.output_gain_db == b.output_gain_db);
    CHECK(a.wet_mix == b.wet_mix);
    CHECK(a.retune.semitones == b.retune.semitones);
    CHECK(a.retune.mix == b.retune.mix);
    CHECK(a.formant.factor == b.formant.factor);
    CHECK(a.formant.amount == b.formant.amount);
    CHECK(a.formant.body == b.formant.body);
    CHECK(a.formant.brightness == b.formant.brightness);
    CHECK(a.formant.nasal == b.formant.nasal);
    CHECK(a.eq.highpass_hz == b.eq.highpass_hz);
    CHECK(a.eq.body_db == b.eq.body_db);
    CHECK(a.eq.presence_db == b.eq.presence_db);
    CHECK(a.eq.air_db == b.eq.air_db);
    CHECK(a.gate.threshold_db == b.gate.threshold_db);
    CHECK(a.gate.attack_ms == b.gate.attack_ms);
    CHECK(a.gate.release_ms == b.gate.release_ms);
    CHECK(a.gate.range_db == b.gate.range_db);
    CHECK(a.compressor.threshold_db == b.compressor.threshold_db);
    CHECK(a.compressor.ratio == b.compressor.ratio);
    CHECK(a.compressor.attack_ms == b.compressor.attack_ms);
    CHECK(a.compressor.release_ms == b.compressor.release_ms);
    CHECK(a.compressor.makeup_gain_db == b.compressor.makeup_gain_db);
    CHECK(a.deesser.frequency_hz == b.deesser.frequency_hz);
    CHECK(a.deesser.threshold_db == b.deesser.threshold_db);
    CHECK(a.deesser.ratio == b.deesser.ratio);
    CHECK(a.deesser.range_db == b.deesser.range_db);
    CHECK(a.reverb.mix == b.reverb.mix);
    CHECK(a.reverb.time_ms == b.reverb.time_ms);
    CHECK(a.reverb.damping == b.reverb.damping);
    CHECK(a.limiter.ceiling_db == b.limiter.ceiling_db);
    CHECK(a.limiter.release_ms == b.limiter.release_ms);
    CHECK(a.limiter.isp_ceiling_dbtp == b.limiter.isp_ceiling_dbtp);
    CHECK(a.pitch_correction.mode == b.pitch_correction.mode);
    CHECK(a.pitch_correction.amount == b.pitch_correction.amount);
    CHECK(a.pitch_correction.speed_ms == b.pitch_correction.speed_ms);
    CHECK(a.pitch_correction.key_root == b.pitch_correction.key_root);
    CHECK(a.pitch_correction.scale == b.pitch_correction.scale);
    CHECK(a.pitch_correction.robot_octave == b.pitch_correction.robot_octave);
    CHECK(a.harmony.voices == b.harmony.voices);
    CHECK(a.harmony.steps[0] == b.harmony.steps[0]);
    CHECK(a.harmony.steps[1] == b.harmony.steps[1]);
    CHECK(a.harmony.steps[2] == b.harmony.steps[2]);
    CHECK(a.harmony.level == b.harmony.level);
    CHECK(a.harmony.lead_level == b.harmony.lead_level);
    CHECK(a.harmony.detune_cents == b.harmony.detune_cents);
    CHECK(a.harmony.delay_ms == b.harmony.delay_ms);
    CHECK(a.vocoder.enabled == b.vocoder.enabled);
    CHECK(a.vocoder.attack_ms == b.vocoder.attack_ms);
    CHECK(a.vocoder.release_ms == b.vocoder.release_ms);
    CHECK(a.vocoder.sibilance == b.vocoder.sibilance);
    CHECK(a.vocoder.carrier_wave == b.vocoder.carrier_wave);
    CHECK(a.vocoder.ensemble_detune_cents == b.vocoder.ensemble_detune_cents);
    CHECK(a.vocoder.level == b.vocoder.level);
    CHECK(a.character.type == b.character.type);
    CHECK(a.character.amount == b.character.amount);
    CHECK(a.character.tone == b.character.tone);
    CHECK(a.reverb_variation.variation == b.reverb_variation.variation);
    CHECK(a.reverb_variation.delay_ms == b.reverb_variation.delay_ms);
    CHECK(a.reverb_variation.feedback == b.reverb_variation.feedback);
    CHECK(a.reverb_variation.tempo_bpm == b.reverb_variation.tempo_bpm);
    CHECK(a.reverb_variation.division == b.reverb_variation.division);
    CHECK(a.midi.release == b.midi.release);

    // Past either bound the automation path refuses and leaves the configuration alone; a
    // switch has no bound, only a refused NaN.
    if (param.find("type")->as_string() != "boolean") {
      CHECK_FALSE(automated.set_parameter(id, static_cast<float>(max + 10.0 * (max - min) + 1.0)));
      CHECK_FALSE(automated.set_parameter(id, static_cast<float>(min - 10.0 * (max - min) - 1.0)));
    }
    CHECK(automated.config().formant.factor == b.formant.factor);
    CHECK_FALSE(automated.set_parameter(id, std::numeric_limits<float>::quiet_NaN()));
  }
  VoiceChangerInsert insert;
  CHECK_FALSE(
      insert.set_parameter(static_cast<unsigned int>(insert.parameter_descriptors().size()), 0.0f));
}

TEST_CASE("voice.changer automation refuses an out-of-range value and keeps the config",
          "[voice_changer][insert]") {
  VoiceChangerInsert insert;
  const unsigned int semitones_id = descriptor_id(insert, "retuneSemitones");
  const unsigned int factor_id = descriptor_id(insert, "formantFactor");
  const unsigned int ceiling_id = descriptor_id(insert, "limiterCeilingDb");
  CHECK_FALSE(insert.set_parameter(semitones_id, 99.0f));
  CHECK(insert.config().retune.semitones == 0.0f);
  CHECK_FALSE(insert.set_parameter(factor_id, 3.0f));
  CHECK(insert.config().formant.factor == 1.0f);
  CHECK_FALSE(insert.set_parameter(ceiling_id, 0.5f));
  CHECK(insert.config().limiter.ceiling_db == -1.0f);
  // The edge itself is accepted.
  CHECK(insert.set_parameter(semitones_id, 24.0f));
  CHECK(insert.config().retune.semitones == 24.0f);
}

TEST_CASE("voice.changer insert construction names the out-of-range field",
          "[voice_changer][insert]") {
  RealtimeVoiceChangerConfig config;
  config.formant.factor = 3.0f;
  try {
    VoiceChangerInsert insert(config);
    FAIL("expected a refusal");
  } catch (const SonareException& e) {
    CHECK(e.code() == ErrorCode::InvalidParameter);
    CHECK(std::string(e.what()) ==
          "voice.changer: formant.factor out of range [0.55, 1.65], got 3");
  }
}

TEST_CASE("voice.changer automation takes effect on the audio", "[voice_changer][insert]") {
  VoiceChangerInsert insert;
  insert.prepare(kRate, kBlock, 1);
  std::vector<float> signal = tone(kBlock * 40, 0.4f, 0);
  const unsigned int mix_id = descriptor_id(insert, "wetMix");
  double dry_error = 0.0;
  for (int pos = 0; pos < static_cast<int>(signal.size()); pos += kBlock) {
    if (pos == kBlock * 4) REQUIRE(insert.set_parameter(mix_id, 0.0f));
    float* plane = signal.data() + pos;
    insert.process(&plane, 1, kBlock);
  }
  // Once the mix has settled to dry, the output is the delayed input again.
  const std::vector<float> reference = tone(kBlock * 40, 0.4f, 0);
  const size_t latency = static_cast<size_t>(insert.latency_samples());
  for (size_t i = signal.size() - 512; i < signal.size(); ++i) {
    dry_error =
        std::max(dry_error, static_cast<double>(std::fabs(signal[i] - reference[i - latency])));
  }
  CHECK(dry_error < 1.0e-3);
}

TEST_CASE("voice.changer reports gain reduction and non-finite discards",
          "[voice_changer][insert]") {
  RealtimeVoiceChangerConfig config;
  config.compressor.threshold_db = -40.0f;
  config.compressor.ratio = 10.0f;
  config.compressor.makeup_gain_db = 0.0f;
  VoiceChangerInsert loud(config);
  loud.prepare(kRate, kBlock, 1);
  std::vector<float> signal = tone(kBlock * 80, 0.6f, 0);
  float worst = 0.0f;
  for (int pos = 0; pos < static_cast<int>(signal.size()); pos += kBlock) {
    float* plane = signal.data() + pos;
    loud.process(&plane, 1, kBlock);
    worst = std::min(worst, loud.last_gain_reduction_db());
  }
  CHECK(worst < -6.0f);
  CHECK(worst > -40.0f);

  // Quiet material under the threshold, with make-up in the chain, is not a reduction.
  RealtimeVoiceChangerConfig quiet_config;
  VoiceChangerInsert quiet(quiet_config);
  quiet.prepare(kRate, kBlock, 1);
  std::vector<float> faint = tone(kBlock * 80, 0.001f, 0);
  for (int pos = 0; pos < static_cast<int>(faint.size()); pos += kBlock) {
    float* plane = faint.data() + pos;
    quiet.process(&plane, 1, kBlock);
  }
  CHECK(quiet.last_gain_reduction_db() > -0.1f);
  quiet.reset();
  CHECK(quiet.last_gain_reduction_db() == 0.0f);

  VoiceChangerInsert insert;
  insert.prepare(kRate, kBlock, 1);
  std::vector<float> dirty = tone(kBlock, 0.2f, 0);
  dirty[10] = std::numeric_limits<float>::quiet_NaN();
  float* plane = dirty.data();
  insert.process(&plane, 1, kBlock);
  CHECK(insert.non_finite_discard_count() == 1);
  for (float sample : dirty) REQUIRE(std::isfinite(sample));
  std::vector<float> clean = tone(kBlock, 0.2f, 0);
  plane = clean.data();
  insert.process(&plane, 1, kBlock);
  CHECK(insert.non_finite_discard_count() == 1);
}

TEST_CASE("voice.changer splits oversized blocks and refuses more than two channels",
          "[voice_changer][insert]") {
  const RealtimeVoiceChangerConfig config = character_config();
  VoiceChangerInsert whole(config);
  VoiceChangerInsert chunked(config);
  whole.prepare(kRate, kBlock, 1);
  chunked.prepare(kRate, kBlock, 1);
  std::vector<float> a = tone(kBlock * 3 + 17, 0.3f, 0);
  std::vector<float> b = a;
  float* pa = a.data();
  whole.process(&pa, 1, static_cast<int>(a.size()));
  for (int pos = 0; pos < static_cast<int>(b.size()); pos += kBlock) {
    float* pb = b.data() + pos;
    chunked.process(&pb, 1, std::min(kBlock, static_cast<int>(b.size()) - pos));
  }
  CHECK(a == b);

  VoiceChangerInsert insert;
  CHECK_THROWS_AS(insert.prepare(kRate, kBlock, 3), SonareException);
  insert.prepare(kRate, kBlock, 2);
  std::vector<float> planes[3] = {tone(kBlock, 0.1f, 0), tone(kBlock, 0.1f, 1),
                                  tone(kBlock, 0.1f, 2)};
  float* pointers[3] = {planes[0].data(), planes[1].data(), planes[2].data()};
  CHECK_THROWS_AS(insert.process(pointers, 3, kBlock), SonareException);

  VoiceChangerInsert unprepared;
  CHECK_THROWS_AS(unprepared.process(pointers, 2, kBlock), SonareException);
}

TEST_CASE("voice.changer is a registered stereo-pair insert in the catalog",
          "[voice_changer][insert][catalog]") {
  const json::Value catalog = json::parse_strict(sonare::mastering::api::processor_catalog_json());
  const json::Value* entry = nullptr;
  for (const json::Value& processor : catalog.as_array()) {
    if (processor.find("id")->as_string() == "voice.changer") entry = &processor;
  }
  REQUIRE(entry != nullptr);
  CHECK(entry->find("kind")->as_string() == "realtime");
  CHECK(entry->find("category")->as_string() == "voice");
  CHECK(entry->find("channelPolicy")->as_string() == "stereoPairOnly");
  CHECK(entry->find("latencySamples")->as_number() > 0.0);
  CHECK(entry->find("causal")->as_bool());
}

TEST_CASE("voice.changer is reachable from the one-shot named-processor path",
          "[voice_changer][insert][catalog]") {
  const auto names = sonare::mastering::api::processor_names();
  REQUIRE(std::find(names.begin(), names.end(), "voice.changer") != names.end());
  const auto stereo_only = sonare::mastering::api::stereo_processor_names();
  CHECK(std::find(stereo_only.begin(), stereo_only.end(), "voice.changer") == stereo_only.end());

  const std::vector<float> left = tone(9000, 0.3f, 0);
  const std::vector<float> right = tone(9000, 0.3f, 1);
  const std::vector<sonare::mastering::api::Param> params = {{"retuneSemitones", 3.0}};

  const auto mono = sonare::mastering::api::apply_named_processor("voice.changer", left.data(),
                                                                  left.size(), 48000, params);
  CHECK(mono.samples.size() == left.size());
  CHECK(mono.latency_samples > 0);
  CHECK(mono.samples != left);

  const auto stereo = sonare::mastering::api::apply_named_processor_stereo(
      "voice.changer", left.data(), right.data(), left.size(), 48000, params);
  CHECK(stereo.left.size() == left.size());
  CHECK(stereo.right.size() == right.size());
  CHECK(stereo.left == mono.samples);

  CHECK_THROWS_AS(
      sonare::mastering::api::apply_named_processor("voice.changer", left.data(), left.size(),
                                                    48000, {{"retuneSemitones", 40.0}}),
      SonareException);
}

TEST_CASE("voice.changer reports the reverb decay as its tail", "[voice_changer][insert]") {
  RealtimeVoiceChangerConfig config;
  config.reverb.mix = 0.45f;
  config.reverb.time_ms = 400.0f;
  config.limiter.enable_isp_limiter = false;
  VoiceChangerInsert insert(config);
  CHECK(insert.tail_samples() == 0);  // not prepared yet
  insert.prepare(kRate, kBlock, 1);
  const int tail = insert.tail_samples();
  CHECK(tail > static_cast<int>(std::ceil(0.4 * kRate)));
  CHECK(tail < static_cast<int>(std::ceil(3.0 * kRate)));

  // Without the reverb the tail is what the filters and the retune history still owe.
  config.reverb.mix = 0.0f;
  VoiceChangerInsert dry_reverb(config);
  dry_reverb.prepare(kRate, kBlock, 1);
  CHECK(dry_reverb.tail_samples() > 0);
  CHECK(dry_reverb.tail_samples() < tail);
  // A wet mix at zero owes only what its glide still lets through.
  config.reverb.mix = 0.45f;
  config.wet_mix = 0.0f;
  VoiceChangerInsert dry_mix(config);
  dry_mix.prepare(kRate, kBlock, 1);
  CHECK(dry_mix.tail_samples() > 0);
  CHECK(dry_mix.tail_samples() < static_cast<int>(std::ceil(0.2 * kRate)));

  // A short burst, then silence for the reported latency and tail.
  const auto render = [&](VoiceChangerInsert& processor, int total) {
    std::vector<float> signal(static_cast<size_t>(total), 0.0f);
    const std::vector<float> burst = tone(2048, 0.5f, 0);
    std::copy(burst.begin(), burst.end(), signal.begin());
    processor.prepare(kRate, kBlock, 1);
    for (int pos = 0; pos < total; pos += kBlock) {
      float* plane = signal.data() + pos;
      processor.process(&plane, 1, std::min(kBlock, total - pos));
    }
    return signal;
  };
  const auto rms = [](const std::vector<float>& x, int begin, int end) {
    double sum = 0.0;
    for (int i = begin; i < end; ++i)
      sum += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
    return std::sqrt(sum / std::max(1, end - begin));
  };
  const int latency = insert.latency_samples();
  const int input_end = 2048 + latency;
  const int total = input_end + tail + 2400 + kBlock;
  const auto with_reverb = render(insert, total);

  RealtimeVoiceChangerConfig no_reverb_config = config;
  no_reverb_config.reverb.mix = 0.0f;
  VoiceChangerInsert no_reverb(no_reverb_config);
  const auto without_reverb = render(no_reverb, total);

  // The tail is audible through its first half and sits well under the floor at its end.
  std::vector<float> reverb_only(with_reverb.size());
  for (size_t i = 0; i < with_reverb.size(); ++i)
    reverb_only[i] = with_reverb[i] - without_reverb[i];
  const int window = 2400;
  double peak = 0.0;
  for (int pos = input_end; pos + window <= input_end + tail; pos += window) {
    peak = std::max(peak, rms(reverb_only, pos, pos + window));
  }
  const int quarter = input_end + tail / 4;
  const double kept = rms(reverb_only, quarter, quarter + window);
  const double residue = rms(reverb_only, input_end + tail, input_end + tail + window);
  INFO("kept " << 20.0 * std::log10(kept / peak) << " dB, residue "
               << 20.0 * std::log10(std::max(residue, 1.0e-12) / peak) << " dB");
  CHECK(kept > 1.0e-3 * peak);
  CHECK(20.0 * std::log10(std::max(residue, 1.0e-12) / peak) < -45.0);
}

namespace {

// Renders a unit impulse or a 0.5 s two-tone burst (peak 0.35) through an insert built from
// @p config at @p rate and requires silence below the tail floor once latency + reported tail
// have elapsed. @p prime runs first; @p at_input_end runs once the input has been fed and before
// the tail is read, so a live change made there must be covered by the tail it reports.
void require_silent_after_reported_tail(
    double rate, const RealtimeVoiceChangerConfig& config, bool impulse,
    const std::function<void(VoiceChangerInsert&)>& prime = {},
    const std::function<void(VoiceChangerInsert&)>& at_input_end = {}) {
  VoiceChangerInsert insert(config);
  insert.prepare(rate, kBlock, 1);
  if (prime) prime(insert);

  const int input_length = impulse ? 1 : static_cast<int>(0.5 * rate);
  std::vector<float> input(static_cast<size_t>(input_length), 0.0f);
  if (impulse) {
    input[0] = 1.0f;
  } else {
    for (int i = 0; i < input_length; ++i) {
      const double t = static_cast<double>(i) / rate;
      input[static_cast<size_t>(i)] =
          static_cast<float>(0.25 * std::sin(sonare::constants::kTwoPiD * 220.0 * t) +
                             0.1 * std::sin(sonare::constants::kTwoPiD * 1370.0 * t));
    }
  }
  std::vector<float> output;
  const auto feed = [&](std::vector<float> block) {
    for (size_t pos = 0; pos < block.size(); pos += kBlock) {
      float* plane = block.data() + pos;
      insert.process(&plane, 1, static_cast<int>(std::min<size_t>(kBlock, block.size() - pos)));
    }
    output.insert(output.end(), block.begin(), block.end());
  };
  feed(input);
  if (at_input_end) at_input_end(insert);

  const int latency = insert.latency_samples();
  const int tail = insert.tail_samples();
  REQUIRE(tail > 0);
  const int silent_from = input_length + latency + tail;
  const int total = silent_from + static_cast<int>(0.25 * rate);
  feed(std::vector<float>(static_cast<size_t>(total - input_length), 0.0f));

  float residual = 0.0f;
  int last_audible = -1;
  for (int i = silent_from; i < total; ++i) {
    const float magnitude = std::fabs(output[static_cast<size_t>(i)]);
    residual = std::max(residual, magnitude);
    if (magnitude >= 1.0e-5f) last_audible = i;
  }
  INFO("rate " << rate << " impulse " << impulse << " latency " << latency << " tail " << tail
               << " residual " << residual << " last audible " << last_audible);
  CHECK(residual < 1.0e-5f);
}

RealtimeVoiceChangerConfig reverberant_config(float reverb_time_ms) {
  RealtimeVoiceChangerConfig config;
  config.reverb.mix = 0.45f;
  config.reverb.time_ms = reverb_time_ms;
  config.limiter.enable_isp_limiter = false;
  return config;
}

unsigned int parameter_id(const VoiceChangerInsert& insert, const std::string& key) {
  for (const auto& descriptor : insert.parameter_descriptors()) {
    if (descriptor.key == key) return descriptor.id;
  }
  FAIL("no parameter " << key);
  return 0;
}

}  // namespace

TEST_CASE("voice.changer is silent after its reported latency and tail",
          "[voice_changer][insert]") {
  for (const double rate : {44100.0, 48000.0, 96000.0}) {
    CAPTURE(rate);
    for (const bool impulse : {true, false}) {
      require_silent_after_reported_tail(rate, reverberant_config(320.0f), impulse);
    }

    // A short reverb whose diffusion allpass outlasts the comb echoes.
    RealtimeVoiceChangerConfig diffuse = reverberant_config(40.0f);
    diffuse.reverb.damping = 0.55f;
    diffuse.reverb.seed = 1;
    require_silent_after_reported_tail(rate, diffuse, true);

    // No reverb: the high-pass, colour and EQ sections and the retune history still ring.
    RealtimeVoiceChangerConfig dry = reverberant_config(320.0f);
    dry.reverb.mix = 0.0f;
    dry.wet_mix = 1.0f;
    require_silent_after_reported_tail(rate, dry, true);

    // The wet mix taken to zero as the input ends still lets the wet path through its glide.
    require_silent_after_reported_tail(rate, reverberant_config(320.0f), false, {},
                                       [](VoiceChangerInsert& insert) {
                                         insert.set_parameter(parameter_id(insert, "wetMix"), 0.0f);
                                       });

    // A retune held at -5 semitones and returned to unity keeps reading its source history.
    RealtimeVoiceChangerConfig retuned = reverberant_config(320.0f);
    retuned.voice_engine.engine = PitchEngine::Grain;  // the retune's own source history
    retuned.reverb.mix = 0.0f;
    retuned.retune.mix = 1.0f;
    retuned.retune.semitones = -5.0f;
    require_silent_after_reported_tail(rate, retuned, true, [rate](VoiceChangerInsert& insert) {
      std::vector<float> program(static_cast<size_t>(rate));
      for (size_t i = 0; i < program.size(); ++i) {
        program[i] = 0.3f * std::sin(sonare::constants::kTwoPi * 220.0f * static_cast<float>(i) /
                                     static_cast<float>(rate));
      }
      program.resize(program.size() + 2 * static_cast<size_t>(rate), 0.0f);
      bool returned = false;
      for (size_t pos = 0; pos < program.size(); pos += kBlock) {
        if (!returned && pos >= static_cast<size_t>(rate)) {
          insert.set_parameter(parameter_id(insert, "retuneSemitones"), 0.0f);
          returned = true;
        }
        float* plane = program.data() + pos;
        insert.process(&plane, 1, static_cast<int>(std::min<size_t>(kBlock, program.size() - pos)));
      }
    });
  }
}

TEST_CASE("voice.changer is silent after its reported tail at long decay times",
          "[.][slow][voice_changer][insert]") {
  for (const double rate : {44100.0, 48000.0, 96000.0}) {
    for (const float time_ms : {800.0f, 1800.0f}) {
      for (const bool impulse : {true, false}) {
        require_silent_after_reported_tail(rate, reverberant_config(time_ms), impulse);
      }
    }
  }
}

TEST_CASE("voice.changer takes its formant mode at construction only", "[voice_changer][insert]") {
  // The warp frame is the grain engine's; psola resamples its grains instead.
  RealtimeVoiceChangerConfig relative;
  relative.voice_engine.engine = PitchEngine::Grain;
  RealtimeVoiceChangerConfig absolute = relative;
  absolute.formant_mode = sonare::editing::voice_changer::FormantMode::Absolute;
  absolute.formant.factor = 1.1f;
  VoiceChangerInsert plain(relative);
  VoiceChangerInsert warped(absolute);
  plain.prepare(kRate, kBlock);
  warped.prepare(kRate, kBlock);
  CHECK(warped.latency_samples() ==
        plain.latency_samples() + sonare::formant_warp_frame_size(static_cast<int>(kRate)));

  // The mode is not a realtime parameter.
  for (const auto& descriptor : warped.parameter_descriptors()) {
    CHECK(descriptor.key != "formantMode");
  }
  CHECK_FALSE(
      warped.set_parameter(static_cast<unsigned int>(warped.parameter_descriptors().size()), 0.0f));

  // The catalog publishes it as a named construction-time choice, and the factory reads it.
  bool found = false;
  for (const json::Value& param : catalog_params()) {
    if (param.find("name")->as_string() != "formantMode") continue;
    found = true;
    CHECK(param.find("type")->as_string() == "enum");
    CHECK(param.find("rtSafe")->as_bool() == false);
  }
  CHECK(found);
  const auto built = make_insert("voice.changer", R"({"formantMode":1,"voiceEngineEngine":0})");
  REQUIRE(built != nullptr);
  built->prepare(kRate, kBlock);
  CHECK(built->latency_samples() == warped.latency_samples());
}

TEST_CASE("voice.changer refuses an unreachable absolute warp at construction and under automation",
          "[voice_changer][insert]") {
  RealtimeVoiceChangerConfig config;
  config.voice_engine.engine = PitchEngine::Grain;  // only the grain engine warps ahead
  config.formant_mode = sonare::editing::voice_changer::FormantMode::Absolute;
  config.retune.semitones = -9.0f;
  config.formant.factor = 1.0f;
  try {
    VoiceChangerInsert insert(config);
    FAIL("expected a refusal");
  } catch (const SonareException& e) {
    CHECK(e.code() == ErrorCode::InvalidParameter);
    CHECK(std::string(e.what()).find("[0.55, 0.9811]") != std::string::npos);
  }

  // Automation past the edge is refused, the warp stays reachable and the audio stays finite.
  config.retune.semitones = 0.0f;
  VoiceChangerInsert insert(config);
  insert.prepare(kRate, kBlock, 1);
  const unsigned int semitones_id = descriptor_id(insert, "retuneSemitones");
  std::vector<float> signal = tone(kBlock * 30, 0.3f, 0);
  for (int pos = 0; pos < static_cast<int>(signal.size()); pos += kBlock) {
    if (pos == kBlock * 4) REQUIRE_FALSE(insert.set_parameter(semitones_id, -20.0f));
    float* plane = signal.data() + pos;
    insert.process(&plane, 1, kBlock);
  }
  for (const float v : signal) REQUIRE(std::isfinite(v));
  CHECK(insert.config().retune.semitones == 0.0f);
}

TEST_CASE("voice.changer insert gain reduction ignores the make-up ramp",
          "[voice_changer][insert][gain-reduction]") {
  for (const double rate : {44100.0, 48000.0, 96000.0}) {
    for (const int block : {1, 128, 512}) {
      for (const float makeup_db : {0.0f, 6.0f, 12.0f}) {
        INFO("rate=" << rate << " block=" << block << " makeup_db=" << makeup_db);
        RealtimeVoiceChangerConfig config;
        config.compressor.ratio = 1.0f;
        config.compressor.makeup_gain_db = makeup_db;
        config.deesser.ratio = 1.0f;
        VoiceChangerInsert insert(config);
        insert.prepare(rate, block, 1);
        std::vector<float> silence(static_cast<size_t>(block), 0.0f);
        const int blocks = std::max(4, static_cast<int>(rate * 0.2) / block);
        for (int b = 0; b < blocks; ++b) {
          float* plane = silence.data();
          insert.process(&plane, 1, block);
          REQUIRE(std::fabs(insert.last_gain_reduction_db()) < 0.01f);
        }
      }
    }
  }
}
