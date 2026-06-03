/// @file voice_changer_preset_capi_test.cpp
/// @brief Voice changer preset, JSON, and C API tests.

#include "voice_changer_test_helpers.h"

TEST_CASE("Factory presets JSON matches in-code definitions", "[voice_changer][preset-golden]") {
  std::ifstream file("schemas/realtime-voice-changer-presets.example.json");
  if (!file.is_open()) {
    // CMAKE_SOURCE_DIR-relative path missing usually means the test binary was
    // run with a different working directory (e.g. ctest in a build subdir).
    // Skip with a clear hint instead of failing on environment.
    WARN("Skipping golden test: schemas/realtime-voice-changer-presets.example.json not found");
    return;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  const auto root = sonare::util::json::parse(buffer.str());
  REQUIRE(root.is_object());
  REQUIRE(root["packId"].as_string() == "factory");
  REQUIRE(root["schemaVersion"].as_int() == 1);
  const auto& presets = root["presets"].as_array();
  REQUIRE(presets.size() == 6);

  auto field = [](const sonare::util::json::Value& obj, const char* key) -> float {
    return obj[key].as_float();
  };

  for (const auto& preset_json : presets) {
    const std::string id = preset_json["id"].as_string();
    const auto preset = realtime_voice_changer_preset_from_id(id);
    const auto code = realtime_voice_changer_preset(preset);
    const auto& dsp = preset_json["dsp"];

    INFO("Preset id=" << id);
    REQUIRE(field(dsp, "inputGainDb") == code.input_gain_db);
    REQUIRE(field(dsp, "outputGainDb") == code.output_gain_db);
    REQUIRE(field(dsp, "wetMix") == code.wet_mix);

    const auto& retune = dsp["retune"];
    REQUIRE(field(retune, "semitones") == code.retune.semitones);
    REQUIRE(field(retune, "mix") == code.retune.mix);
    REQUIRE(retune["grainSize"].as_int() == code.retune.grain_size);

    const auto& formant = dsp["formant"];
    REQUIRE(field(formant, "factor") == code.formant.factor);
    REQUIRE(field(formant, "amount") == code.formant.amount);
    REQUIRE(field(formant, "body") == code.formant.body);
    REQUIRE(field(formant, "brightness") == code.formant.brightness);
    REQUIRE(field(formant, "nasal") == code.formant.nasal);

    const auto& eq = dsp["eq"];
    REQUIRE(field(eq, "highpassHz") == code.eq.highpass_hz);
    REQUIRE(field(eq, "bodyDb") == code.eq.body_db);
    REQUIRE(field(eq, "presenceDb") == code.eq.presence_db);
    REQUIRE(field(eq, "airDb") == code.eq.air_db);

    const auto& gate = dsp["gate"];
    REQUIRE(field(gate, "thresholdDb") == code.gate.threshold_db);
    REQUIRE(field(gate, "attackMs") == code.gate.attack_ms);
    REQUIRE(field(gate, "releaseMs") == code.gate.release_ms);
    REQUIRE(field(gate, "rangeDb") == code.gate.range_db);

    const auto& compressor = dsp["compressor"];
    REQUIRE(field(compressor, "thresholdDb") == code.compressor.threshold_db);
    REQUIRE(field(compressor, "ratio") == code.compressor.ratio);
    REQUIRE(field(compressor, "attackMs") == code.compressor.attack_ms);
    REQUIRE(field(compressor, "releaseMs") == code.compressor.release_ms);
    REQUIRE(field(compressor, "makeupGainDb") == code.compressor.makeup_gain_db);

    const auto& deesser = dsp["deesser"];
    REQUIRE(field(deesser, "frequencyHz") == code.deesser.frequency_hz);
    REQUIRE(field(deesser, "thresholdDb") == code.deesser.threshold_db);
    REQUIRE(field(deesser, "ratio") == code.deesser.ratio);
    REQUIRE(field(deesser, "rangeDb") == code.deesser.range_db);

    const auto& reverb = dsp["reverb"];
    REQUIRE(field(reverb, "mix") == code.reverb.mix);
    REQUIRE(field(reverb, "timeMs") == code.reverb.time_ms);
    REQUIRE(field(reverb, "damping") == code.reverb.damping);
    REQUIRE(reverb["seed"].as_int() == code.reverb.seed);

    const auto& limiter = dsp["limiter"];
    REQUIRE(field(limiter, "ceilingDb") == code.limiter.ceiling_db);
    REQUIRE(field(limiter, "releaseMs") == code.limiter.release_ms);
  }
}

TEST_CASE("Voice changer schemaVersion constant matches emitted JSON", "[voice_changer][json]") {
  // The constant in realtime_voice_changer.h is the single source of truth for
  // the preset schema version. Any change here MUST be accompanied by an update
  // of the JSON Schema files under schemas/ and by a binding-side migration.
  REQUIRE(kVoiceChangerPresetSchemaVersion == 1);

  const auto json = realtime_voice_changer_preset_json(VoiceCharacterPreset::NeutralMonitor);
  const auto root = sonare::util::json::parse(json);
  REQUIRE(root["schemaVersion"].as_int() == kVoiceChangerPresetSchemaVersion);

  // The validator must also accept the current constant value end-to-end.
  std::string normalized;
  std::string error;
  REQUIRE(validate_realtime_voice_changer_preset_json(json, &normalized, &error));
  REQUIRE(error.empty());
}

TEST_CASE("Voice changer schemaVersion validator rejects future versions",
          "[voice_changer][json]") {
  // Replace the schemaVersion in a known-good preset with an unsupported value.
  // The validator should reject it rather than silently accepting (bindings
  // depend on this to refuse incompatible third-party preset packs).
  const auto baseline = realtime_voice_changer_preset_json(VoiceCharacterPreset::NeutralMonitor);
  const std::string needle = "\"schemaVersion\":1";
  REQUIRE(baseline.find(needle) != std::string::npos);
  std::string bumped = baseline;
  const auto pos = bumped.find(needle);
  bumped.replace(pos, needle.size(), "\"schemaVersion\":2");

  std::string normalized;
  std::string error;
  REQUIRE_FALSE(validate_realtime_voice_changer_preset_json(bumped, &normalized, &error));
  REQUIRE(error.find("schemaVersion") != std::string::npos);
}

TEST_CASE("preset JSON validator rejects invalid id patterns", "[voice_changer][json]") {
  // The JSON Schema declares `id` as `^[a-z0-9][a-z0-9._-]*$` with length 1..96.
  // The C++ validator used to accept anything non-empty up to 96 bytes, which
  // let uppercase / whitespace / punctuation slip through and break downstream
  // consumers that treat the id as a TS enum literal or comparison key.
  const auto baseline = realtime_voice_changer_preset_json(VoiceCharacterPreset::NeutralMonitor);
  const std::string id_needle = "\"id\":\"neutral-monitor\"";
  REQUIRE(baseline.find(id_needle) != std::string::npos);

  // 97-char id: max length is 96, so this must be rejected.
  const std::string too_long(97, 'a');
  const std::array<std::string, 9> bad_ids = {
      std::string(""),
      std::string("ID-with-Uppercase"),
      std::string("has space"),
      too_long,
      std::string("_underscore-start"),
      std::string(".dot-start"),
      std::string("-dash-start"),
      std::string("preset!"),
      std::string("name#sigil"),
  };
  for (const auto& id : bad_ids) {
    INFO("bad id=\"" << id << "\"");
    std::string bumped = baseline;
    const auto pos = bumped.find(id_needle);
    bumped.replace(pos, id_needle.size(), "\"id\":\"" + id + "\"");

    std::string error;
    REQUIRE_FALSE(validate_realtime_voice_changer_preset_json(bumped, nullptr, &error));
    REQUIRE_FALSE(error.empty());
    // Empty / oversized ids are caught by require_string's length check before
    // the regex check, so accept either error path provided the field is named.
    REQUIRE(error.find("$.id") != std::string::npos);
  }
}

TEST_CASE("preset JSON validator rejects non-integer schemaVersion", "[voice_changer][json]") {
  // as_int() silently truncates 1.5 -> 1, so the old `schema->as_int() == 1`
  // check let fractional values through. Also exercises the type-check path
  // when schemaVersion is a string rather than a number.
  const auto baseline = realtime_voice_changer_preset_json(VoiceCharacterPreset::NeutralMonitor);
  const std::string needle = "\"schemaVersion\":1";
  REQUIRE(baseline.find(needle) != std::string::npos);

  {
    std::string fractional = baseline;
    fractional.replace(fractional.find(needle), needle.size(), "\"schemaVersion\":1.5");
    std::string error;
    REQUIRE_FALSE(validate_realtime_voice_changer_preset_json(fractional, nullptr, &error));
    REQUIRE(error.find("schemaVersion") != std::string::npos);
  }
  {
    std::string string_version = baseline;
    string_version.replace(string_version.find(needle), needle.size(), "\"schemaVersion\":\"1\"");
    std::string error;
    REQUIRE_FALSE(validate_realtime_voice_changer_preset_json(string_version, nullptr, &error));
    REQUIRE(error.find("schemaVersion") != std::string::npos);
  }
}

TEST_CASE("preset JSON validator rejects duplicate top-level keys", "[voice_changer][json]") {
  // The strict validator uses util::json::parse_strict, which rejects objects
  // with repeated keys. A preset document with two `"id"` (or any other)
  // entries is almost certainly a user-config bug rather than a legitimate
  // "last-write-wins" override — fail fast with a clear error.
  const auto baseline = realtime_voice_changer_preset_json(VoiceCharacterPreset::NeutralMonitor);
  const std::string id_needle = "\"id\":\"neutral-monitor\"";
  REQUIRE(baseline.find(id_needle) != std::string::npos);
  // Splice in a second "id" key right after the first so the document is
  // syntactically valid JSON but contains a duplicate.
  std::string with_dup_id = baseline;
  with_dup_id.replace(with_dup_id.find(id_needle), id_needle.size(),
                      id_needle + ",\"id\":\"second-id\"");

  std::string normalized;
  std::string error;
  REQUIRE_FALSE(validate_realtime_voice_changer_preset_json(with_dup_id, &normalized, &error));
  REQUIRE_FALSE(error.empty());
  // The JsonError message uses the word "duplicate" — guard against a regression
  // back to the lenient `parse` entry point, which would silently accept this.
  REQUIRE(error.find("duplicate") != std::string::npos);
}

TEST_CASE("StreamingReverb passes dry input through when mix is zero", "[voice_changer][reverb]") {
  // mix == 0 must short-circuit the comb/allpass network so the output is
  // bit-identical to the input. This guards the audio thread against any
  // accidental state mutation when the reverb is "off" via mix only.
  StreamingReverb reverb;
  reverb.prepare(48000.0, 256);
  reverb.set_config({/*mix=*/0.0f, /*time_ms=*/200.0f, /*damping=*/0.5f, /*seed=*/1});
  for (int i = 0; i < 64; ++i) {
    const float x = std::sin(static_cast<float>(i) * 0.1f);
    REQUIRE(reverb.process_sample(x) == x);
  }
}

TEST_CASE("StreamingReverb decorrelates left and right channels via channel_index seed",
          "[voice_changer][reverb]") {
  // Same config, different channel_index — the tails must diverge thanks to
  // the kChannelSeedSalt XOR applied to the seed inside set_config.
  StreamingReverbConfig config{/*mix=*/0.45f, /*time_ms=*/200.0f, /*damping=*/0.5f, /*seed=*/7};

  StreamingReverb left, right;
  left.prepare(48000.0, 1);
  right.prepare(48000.0, 1);
  left.set_config(config, 0);
  right.set_config(config, 1);

  constexpr int kBlock = 32768;
  std::vector<float> input(kBlock, 0.0f);
  input[0] = 1.0f;  // Single sample impulse.

  std::vector<float> left_tail(kBlock), right_tail(kBlock);
  for (int i = 0; i < kBlock; ++i) {
    left_tail[i] = left.process_sample(input[i]);
    right_tail[i] = right.process_sample(input[i]);
  }

  // After the comb delay has wrapped a few times, the channels should differ
  // measurably. Skip the first 8192 samples (initial impulse + transient).
  double diff = 0.0;
  double energy = 0.0;
  for (int i = 8192; i < kBlock; ++i) {
    diff += static_cast<double>(left_tail[i] - right_tail[i]) * (left_tail[i] - right_tail[i]);
    energy += static_cast<double>(left_tail[i]) * left_tail[i] +
              static_cast<double>(right_tail[i]) * right_tail[i];
  }
  REQUIRE(energy > 1.0e-6);
  REQUIRE(diff > 0.0);            // Must not be bit-identical.
  REQUIRE(diff / energy > 0.05);  // Non-trivial divergence (>5% of tail energy).
}

TEST_CASE("StreamingReverb tail length scales with time_ms", "[voice_changer][reverb]") {
  // A longer decay must extend the audible tail. Compare tail energies at a
  // fixed offset late in the impulse response: short_ms's tail has decayed
  // ~60 dB by then, but long_ms's tail is still ringing.
  // Buffer must be large enough that the longest comb tap has wrapped at
  // least twice — see StreamingReverb::set_config for the 0.42/0.61 ratios.
  constexpr double sample_rate = 48000.0;
  constexpr int kBlock = 32768;
  auto tail_energy_after = [&](float time_ms, int skip_samples) {
    StreamingReverb reverb;
    reverb.prepare(sample_rate, 1);
    reverb.set_config({0.45f, time_ms, 0.5f, /*seed=*/3});
    double energy = 0.0;
    for (int i = 0; i < kBlock; ++i) {
      const float x = (i == 0) ? 1.0f : 0.0f;
      const float y = reverb.process_sample(x);
      if (i >= skip_samples) energy += static_cast<double>(y) * y;
    }
    return energy;
  };

  // 100 ms reverb decays ~60 dB by 100 ms ≈ 4800 samples; measure tail after
  // that window. 400 ms reverb still has substantial ring at the same offset.
  const double short_energy = tail_energy_after(100.0f, 8192);
  const double long_energy = tail_energy_after(400.0f, 8192);
  REQUIRE(long_energy > short_energy);
}

TEST_CASE("StreamingReverb reset clears delay-line state", "[voice_changer][reverb]") {
  // After reset(), an impulse + identical playback must yield bit-identical
  // results, demonstrating that all internal state has been cleared.
  StreamingReverb reverb;
  reverb.prepare(48000.0, 1);
  reverb.set_config({0.4f, 250.0f, 0.5f, /*seed=*/2});

  std::vector<float> baseline;
  baseline.reserve(2048);
  for (int i = 0; i < 2048; ++i) {
    baseline.push_back(reverb.process_sample(i == 0 ? 1.0f : 0.0f));
  }

  reverb.reset();
  for (int i = 0; i < 2048; ++i) {
    const float y = reverb.process_sample(i == 0 ? 1.0f : 0.0f);
    REQUIRE(y == baseline[i]);
  }
}

TEST_CASE("Voice preset metadata table is consistent across surfaces", "[voice_changer][c-api]") {
  // The table-driven preset metadata in realtime_voice_changer.cpp is the
  // single source of truth. SONARE_REALTIME_VOICE_CHANGER_PRESET_IDS is a
  // compile-time mirror used by language bindings (TS unions, Python enums) —
  // it MUST stay byte-identical to the newline-joined table ids. This test
  // catches the case where someone adds a preset to the C++ enum + table but
  // forgets to update the macro (which would silently break bindings). The
  // separator switched from ',' to '\n' to align with every other *_names API
  // in this header (see C API consistency review).
  const std::string macro_ids = SONARE_REALTIME_VOICE_CHANGER_PRESET_IDS;
  const auto names = realtime_voice_changer_preset_names();
  std::string joined;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i) joined.push_back('\n');
    joined += names[i];
  }
  REQUIRE(macro_ids == joined);

  // Every C enum value must also map to a known preset id via the C-API.
  static constexpr std::array<SonareVoiceCharacterPreset, 6> kCEnumValues = {
      SONARE_VC_PRESET_NEUTRAL_MONITOR, SONARE_VC_PRESET_BRIGHT_IDOL,
      SONARE_VC_PRESET_SOFT_WHISPER,    SONARE_VC_PRESET_DEEP_NARRATOR,
      SONARE_VC_PRESET_ROBOT_MASCOT,    SONARE_VC_PRESET_DARK_VILLAIN,
  };
  REQUIRE(kCEnumValues.size() == names.size());
  for (std::size_t i = 0; i < kCEnumValues.size(); ++i) {
    const char* id = sonare_voice_character_preset_id(kCEnumValues[i]);
    REQUIRE(id != nullptr);
    REQUIRE(std::string(id) == names[i]);
  }

  // Display names must be non-empty and round-trip via the id reverse-lookup.
  for (std::size_t i = 0; i < names.size(); ++i) {
    const auto preset = realtime_voice_changer_preset_from_id(names[i]);
    REQUIRE(std::string(realtime_voice_changer_preset_id(preset)) == names[i]);
  }
}

