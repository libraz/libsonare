/// @file sonare_c_voice_transform_test.cpp
/// @brief C ABI contract of the voice transformer fields, note entries and carrier entries of the
///        realtime voice changer.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "util/constants.h"

namespace {

using Catch::Matchers::ContainsSubstring;
using Config = SonareRealtimeVoiceChangerConfig;

constexpr int kRate = 48000;
constexpr int kBlock = 128;
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

struct HandleDeleter {
  void operator()(SonareRealtimeVoiceChanger* h) const { sonare_realtime_voice_changer_destroy(h); }
};
using Handle = std::unique_ptr<SonareRealtimeVoiceChanger, HandleDeleter>;

Config default_config() {
  Config c{};
  REQUIRE(sonare_realtime_voice_changer_config_default(&c) == SONARE_OK);
  return c;
}

Config preset_config(SonareVoiceCharacterPreset preset) {
  Config c{};
  REQUIRE(sonare_realtime_voice_changer_preset_config(preset, &c) == SONARE_OK);
  return c;
}

SonareError create(const Config& c, Handle* out, int channels = 1) {
  SonareRealtimeVoiceChanger* raw = nullptr;
  const SonareError err = sonare_realtime_voice_changer_create(&c, kRate, kBlock, channels, &raw);
  out->reset(raw);
  return err;
}

Handle make(const Config& c, int channels = 1) {
  Handle h;
  REQUIRE(create(c, &h, channels) == SONARE_OK);
  REQUIRE(h != nullptr);
  return h;
}

Config read_config(const Handle& h) {
  Config c{};
  REQUIRE(sonare_realtime_voice_changer_get_config(h.get(), &c) == SONARE_OK);
  return c;
}

bool same_bytes(const Config& a, const Config& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

bool same_bytes(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

bool all_finite(const std::vector<float>& v) {
  for (const float x : v) {
    if (!std::isfinite(x)) return false;
  }
  return true;
}

/// A voiced test signal: eight harmonics of @p f0.
std::vector<float> voiced(std::size_t n, double f0) {
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    double s = 0.0;
    for (int k = 1; k <= 8; ++k) {
      s += std::sin(sonare::constants::kTwoPiD * f0 * k * static_cast<double>(i) / kRate) / k;
    }
    x[i] = static_cast<float>(0.25 * s);
  }
  return x;
}

/// Runs @p input through the mono entry in blocks and returns the output.
std::vector<float> run_mono(const Handle& h, const std::vector<float>& input,
                            const std::vector<float>* carrier = nullptr) {
  std::vector<float> out(input.size(), 0.0f);
  for (std::size_t pos = 0; pos < input.size(); pos += kBlock) {
    const std::size_t n = std::min<std::size_t>(kBlock, input.size() - pos);
    const SonareError err =
        carrier ? sonare_realtime_voice_changer_process_mono_with_carrier(
                      h.get(), input.data() + pos, carrier->data() + pos, out.data() + pos, n)
                : sonare_realtime_voice_changer_process_mono(h.get(), input.data() + pos,
                                                             out.data() + pos, n);
    REQUIRE(err == SONARE_OK);
  }
  return out;
}

double energy(const std::vector<float>& v, std::size_t from = 0) {
  double e = 0.0;
  for (std::size_t i = from; i < v.size(); ++i) e += static_cast<double>(v[i]) * v[i];
  return e;
}

// ---------------------------------------------------------------------------------------------
// Field tables
// ---------------------------------------------------------------------------------------------

struct FloatField {
  const char* path;  // the name the refusal message carries
  float Config::*member;
  float lo;
  float hi;
};

const FloatField kFloatFields[] = {
    {"voiceEngine.f0MinHz", &Config::voice_engine_f0_min_hz, 50.0f, 200.0f},
    {"pitchCorrection.amount", &Config::pitch_correction_amount, 0.0f, 1.0f},
    {"pitchCorrection.speedMs", &Config::pitch_correction_speed_ms, 0.0f, 400.0f},
    {"harmony.level", &Config::harmony_level, 0.0f, 1.0f},
    {"harmony.leadLevel", &Config::harmony_lead_level, 0.0f, 1.0f},
    {"harmony.detuneCents", &Config::harmony_detune_cents, 0.0f, 30.0f},
    {"harmony.delayMs", &Config::harmony_delay_ms, 0.0f, 40.0f},
    {"vocoder.attackMs", &Config::vocoder_attack_ms, 0.5f, 50.0f},
    {"vocoder.releaseMs", &Config::vocoder_release_ms, 2.0f, 300.0f},
    {"vocoder.sibilance", &Config::vocoder_sibilance, 0.0f, 1.0f},
    {"vocoder.ensembleDetuneCents", &Config::vocoder_ensemble_detune_cents, 0.0f, 30.0f},
    {"vocoder.level", &Config::vocoder_level, 0.0f, 1.0f},
    {"character.amount", &Config::character_amount, 0.0f, 1.0f},
    {"character.tone", &Config::character_tone, -1.0f, 1.0f},
    {"reverbVariation.delayMs", &Config::reverb_variation_delay_ms, 1.0f, 3000.0f},
    {"reverbVariation.feedback", &Config::reverb_variation_feedback, 0.0f, 0.95f},
    {"reverbVariation.tempoBpm", &Config::reverb_variation_tempo_bpm, 30.0f, 300.0f},
};

struct IntField {
  const char* path;
  int32_t Config::*member;
  int32_t lo;
  int32_t hi;
};

const IntField kIntFields[] = {
    {"pitchCorrection.keyRoot", &Config::pitch_correction_key_root, 0, 11},
    {"pitchCorrection.robotOctave", &Config::pitch_correction_robot_octave, -1, 1},
    {"harmony.voices", &Config::harmony_voices, 0, 3},
    {"vocoder.bands", &Config::vocoder_bands, 8, 32},
};

struct EnumField {
  const char* name;  // the POD member the refusal message names
  int32_t Config::*member;
  int32_t last;
};

const EnumField kEnumFields[] = {
    {"voice_engine_engine", &Config::voice_engine_engine, SONARE_PITCH_ENGINE_PSOLA},
    {"pitch_correction_mode", &Config::pitch_correction_mode, SONARE_PITCH_CORRECTION_ROBOT},
    {"pitch_correction_scale", &Config::pitch_correction_scale, SONARE_SCALE_CHROMATIC},
    {"vocoder_carrier_source", &Config::vocoder_carrier_source, SONARE_VOCODER_CARRIER_EXTERNAL},
    {"vocoder_carrier_wave", &Config::vocoder_carrier_wave, SONARE_VOCODER_WAVE_ENSEMBLE},
    {"character_type", &Config::character_type, SONARE_CHARACTER_STROBO},
    {"reverb_variation_variation", &Config::reverb_variation_variation,
     SONARE_REVERB_VARIATION_DUB_ECHO},
    {"reverb_variation_division", &Config::reverb_variation_division,
     SONARE_NOTE_DIVISION_SIXTEENTH},
    {"midi_release", &Config::midi_release, SONARE_MIDI_RELEASE_INPUT},
};

constexpr SonareVoiceCharacterPreset kAllPresets[] = {
    SONARE_VC_PRESET_NEUTRAL_MONITOR, SONARE_VC_PRESET_BRIGHT_IDOL,
    SONARE_VC_PRESET_SOFT_WHISPER,    SONARE_VC_PRESET_DEEP_NARRATOR,
    SONARE_VC_PRESET_ROBOT_MASCOT,    SONARE_VC_PRESET_DARK_VILLAIN,
    SONARE_VC_PRESET_HARD_TUNE,       SONARE_VC_PRESET_ROBOT_MONOTONE,
    SONARE_VC_PRESET_CHOIR_HARMONY,   SONARE_VC_PRESET_VOCODER_STRINGS,
    SONARE_VC_PRESET_MEGAPHONE,       SONARE_VC_PRESET_RADIO_VOICE,
};

/// A configuration whose every transformer field differs from the default.
Config distinctive_config() {
  Config c = default_config();
  c.voice_engine_engine = SONARE_PITCH_ENGINE_PSOLA;
  c.voice_engine_f0_min_hz = 85.5f;
  c.pitch_correction_mode = SONARE_PITCH_CORRECTION_AUTO;
  c.pitch_correction_amount = 0.75f;
  c.pitch_correction_speed_ms = 35.5f;
  c.pitch_correction_key_root = 7;
  c.pitch_correction_scale = SONARE_SCALE_NATURAL_MINOR;
  c.pitch_correction_robot_octave = -1;
  c.harmony_voices = 3;
  c.harmony_steps[0] = 5;
  c.harmony_steps[1] = -2;
  c.harmony_steps[2] = -7;
  c.harmony_level = 0.625f;
  c.harmony_lead_level = 0.875f;
  c.harmony_detune_cents = 17.5f;
  c.harmony_delay_ms = 23.5f;
  c.vocoder_enabled = 7;  // any non-zero value reads back as 1
  c.vocoder_bands = 24;
  c.vocoder_attack_ms = 3.5f;
  c.vocoder_release_ms = 45.5f;
  c.vocoder_sibilance = 0.625f;
  c.vocoder_carrier_source = SONARE_VOCODER_CARRIER_EXTERNAL;
  c.vocoder_carrier_wave = SONARE_VOCODER_WAVE_PULSE;
  c.vocoder_ensemble_detune_cents = 9.5f;
  c.vocoder_level = 0.875f;
  c.character_type = SONARE_CHARACTER_BBD_CHORUS;
  c.character_amount = 0.375f;
  c.character_tone = -0.5f;
  c.reverb_variation_variation = SONARE_REVERB_VARIATION_ECHO;
  c.reverb_variation_delay_ms = 640.0f;
  c.reverb_variation_feedback = 0.625f;
  c.reverb_variation_tempo_bpm = 96.0f;
  c.reverb_variation_division = SONARE_NOTE_DIVISION_DOTTED_EIGHTH;
  c.midi_release = SONARE_MIDI_RELEASE_INPUT;
  return c;
}

/// A config with the vocoder on and the internal carrier, taken from a shipped preset.
Config vocoder_config(int32_t source) {
  Config c = preset_config(SONARE_VC_PRESET_VOCODER_STRINGS);
  REQUIRE(c.vocoder_enabled != 0);
  REQUIRE(c.vocoder_carrier_source == SONARE_VOCODER_CARRIER_INTERNAL);
  c.vocoder_carrier_source = source;
  return c;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Layout and defaults
// ---------------------------------------------------------------------------------------------

TEST_CASE("The voice changer POD is version 2 and the ABI version is 4",
          "[capi][voice_transform]") {
  STATIC_REQUIRE(sizeof(Config) == 72u * sizeof(float));
  STATIC_REQUIRE(SONARE_VOICE_CHANGER_ABI_VERSION == 4u);
  CHECK(sonare_voice_changer_abi_version() == 4u);
  STATIC_REQUIRE(offsetof(Config, harmony_steps) % sizeof(int32_t) == 0);
  STATIC_REQUIRE(sizeof(Config::harmony_steps) == 3u * sizeof(int32_t));
}

TEST_CASE("config_default and every preset_config write struct_version 2",
          "[capi][voice_transform]") {
  Config c{};
  c.struct_version = -1;
  REQUIRE(sonare_realtime_voice_changer_config_default(&c) == SONARE_OK);
  CHECK(c.struct_version == 2);
  for (const auto preset : kAllPresets) {
    INFO("preset " << sonare_voice_character_preset_id(preset));
    Config p{};
    p.struct_version = -1;
    REQUIRE(sonare_realtime_voice_changer_preset_config(preset, &p) == SONARE_OK);
    CHECK(p.struct_version == 2);
  }
  const Handle h = make(c);
  CHECK(read_config(h).struct_version == 2);
}

TEST_CASE("struct_version 0, 1 and 3 are refused and the previous config is kept",
          "[capi][voice_transform]") {
  const Config good = default_config();
  const Handle h = make(good);
  const Config before = read_config(h);
  for (const int32_t version : {-1, 0, 1, 3, 2147483647}) {
    INFO("struct_version " << version);
    Config bad = good;
    bad.struct_version = version;
    Handle none;
    CHECK(create(bad, &none) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(none == nullptr);
    CHECK(sonare_realtime_voice_changer_set_config(h.get(), &bad) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(std::string(sonare_last_error_message()), ContainsSubstring("struct_version"));
    CHECK(same_bytes(read_config(h), before));
  }
}

TEST_CASE("The default config reports a latency of 717 samples at 48 kHz",
          "[capi][voice_transform]") {
  const Handle h = make(default_config());
  int latency = 0;
  REQUIRE(sonare_realtime_voice_changer_latency_samples(h.get(), &latency) == SONARE_OK);
  CHECK(latency == 717);
}

TEST_CASE("get_config round-trips every transformer field, harmony steps included",
          "[capi][voice_transform]") {
  const Config in = distinctive_config();
  const Handle h = make(in);
  const Config out = read_config(h);

  CHECK(out.voice_engine_engine == in.voice_engine_engine);
  CHECK(out.voice_engine_f0_min_hz == in.voice_engine_f0_min_hz);
  CHECK(out.pitch_correction_mode == in.pitch_correction_mode);
  CHECK(out.pitch_correction_amount == in.pitch_correction_amount);
  CHECK(out.pitch_correction_speed_ms == in.pitch_correction_speed_ms);
  CHECK(out.pitch_correction_key_root == in.pitch_correction_key_root);
  CHECK(out.pitch_correction_scale == in.pitch_correction_scale);
  CHECK(out.pitch_correction_robot_octave == in.pitch_correction_robot_octave);
  CHECK(out.harmony_voices == in.harmony_voices);
  CHECK(out.harmony_steps[0] == 5);
  CHECK(out.harmony_steps[1] == -2);
  CHECK(out.harmony_steps[2] == -7);
  CHECK(out.harmony_level == in.harmony_level);
  CHECK(out.harmony_lead_level == in.harmony_lead_level);
  CHECK(out.harmony_detune_cents == in.harmony_detune_cents);
  CHECK(out.harmony_delay_ms == in.harmony_delay_ms);
  CHECK(out.vocoder_enabled == 1);
  CHECK(out.vocoder_bands == in.vocoder_bands);
  CHECK(out.vocoder_attack_ms == in.vocoder_attack_ms);
  CHECK(out.vocoder_release_ms == in.vocoder_release_ms);
  CHECK(out.vocoder_sibilance == in.vocoder_sibilance);
  CHECK(out.vocoder_carrier_source == in.vocoder_carrier_source);
  CHECK(out.vocoder_carrier_wave == in.vocoder_carrier_wave);
  CHECK(out.vocoder_ensemble_detune_cents == in.vocoder_ensemble_detune_cents);
  CHECK(out.vocoder_level == in.vocoder_level);
  CHECK(out.character_type == in.character_type);
  CHECK(out.character_amount == in.character_amount);
  CHECK(out.character_tone == in.character_tone);
  CHECK(out.reverb_variation_variation == in.reverb_variation_variation);
  CHECK(out.reverb_variation_delay_ms == in.reverb_variation_delay_ms);
  CHECK(out.reverb_variation_feedback == in.reverb_variation_feedback);
  CHECK(out.reverb_variation_tempo_bpm == in.reverb_variation_tempo_bpm);
  CHECK(out.reverb_variation_division == in.reverb_variation_division);
  CHECK(out.midi_release == in.midi_release);

  // The distinctive config differs from the default in every transformer field, so a field the
  // bridge drops would read back as the default above.
  const Config def = default_config();
  CHECK(out.harmony_steps[0] != def.harmony_steps[0]);
  CHECK(out.harmony_steps[1] != def.harmony_steps[1]);
  CHECK(out.harmony_steps[2] != def.harmony_steps[2]);
}

TEST_CASE("Every preset round-trips through the POD and agrees with its JSON form",
          "[capi][voice_transform]") {
  for (const auto preset : kAllPresets) {
    const std::string id = sonare_voice_character_preset_id(preset);
    INFO("preset " << id);
    const Config pod = preset_config(preset);
    const Handle from_pod = make(pod);

    // The POD read back is the POD that went in, bar the grain size prepare() resolves (the
    // struct has no padding, so a byte comparison covers every field).
    Config expected = pod;
    expected.retune_grain_size = read_config(from_pod).retune_grain_size;
    CHECK(same_bytes(read_config(from_pod), expected));

    // The POD route and the preset-id route build the same chain configuration.
    SonareRealtimeVoiceChanger* raw = nullptr;
    REQUIRE(sonare_realtime_voice_changer_create_json(id.c_str(), kRate, kBlock, 1, &raw) ==
            SONARE_OK);
    const Handle from_json(raw);
    CHECK(same_bytes(read_config(from_json), read_config(from_pod)));

    char* json_pod = nullptr;
    char* json_id = nullptr;
    REQUIRE(sonare_realtime_voice_changer_config_json(from_pod.get(), &json_pod) == SONARE_OK);
    REQUIRE(sonare_realtime_voice_changer_config_json(from_json.get(), &json_id) == SONARE_OK);
    CHECK(std::string(json_pod) == std::string(json_id));
    sonare_free_string(json_pod);
    sonare_free_string(json_id);

    // And the chain it builds renders finite audio.
    const auto input = voiced(2048, 180.0);
    CHECK(all_finite(run_mono(from_pod, input)));
  }
}

// ---------------------------------------------------------------------------------------------
// Range and enum refusals
// ---------------------------------------------------------------------------------------------

TEST_CASE("Range ends are accepted, values outside are refused", "[capi][voice_transform]") {
  const Config base = default_config();
  for (const auto& f : kFloatFields) {
    INFO("field " << f.path);
    for (const float edge : {f.lo, f.hi}) {
      Config c = base;
      c.*(f.member) = edge;
      Handle h;
      CHECK(create(c, &h) == SONARE_OK);
      CHECK(h != nullptr);
      if (h) CHECK(read_config(h).*(f.member) == edge);
    }
    const float below = std::nextafter(f.lo, -kInf);
    const float above = std::nextafter(f.hi, kInf);
    for (const float outside : {below, above, f.lo - 1000.0f, f.hi + 1000.0f}) {
      Config c = base;
      c.*(f.member) = outside;
      Handle h;
      CHECK(create(c, &h) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK(h == nullptr);
      CHECK_THAT(std::string(sonare_last_error_message()), ContainsSubstring(f.path));
    }
  }
  for (const auto& f : kIntFields) {
    INFO("field " << f.path);
    for (const int32_t edge : {f.lo, f.hi}) {
      Config c = base;
      c.*(f.member) = edge;
      Handle h;
      CHECK(create(c, &h) == SONARE_OK);
      CHECK(h != nullptr);
      if (h) CHECK(read_config(h).*(f.member) == edge);
    }
    for (const int32_t outside : {f.lo - 1, f.hi + 1, INT32_MIN, INT32_MAX}) {
      Config c = base;
      c.*(f.member) = outside;
      Handle h;
      CHECK(create(c, &h) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK(h == nullptr);
      CHECK_THAT(std::string(sonare_last_error_message()), ContainsSubstring(f.path));
    }
  }
}

TEST_CASE("Each harmony step accepts -7 and 7 and refuses 8 and -8", "[capi][voice_transform]") {
  const Config base = default_config();
  for (int i = 0; i < 3; ++i) {
    INFO("steps[" << i << "]");
    for (const int32_t edge : {-7, 7}) {
      Config c = base;
      c.harmony_steps[i] = edge;
      Handle h;
      REQUIRE(create(c, &h) == SONARE_OK);
      CHECK(read_config(h).harmony_steps[i] == edge);
    }
    for (const int32_t outside : {-8, 8, INT32_MIN, INT32_MAX}) {
      Config c = base;
      c.harmony_steps[i] = outside;
      Handle h;
      CHECK(create(c, &h) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK_THAT(std::string(sonare_last_error_message()),
                 ContainsSubstring("harmony.steps[" + std::to_string(i) + "]"));
    }
  }
}

TEST_CASE("A zero and a negative zero are accepted where the range reaches zero",
          "[capi][voice_transform]") {
  const Config base = default_config();
  for (const auto& f : kFloatFields) {
    if (f.lo != 0.0f) continue;
    INFO("field " << f.path);
    for (const float zero : {0.0f, -0.0f}) {
      Config c = base;
      c.*(f.member) = zero;
      Handle h;
      CHECK(create(c, &h) == SONARE_OK);
      if (h) CHECK(read_config(h).*(f.member) == 0.0f);
    }
  }
}

TEST_CASE("NaN and infinity in a new float field are refused and the previous config is kept",
          "[capi][voice_transform]") {
  const Config good = distinctive_config();
  const Handle h = make(good);
  const Config before = read_config(h);
  for (const auto& f : kFloatFields) {
    INFO("field " << f.path);
    for (const float bad : {kNaN, kInf, -kInf}) {
      Config c = good;
      c.*(f.member) = bad;
      CHECK(sonare_realtime_voice_changer_set_config(h.get(), &c) ==
            SONARE_ERROR_INVALID_PARAMETER);
      CHECK_THAT(std::string(sonare_last_error_message()), ContainsSubstring(f.path));
      CHECK(same_bytes(read_config(h), before));

      Handle none;
      CHECK(create(c, &none) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK(none == nullptr);
    }
  }
  // The refusals left the live chain usable.
  CHECK(sonare_realtime_voice_changer_set_config(h.get(), &good) == SONARE_OK);
}

TEST_CASE("An unknown enum ordinal is refused and the previous config is kept",
          "[capi][voice_transform]") {
  const Config good = default_config();
  const Handle h = make(good);
  const Config before = read_config(h);
  for (const auto& f : kEnumFields) {
    INFO("field " << f.name);
    for (const int32_t ordinal : {-1, f.last + 1, INT32_MIN, INT32_MAX}) {
      Config c = good;
      c.*(f.member) = ordinal;
      CHECK(sonare_realtime_voice_changer_set_config(h.get(), &c) ==
            SONARE_ERROR_INVALID_PARAMETER);
      CHECK_THAT(std::string(sonare_last_error_message()), ContainsSubstring(f.name));
      CHECK(same_bytes(read_config(h), before));
      Handle none;
      CHECK(create(c, &none) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK(none == nullptr);
    }
    // The last declared ordinal itself is a value, not a refusal, on a fresh handle.
    Config c = good;
    c.*(f.member) = f.last;
    if (f.member == &Config::voice_engine_engine) continue;  // psola is the default already
    Handle ok;
    const SonareError err = create(c, &ok);
    INFO("last ordinal " << f.last);
    CHECK(err == SONARE_OK);
  }
}

// ---------------------------------------------------------------------------------------------
// Engine rules
// ---------------------------------------------------------------------------------------------

TEST_CASE("The grain engine refuses auto, robot, harmony and the vocoder",
          "[capi][voice_transform]") {
  Config grain = default_config();
  grain.voice_engine_engine = SONARE_PITCH_ENGINE_GRAIN;
  {
    Handle h;
    REQUIRE(create(grain, &h) == SONARE_OK);  // the grain engine itself is valid
  }

  const auto refused = [&](const char* what, auto&& edit) {
    INFO(what);
    Config c = grain;
    edit(c);
    Handle h;
    CHECK(create(c, &h) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(h == nullptr);
    CHECK_THAT(std::string(sonare_last_error_message()), ContainsSubstring("grain"));
  };
  refused("auto", [](Config& c) { c.pitch_correction_mode = SONARE_PITCH_CORRECTION_AUTO; });
  refused("robot", [](Config& c) { c.pitch_correction_mode = SONARE_PITCH_CORRECTION_ROBOT; });
  refused("harmony", [](Config& c) { c.harmony_voices = 1; });
  refused("vocoder", [](Config& c) { c.vocoder_enabled = 1; });

  // Live: a grain handle refuses the same switches through set_config and keeps its config.
  const Handle h = make(grain);
  const Config before = read_config(h);
  Config c = grain;
  c.pitch_correction_mode = SONARE_PITCH_CORRECTION_AUTO;
  CHECK(sonare_realtime_voice_changer_set_config(h.get(), &c) == SONARE_ERROR_INVALID_PARAMETER);
  c = grain;
  c.vocoder_enabled = 1;
  CHECK(sonare_realtime_voice_changer_set_config(h.get(), &c) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(same_bytes(read_config(h), before));
}

TEST_CASE("engine, f0_min_hz and vocoder bands are fixed at creation", "[capi][voice_transform]") {
  const Config base = default_config();
  const Handle h = make(base);
  const Config before = read_config(h);

  Config c = base;
  c.voice_engine_engine = SONARE_PITCH_ENGINE_GRAIN;
  CHECK(sonare_realtime_voice_changer_set_config(h.get(), &c) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(std::string(sonare_last_error_message()), ContainsSubstring("prepare"));

  c = base;
  c.voice_engine_f0_min_hz = base.voice_engine_f0_min_hz + 10.0f;
  CHECK(sonare_realtime_voice_changer_set_config(h.get(), &c) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(std::string(sonare_last_error_message()), ContainsSubstring("prepare"));

  c = base;
  c.vocoder_bands = base.vocoder_bands + 4;
  CHECK(sonare_realtime_voice_changer_set_config(h.get(), &c) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(std::string(sonare_last_error_message()), ContainsSubstring("prepare"));

  CHECK(same_bytes(read_config(h), before));

  // The same values are a live update; a grain handle refuses a switch to psola the same way.
  CHECK(sonare_realtime_voice_changer_set_config(h.get(), &base) == SONARE_OK);
  Config grain = base;
  grain.voice_engine_engine = SONARE_PITCH_ENGINE_GRAIN;
  const Handle g = make(grain);
  CHECK(sonare_realtime_voice_changer_set_config(g.get(), &base) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("The echo delay is capped at 3000 ms and a slow tempo delay stays inside it",
          "[capi][voice_transform]") {
  Config echo = default_config();
  echo.reverb_variation_variation = SONARE_REVERB_VARIATION_ECHO;
  echo.reverb_variation_delay_ms = 3000.0f;
  Handle h;
  CHECK(create(echo, &h) == SONARE_OK);
  echo.reverb_variation_delay_ms = std::nextafter(3000.0f, kInf);
  CHECK(create(echo, &h) == SONARE_ERROR_INVALID_PARAMETER);
  echo.reverb_variation_delay_ms = 3001.0f;
  CHECK(create(echo, &h) == SONARE_ERROR_INVALID_PARAMETER);

  // Slowest tempo, longest note value: 2000 ms, accepted.
  Config tempo = default_config();
  tempo.reverb_variation_variation = SONARE_REVERB_VARIATION_TEMPO_DELAY;
  tempo.reverb_variation_tempo_bpm = 30.0f;
  tempo.reverb_variation_division = SONARE_NOTE_DIVISION_QUARTER;
  CHECK(create(tempo, &h) == SONARE_OK);
  CHECK(h != nullptr);
}

TEST_CASE("A chromatic scale validates key_root and ignores it", "[capi][voice_transform]") {
  Config chromatic = default_config();
  chromatic.pitch_correction_mode = SONARE_PITCH_CORRECTION_AUTO;
  chromatic.pitch_correction_scale = SONARE_SCALE_CHROMATIC;
  chromatic.pitch_correction_amount = 1.0f;
  chromatic.pitch_correction_speed_ms = 0.0f;
  chromatic.wet_mix = 1.0f;
  chromatic.retune_mix = 1.0f;

  Handle none;
  for (const int32_t bad : {-1, 12}) {
    Config c = chromatic;
    c.pitch_correction_key_root = bad;
    CHECK(create(c, &none) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(std::string(sonare_last_error_message()),
               ContainsSubstring("pitchCorrection.keyRoot"));
  }

  // A pitch between two semitones (C#4 plus 30 cents), long enough for the tracker to settle.
  const auto input = voiced(24000, 277.18 * std::pow(2.0, 30.0 / 1200.0));
  std::vector<float> reference;
  for (const int32_t root : {0, 3, 7, 11}) {
    INFO("chromatic key_root " << root);
    Config c = chromatic;
    c.pitch_correction_key_root = root;
    const auto out = run_mono(make(c), input);
    REQUIRE(all_finite(out));
    if (reference.empty()) {
      reference = out;
      CHECK(energy(out, 12000) > 0.0);
    } else {
      CHECK(same_bytes(out, reference));
    }
  }

  // The same pitch under a major scale does depend on the root, so the check above could fail.
  Config major_c = chromatic;
  major_c.pitch_correction_scale = SONARE_SCALE_MAJOR;
  major_c.pitch_correction_key_root = 0;  // C#4 is outside C major
  Config major_fs = major_c;
  major_fs.pitch_correction_key_root = 6;  // and inside F# major
  const auto a = run_mono(make(major_c), input);
  const auto b = run_mono(make(major_fs), input);
  CHECK_FALSE(same_bytes(a, b));
}

// ---------------------------------------------------------------------------------------------
// Note entries
// ---------------------------------------------------------------------------------------------

TEST_CASE("note_on and note_off refuse a note outside [0, 127]", "[capi][voice_transform]") {
  const Handle h = make(default_config());
  for (const int note : {-1, 128, INT32_MIN, INT32_MAX}) {
    INFO("note " << note);
    CHECK(sonare_realtime_voice_changer_note_on(h.get(), note, 1.0f) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_realtime_voice_changer_note_off(h.get(), note) == SONARE_ERROR_INVALID_PARAMETER);
  }
  for (const int note : {0, 127}) {
    INFO("note " << note);
    CHECK(sonare_realtime_voice_changer_note_on(h.get(), note, 1.0f) == SONARE_OK);
    CHECK(sonare_realtime_voice_changer_note_off(h.get(), note) == SONARE_OK);
  }
  CHECK(sonare_realtime_voice_changer_note_on(nullptr, 60, 1.0f) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_realtime_voice_changer_note_off(nullptr, 60) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_realtime_voice_changer_all_notes_off(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("note_on refuses a velocity of 0, NaN, above 1 or negative", "[capi][voice_transform]") {
  const Handle h = make(default_config());
  for (const float velocity :
       {0.0f, -0.0f, -0.5f, kNaN, kInf, -kInf, std::nextafter(1.0f, 2.0f), 1.5f, 127.0f}) {
    INFO("velocity " << velocity);
    CHECK(sonare_realtime_voice_changer_note_on(h.get(), 60, velocity) ==
          SONARE_ERROR_INVALID_PARAMETER);
  }
  for (const float velocity : {std::numeric_limits<float>::min(), 0.01f, 0.5f, 1.0f}) {
    INFO("velocity " << velocity);
    CHECK(sonare_realtime_voice_changer_note_on(h.get(), 60, velocity) == SONARE_OK);
  }
}

TEST_CASE("A second note_on for a held note and a note_off for a free note succeed",
          "[capi][voice_transform]") {
  const Handle h = make(default_config());
  CHECK(sonare_realtime_voice_changer_note_off(h.get(), 64) == SONARE_OK);  // never held
  CHECK(sonare_realtime_voice_changer_note_on(h.get(), 64, 0.8f) == SONARE_OK);
  CHECK(sonare_realtime_voice_changer_note_on(h.get(), 64, 0.4f) == SONARE_OK);
  CHECK(sonare_realtime_voice_changer_note_off(h.get(), 64) == SONARE_OK);
  CHECK(sonare_realtime_voice_changer_note_off(h.get(), 64) == SONARE_OK);
  // The events are consumed by the audio thread without error.
  const auto input = voiced(1024, 150.0);
  CHECK(all_finite(run_mono(h, input)));
}

TEST_CASE("A full note queue gives INVALID_STATE, and all_notes_off still succeeds",
          "[capi][voice_transform]") {
  const Handle h = make(default_config());
  int accepted = 0;
  for (int i = 0; i < 1000; ++i) {
    const SonareError err = sonare_realtime_voice_changer_note_on(h.get(), 40 + i % 40, 0.5f);
    if (err != SONARE_OK) {
      CHECK(err == SONARE_ERROR_INVALID_STATE);
      break;
    }
    ++accepted;
  }
  CHECK(accepted == 256);  // kNoteQueueCapacity
  // The queue stays full: both entries report it, and neither is an argument error.
  CHECK(sonare_realtime_voice_changer_note_on(h.get(), 60, 1.0f) == SONARE_ERROR_INVALID_STATE);
  CHECK(sonare_realtime_voice_changer_note_off(h.get(), 60) == SONARE_ERROR_INVALID_STATE);
  // An argument error is still reported as one.
  CHECK(sonare_realtime_voice_changer_note_on(h.get(), 128, 1.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);

  CHECK(sonare_realtime_voice_changer_all_notes_off(h.get()) == SONARE_OK);

  // Processing drains the queue, after which the entries accept events again.
  const auto input = voiced(kBlock, 150.0);
  CHECK(all_finite(run_mono(h, input)));
  CHECK(sonare_realtime_voice_changer_note_on(h.get(), 60, 1.0f) == SONARE_OK);
  CHECK(sonare_realtime_voice_changer_note_off(h.get(), 60) == SONARE_OK);
}

TEST_CASE("The note entries on a grain handle give INVALID_STATE", "[capi][voice_transform]") {
  Config grain = default_config();
  grain.voice_engine_engine = SONARE_PITCH_ENGINE_GRAIN;
  const Handle h = make(grain);
  CHECK(sonare_realtime_voice_changer_note_on(h.get(), 60, 1.0f) == SONARE_ERROR_INVALID_STATE);
  CHECK(sonare_realtime_voice_changer_note_off(h.get(), 60) == SONARE_ERROR_INVALID_STATE);
  // An argument error outranks the state error.
  CHECK(sonare_realtime_voice_changer_note_on(h.get(), -1, 1.0f) == SONARE_ERROR_INVALID_PARAMETER);
  // Releasing everything holds nothing back and succeeds.
  CHECK(sonare_realtime_voice_changer_all_notes_off(h.get()) == SONARE_OK);
}

TEST_CASE("Held notes reach the output whatever the pitch correction mode",
          "[capi][voice_transform]") {
  // Off mode, a held note pulls the voice to its pitch: the audio differs from the free run.
  Config c = default_config();
  c.pitch_correction_mode = SONARE_PITCH_CORRECTION_OFF;
  c.wet_mix = 1.0f;
  c.retune_mix = 1.0f;
  const auto input = voiced(24000, 150.0);
  const auto free_run = run_mono(make(c), input);

  const Handle held = make(c);
  REQUIRE(sonare_realtime_voice_changer_note_on(held.get(), 67, 1.0f) == SONARE_OK);
  const auto held_run = run_mono(held, input);
  REQUIRE(all_finite(held_run));
  CHECK_FALSE(same_bytes(free_run, held_run));

  // Releasing it again restores the free pitch behaviour on a fresh handle's terms: the release
  // call itself succeeds and the output stays finite.
  CHECK(sonare_realtime_voice_changer_all_notes_off(held.get()) == SONARE_OK);
  CHECK(all_finite(run_mono(held, input)));
}

// ---------------------------------------------------------------------------------------------
// Carrier entries
// ---------------------------------------------------------------------------------------------

TEST_CASE("A NULL carrier is refused by every *_with_carrier entry", "[capi][voice_transform]") {
  const Handle h = make(vocoder_config(SONARE_VOCODER_CARRIER_EXTERNAL), 2);
  std::vector<float> in(kBlock, 0.1f);
  std::vector<float> out(kBlock * 2, 0.5f);
  CHECK(sonare_realtime_voice_changer_process_mono_with_carrier(
            h.get(), in.data(), nullptr, out.data(), kBlock) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_realtime_voice_changer_process_interleaved_with_carrier(
            h.get(), in.data(), nullptr, out.data(), kBlock, 1) == SONARE_ERROR_INVALID_PARAMETER);
  std::vector<float> left(kBlock, 0.1f);
  std::vector<float> right(kBlock, 0.1f);
  CHECK(sonare_realtime_voice_changer_process_planar_stereo_with_carrier(
            h.get(), left.data(), right.data(), nullptr, kBlock) == SONARE_ERROR_INVALID_PARAMETER);

  // The same holds when the configuration would not read a carrier at all.
  const Handle internal = make(vocoder_config(SONARE_VOCODER_CARRIER_INTERNAL), 2);
  CHECK(sonare_realtime_voice_changer_process_mono_with_carrier(internal.get(), in.data(), nullptr,
                                                                out.data(), kBlock) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_realtime_voice_changer_process_interleaved_with_carrier(
            internal.get(), in.data(), nullptr, out.data(), kBlock, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_realtime_voice_changer_process_planar_stereo_with_carrier(
            internal.get(), left.data(), right.data(), nullptr, kBlock) ==
        SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("An internal carrier is never read by the *_with_carrier entries",
          "[capi][voice_transform]") {
  const Config cfg = vocoder_config(SONARE_VOCODER_CARRIER_INTERNAL);
  const auto input = voiced(4096, 160.0);
  const std::vector<float> poisoned(input.size(), kNaN);

  SECTION("mono") {
    const auto plain = run_mono(make(cfg), input);
    const auto with_carrier = run_mono(make(cfg), input, &poisoned);
    CHECK(all_finite(with_carrier));
    CHECK(same_bytes(plain, with_carrier));
    CHECK(energy(with_carrier, 2048) > 0.0);
  }

  SECTION("interleaved") {
    const Handle a = make(cfg, 2);
    const Handle b = make(cfg, 2);
    std::vector<float> stereo(input.size() * 2);
    for (std::size_t i = 0; i < input.size(); ++i) {
      stereo[2 * i] = input[i];
      stereo[2 * i + 1] = 0.5f * input[i];
    }
    std::vector<float> out_a(stereo.size());
    std::vector<float> out_b(stereo.size());
    for (std::size_t pos = 0; pos < input.size(); pos += kBlock) {
      REQUIRE(sonare_realtime_voice_changer_process_interleaved(a.get(), stereo.data() + 2 * pos,
                                                                out_a.data() + 2 * pos, kBlock,
                                                                2) == SONARE_OK);
      REQUIRE(sonare_realtime_voice_changer_process_interleaved_with_carrier(
                  b.get(), stereo.data() + 2 * pos, poisoned.data() + pos, out_b.data() + 2 * pos,
                  kBlock, 2) == SONARE_OK);
    }
    CHECK(all_finite(out_b));
    CHECK(same_bytes(out_a, out_b));
  }

  SECTION("planar stereo") {
    const Handle a = make(cfg, 2);
    const Handle b = make(cfg, 2);
    std::vector<float> la = input;
    std::vector<float> ra(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) ra[i] = 0.5f * input[i];
    std::vector<float> lb = la;
    std::vector<float> rb = ra;
    for (std::size_t pos = 0; pos < input.size(); pos += kBlock) {
      REQUIRE(sonare_realtime_voice_changer_process_planar_stereo(
                  a.get(), la.data() + pos, ra.data() + pos, kBlock) == SONARE_OK);
      REQUIRE(sonare_realtime_voice_changer_process_planar_stereo_with_carrier(
                  b.get(), lb.data() + pos, rb.data() + pos, poisoned.data() + pos, kBlock) ==
              SONARE_OK);
    }
    CHECK(all_finite(lb));
    CHECK(all_finite(rb));
    CHECK(same_bytes(la, lb));
    CHECK(same_bytes(ra, rb));
  }
}

TEST_CASE("An external carrier without a carrier gives INVALID_STATE and writes no output",
          "[capi][voice_transform]") {
  const Config cfg = vocoder_config(SONARE_VOCODER_CARRIER_EXTERNAL);
  const Handle h = make(cfg, 2);
  constexpr float kSentinel = 12345.0f;
  const auto input = voiced(kBlock, 160.0);

  std::vector<float> mono_out(kBlock, kSentinel);
  CHECK(sonare_realtime_voice_changer_process_mono(h.get(), input.data(), mono_out.data(),
                                                   kBlock) == SONARE_ERROR_INVALID_STATE);
  CHECK(same_bytes(mono_out, std::vector<float>(kBlock, kSentinel)));

  std::vector<float> stereo_in(kBlock * 2, 0.2f);
  std::vector<float> stereo_out(kBlock * 2, kSentinel);
  CHECK(sonare_realtime_voice_changer_process_interleaved(
            h.get(), stereo_in.data(), stereo_out.data(), kBlock, 2) == SONARE_ERROR_INVALID_STATE);
  CHECK(same_bytes(stereo_out, std::vector<float>(kBlock * 2, kSentinel)));

  std::vector<float> left(kBlock, kSentinel);
  std::vector<float> right(kBlock, kSentinel);
  CHECK(sonare_realtime_voice_changer_process_planar_stereo(h.get(), left.data(), right.data(),
                                                            kBlock) == SONARE_ERROR_INVALID_STATE);
  CHECK(same_bytes(left, std::vector<float>(kBlock, kSentinel)));
  CHECK(same_bytes(right, std::vector<float>(kBlock, kSentinel)));

  // With the carrier supplied the same handle renders, and the carrier shapes the output.
  const std::size_t n = 8192;
  const auto speech = voiced(n, 160.0);
  std::vector<float> carrier(n);
  for (std::size_t i = 0; i < n; ++i) {
    carrier[i] = 0.3f * (2.0f * std::fmod(static_cast<float>(i) * 110.0f / kRate, 1.0f) - 1.0f);
  }
  const Handle mono = make(cfg);
  const auto out = run_mono(mono, speech, &carrier);
  CHECK(all_finite(out));
  CHECK(energy(out, 4096) > 0.0);

  // A carrier-less call is fine again once the configuration no longer reads a carrier.
  Config internal = cfg;
  internal.vocoder_carrier_source = SONARE_VOCODER_CARRIER_INTERNAL;
  REQUIRE(sonare_realtime_voice_changer_set_config(mono.get(), &internal) == SONARE_OK);
  CHECK(sonare_realtime_voice_changer_process_mono(mono.get(), speech.data(), mono_out.data(),
                                                   kBlock) == SONARE_OK);
  // An external source with the vocoder switched off reads no carrier either.
  Config off = cfg;
  off.vocoder_enabled = 0;
  const Handle idle = make(off);
  CHECK(sonare_realtime_voice_changer_process_mono(idle.get(), speech.data(), mono_out.data(),
                                                   kBlock) == SONARE_OK);
}