TEST_CASE("Voice changer POD ↔ C++ round-trip preserves every field", "[voice_changer][c-api]") {
  // The X-macro field list in sonare_c_daw.cpp drives both directions of the
  // POD ↔ C++ conversion. This test asserts symmetry end-to-end via the C
  // API: create a handle from a POD with distinctive per-field values, read
  // back the POD, and verify every value survived. A drop-out anywhere in
  // the field list would zero the corresponding field on one side and the
  // assertions below would catch it.

  // Build a POD that does NOT match any preset so each field is uniquely
  // identifiable. Values are chosen within each field's documented range so
  // the validator accepts them unchanged.
  SonareRealtimeVoiceChangerConfig pod_in{};
  pod_in.input_gain_db = 1.5f;
  pod_in.output_gain_db = -2.25f;
  pod_in.wet_mix = 0.875f;
  pod_in.retune_semitones = 3.0f;
  pod_in.retune_mix = 0.625f;
  pod_in.retune_grain_size = 1024;
  pod_in.formant_factor = 1.125f;
  pod_in.formant_amount = 0.75f;
  pod_in.formant_body = 0.125f;
  pod_in.formant_brightness = -0.25f;
  pod_in.formant_nasal = 0.0625f;
  pod_in.eq_highpass_hz = 90.0f;
  pod_in.eq_body_db = 1.25f;
  pod_in.eq_presence_db = 2.5f;
  pod_in.eq_air_db = 0.75f;
  pod_in.gate_threshold_db = -42.0f;
  pod_in.gate_attack_ms = 3.5f;
  pod_in.gate_release_ms = 175.0f;
  pod_in.gate_range_db = 15.0f;
  pod_in.compressor_threshold_db = -20.0f;
  pod_in.compressor_ratio = 3.25f;
  pod_in.compressor_attack_ms = 8.5f;
  pod_in.compressor_release_ms = 130.0f;
  pod_in.compressor_makeup_gain_db = 1.5f;
  pod_in.deesser_frequency_hz = 6800.0f;
  pod_in.deesser_threshold_db = -22.0f;
  pod_in.deesser_ratio = 5.0f;
  pod_in.deesser_range_db = 7.0f;
  pod_in.reverb_mix = 0.125f;
  pod_in.reverb_time_ms = 410.0f;
  pod_in.reverb_damping = 0.625f;
  pod_in.reverb_seed = 23;
  pod_in.limiter_ceiling_db = -2.5f;
  pod_in.limiter_release_ms = 65.0f;

  SonareRealtimeVoiceChanger* handle = nullptr;
  REQUIRE(sonare_realtime_voice_changer_create(&pod_in, 48000, 256, 1, &handle) == SONARE_OK);
  REQUIRE(handle != nullptr);

  SonareRealtimeVoiceChangerConfig pod_out{};
  REQUIRE(sonare_realtime_voice_changer_get_config(handle, &pod_out) == SONARE_OK);

  // Every field must round-trip exactly. Use bit-exact comparisons (==): the
  // normalize step is a no-op on in-range finite inputs, so any change here
  // indicates a missed field in the X-macro list.
  REQUIRE(pod_out.input_gain_db == pod_in.input_gain_db);
  REQUIRE(pod_out.output_gain_db == pod_in.output_gain_db);
  REQUIRE(pod_out.wet_mix == pod_in.wet_mix);
  REQUIRE(pod_out.retune_semitones == pod_in.retune_semitones);
  REQUIRE(pod_out.retune_mix == pod_in.retune_mix);
  REQUIRE(pod_out.retune_grain_size == pod_in.retune_grain_size);
  REQUIRE(pod_out.formant_factor == pod_in.formant_factor);
  REQUIRE(pod_out.formant_amount == pod_in.formant_amount);
  REQUIRE(pod_out.formant_body == pod_in.formant_body);
  REQUIRE(pod_out.formant_brightness == pod_in.formant_brightness);
  REQUIRE(pod_out.formant_nasal == pod_in.formant_nasal);
  REQUIRE(pod_out.eq_highpass_hz == pod_in.eq_highpass_hz);
  REQUIRE(pod_out.eq_body_db == pod_in.eq_body_db);
  REQUIRE(pod_out.eq_presence_db == pod_in.eq_presence_db);
  REQUIRE(pod_out.eq_air_db == pod_in.eq_air_db);
  REQUIRE(pod_out.gate_threshold_db == pod_in.gate_threshold_db);
  REQUIRE(pod_out.gate_attack_ms == pod_in.gate_attack_ms);
  REQUIRE(pod_out.gate_release_ms == pod_in.gate_release_ms);
  REQUIRE(pod_out.gate_range_db == pod_in.gate_range_db);
  REQUIRE(pod_out.compressor_threshold_db == pod_in.compressor_threshold_db);
  REQUIRE(pod_out.compressor_ratio == pod_in.compressor_ratio);
  REQUIRE(pod_out.compressor_attack_ms == pod_in.compressor_attack_ms);
  REQUIRE(pod_out.compressor_release_ms == pod_in.compressor_release_ms);
  REQUIRE(pod_out.compressor_makeup_gain_db == pod_in.compressor_makeup_gain_db);
  REQUIRE(pod_out.deesser_frequency_hz == pod_in.deesser_frequency_hz);
  REQUIRE(pod_out.deesser_threshold_db == pod_in.deesser_threshold_db);
  REQUIRE(pod_out.deesser_ratio == pod_in.deesser_ratio);
  REQUIRE(pod_out.deesser_range_db == pod_in.deesser_range_db);
  REQUIRE(pod_out.reverb_mix == pod_in.reverb_mix);
  REQUIRE(pod_out.reverb_time_ms == pod_in.reverb_time_ms);
  REQUIRE(pod_out.reverb_damping == pod_in.reverb_damping);
  REQUIRE(pod_out.reverb_seed == pod_in.reverb_seed);
  REQUIRE(pod_out.limiter_ceiling_db == pod_in.limiter_ceiling_db);
  REQUIRE(pod_out.limiter_release_ms == pod_in.limiter_release_ms);

  sonare_realtime_voice_changer_destroy(handle);
}

TEST_CASE("sonare_voice_changer_abi_version is non-zero and stable", "[voice_changer][c-api]") {
  // The runtime function and the compile-time constant must agree. Bindings
  // call the runtime function at attach time and compare against the
  // compile-time expectation; a mismatch indicates a host/binding ABI skew.
  const std::uint32_t runtime = sonare_voice_changer_abi_version();
  REQUIRE(runtime != 0u);
  REQUIRE(runtime == kVoiceChangerAbiVersion);

  // Repeated calls must yield the same value (no per-call state).
  REQUIRE(sonare_voice_changer_abi_version() == runtime);
}

// ===================================================================
// DSP functional: compressor actually reduces RMS above threshold.
// ===================================================================
