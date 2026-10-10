/// @file binding_synth_patch_test.cpp
/// @brief NativeSynth C ABI surface: the preset catalog
///        (sonare_synth_preset_names / sonare_synth_preset_patch), the
///        patch-driven bounce (sonare_project_bounce_with_synth_instruments)
///        and the realtime engine entry (sonare_engine_set_synth_instrument).

#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "binding_project_parity_test_helpers.h"
#include "c_api/synth_patch_common.h"
#include "support/midi_render.h"
#include "util/json.h"

namespace {

/// A project with one MIDI track routed to destination @p dest playing a held
/// note (on at 0, off at 2 ppq).
SonareProject* make_synth_project(uint32_t dest, uint8_t note = 60) {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);
  SonareMidiEventPod events[2];
  events[0].ppq = 0.0;
  events[0].data0 = 0x20900040u | (static_cast<uint32_t>(note) << 8);  // note-on, vel 64
  events[0].data1 = 0u;
  events[1].ppq = 2.0;
  events[1].data0 = 0x20800000u | (static_cast<uint32_t>(note) << 8);  // note-off
  events[1].data1 = 0u;
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, dest) == SONARE_OK);
  return project;
}

std::vector<float> bounce_synth(SonareProject* project, const SonareSynthPatch& patch,
                                uint32_t dest = 3) {
  SonareProjectBounceOptions options{};
  options.total_frames = 24000;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = dest;
  binding.patch = patch;
  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  std::vector<float> result(out, out + out_len);
  sonare_free_floats(out);
  return result;
}

float peak_of(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (float s : samples) peak = std::max(peak, std::abs(s));
  return peak;
}

#if defined(SONARE_WITH_ARRANGEMENT)
/// Middle C held for 0.5 s at 48 kHz on a NativeSynth built straight from @p cfg.
sonare::test::StereoRender render_config(const sonare::midi::synth::NativeSynthConfig& cfg) {
  constexpr int kBlock = 256;
  constexpr int kBlocks = 24000 / kBlock;
  sonare::midi::synth::NativeSynth synth(cfg);
  synth.prepare(48000.0, kBlock);
  synth.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  sonare::test::StereoRender out;
  for (int b = 0; b < kBlocks; ++b) {
    const sonare::test::StereoRender block = sonare::test::render_stereo(synth, kBlock);
    out.left.insert(out.left.end(), block.left.begin(), block.left.end());
    out.right.insert(out.right.end(), block.right.begin(), block.right.end());
  }
  return out;
}

/// @p cfg with engine @p mode selected and preset @p donor's section of it.
sonare::midi::synth::NativeSynthConfig with_section_of(sonare::midi::synth::NativeSynthConfig cfg,
                                                       sonare::midi::synth::SynthEngineMode mode,
                                                       const char* donor) {
  const sonare::midi::synth::SynthPreset* preset = sonare::midi::synth::find_synth_preset(donor);
  REQUIRE(preset != nullptr);
  REQUIRE(preset->config.patch.mode == mode);
  cfg.patch.mode = mode;
  sonare_c_detail::copy_engine_section(preset->config.patch, mode, &cfg.patch);
  return cfg;
}

/// Every engine-section field of @p converted reads back as @p donor's, through
/// the walker rather than the section copy, so a mode copied from the wrong
/// member shows up here.
void require_section_read_back(const sonare::midi::synth::NativeSynthPatch& converted,
                               const char* donor) {
  const sonare::midi::synth::SynthPreset* preset = sonare::midi::synth::find_synth_preset(donor);
  REQUIRE(preset != nullptr);
  const auto got = sonare::midi::synth::engine_param_descriptors(converted);
  const auto want = sonare::midi::synth::engine_param_descriptors(preset->config.patch);
  REQUIRE(got.size() == want.size());
  REQUIRE_FALSE(got.empty());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(got[i].key);
    REQUIRE(got[i].key == want[i].key);
    REQUIRE(got[i].value == want[i].value);
  }
}

/// The walker's engine-section path as a public key: the first segment
/// dropped, each remaining one snake_case -> lowerCamelCase.
std::string public_key_of(const std::string& path) {
  std::string key;
  bool upper = false;
  for (char ch : path.substr(path.find('.') + 1)) {
    if (ch == '_') {
      upper = true;
      continue;
    }
    key += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch))) : ch;
    upper = false;
  }
  return key;
}

sonare::util::json::Value parse_info(const char* info) {
  REQUIRE(info != nullptr);
  const sonare::util::json::Value parsed = sonare::util::json::parse(info);
  REQUIRE(parsed.is_array());
  return parsed;
}

/// Shape every descriptor shares: a known type, a unit, and a default inside
/// the range where one is given.
void require_descriptor_shape(const sonare::util::json::Value& d) {
  REQUIRE(d["name"].is_string());
  INFO(d["name"].as_string());
  const std::string& type = d["type"].as_string();
  REQUIRE((type == "number" || type == "boolean"));
  REQUIRE(d["unit"].is_string());
  REQUIRE_FALSE(d["unit"].as_string().empty());
  REQUIRE(d.contains("min") == d.contains("max"));
  if (type == "boolean") {
    REQUIRE(d["default"].is_bool());
    REQUIRE_FALSE(d.contains("min"));
  } else {
    REQUIRE(d["default"].is_number());
  }
  if (d.contains("integer")) REQUIRE(d["integer"].as_bool());
  if (d.contains("min")) {
    REQUIRE(d["min"].as_number() <= d["default"].as_number());
    REQUIRE(d["default"].as_number() <= d["max"].as_number());
  }
}
#endif

}  // namespace

TEST_CASE("synth preset catalog lists every §E entry", "[project][synth_patch]") {
  const char* names = sonare_synth_preset_names();
  REQUIRE(names != nullptr);
#if defined(SONARE_WITH_ARRANGEMENT)
  const std::string joined = std::string("\n") + names + "\n";
  for (const char* expected : {"sine", "saw-lead", "square-lead", "sub-bass", "warm-pad", "e-piano",
                               "bell", "brass", "pluck", "electric-guitar", "harp", "marimba",
                               "glass", "organ", "drum-kit", "acoustic-piano"}) {
    INFO(expected);
    REQUIRE(joined.find(std::string("\n") + expected + "\n") != std::string::npos);
  }
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)

TEST_CASE("synth preset patches round-trip through the versioned struct",
          "[project][synth_patch]") {
  SonareSynthPatch patch{};
  // warm-pad: a 7-osc subtractive supersaw.
  REQUIRE(sonare_synth_preset_patch("warm-pad", &patch) == SONARE_OK);
  REQUIRE(patch.struct_version == SONARE_SYNTH_PATCH_STRUCT_VERSION);
  REQUIRE(std::string(patch.preset) == "warm-pad");
  REQUIRE(patch.engine_mode == SONARE_SYNTH_ENGINE_SUBTRACTIVE);
  REQUIRE(patch.waveform == SONARE_SYNTH_OSC_SAW);
  REQUIRE(patch.unison == 7);
  REQUIRE(patch.stereo_spread > 0.0f);
  // The bare single-oscillator waveforms the CLI documents each resolve to a
  // one-voice subtractive patch on their named waveform (the richer -lead
  // variants stack unison on top).
  for (const auto& [name, osc] :
       {std::pair<const char*, SonareSynthOscWaveform>{"saw", SONARE_SYNTH_OSC_SAW},
        {"square", SONARE_SYNTH_OSC_SQUARE},
        {"triangle", SONARE_SYNTH_OSC_TRIANGLE}}) {
    INFO(name);
    REQUIRE(sonare_synth_preset_patch(name, &patch) == SONARE_OK);
    REQUIRE(patch.engine_mode == SONARE_SYNTH_ENGINE_SUBTRACTIVE);
    REQUIRE(patch.waveform == osc);
    REQUIRE(patch.unison == 1);
  }
  // e-piano selects the FM engine; electric-guitar the KS engine.
  REQUIRE(sonare_synth_preset_patch("e-piano", &patch) == SONARE_OK);
  REQUIRE(patch.engine_mode == SONARE_SYNTH_ENGINE_FM);
  REQUIRE(sonare_synth_preset_patch("electric-guitar", &patch) == SONARE_OK);
  REQUIRE(patch.engine_mode == SONARE_SYNTH_ENGINE_KARPLUS_STRONG);
  REQUIRE(sonare_synth_preset_patch("acoustic-piano", &patch) == SONARE_OK);
  REQUIRE(patch.engine_mode == SONARE_SYNTH_ENGINE_PIANO);
  // Unknown names and NULL args are rejected.
  REQUIRE(sonare_synth_preset_patch("minimoog", &patch) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_synth_preset_patch(nullptr, &patch) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_synth_preset_patch("sine", nullptr) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("synth patch enum counts match the public C ordinals", "[project][synth_patch]") {
  REQUIRE(SONARE_SYNTH_ENGINE_DEFAULT == 0);
  REQUIRE(SONARE_SYNTH_ENGINE_SAMPLE + 1 == SONARE_SYNTH_ENGINE_MODE_COUNT);
  REQUIRE(SONARE_SYNTH_OSC_DEFAULT == 0);
  REQUIRE(SONARE_SYNTH_OSC_NOISE + 1 == SONARE_SYNTH_OSC_WAVEFORM_COUNT);
  REQUIRE(SONARE_SYNTH_FILTER_DEFAULT == 0);
  REQUIRE(SONARE_SYNTH_FILTER_SALLEN_KEY + 1 == SONARE_SYNTH_FILTER_MODEL_COUNT);
  REQUIRE(SONARE_SYNTH_FILTER_OUT_DEFAULT == 0);
  REQUIRE(SONARE_SYNTH_FILTER_OUT_HIGHPASS + 1 == SONARE_SYNTH_FILTER_OUTPUT_COUNT);
  REQUIRE(SONARE_SYNTH_BODY_DEFAULT == 0);
  REQUIRE(SONARE_SYNTH_BODY_VOCAL + 1 == SONARE_SYNTH_BODY_TYPE_COUNT);
  REQUIRE(SONARE_SYNTH_MOD_SOURCE_COUNT == 13);
  REQUIRE(SONARE_SYNTH_MOD_DESTINATION_COUNT == 13);
}

TEST_CASE("every engine mode but sample sounds from engine_mode alone", "[project][synth_patch]") {
  // A bare engine_mode takes its engine section from the engine's base preset,
  // so the one mode left with nothing to voice is sample, whose bank is bound
  // beside the patch. Pinned from both sides: sample stays silent, and every
  // other mode clears -60 dBFS on a held middle C.
  SonareProject* project = make_synth_project(3);

  std::set<int> inaudible;
  for (int mode = 0; mode < SONARE_SYNTH_ENGINE_MODE_COUNT; ++mode) {
    SonareSynthPatch patch{};
    patch.engine_mode = mode;
    CAPTURE(mode);
    const float peak = peak_of(bounce_synth(project, patch));
    if (!(peak > 1.0e-3f)) inaudible.insert(mode);
    if (mode == SONARE_SYNTH_ENGINE_SAMPLE) REQUIRE(peak == 0.0f);
  }
  REQUIRE(inaudible == std::set<int>{SONARE_SYNTH_ENGINE_SAMPLE});

  sonare_project_destroy(project);
}

TEST_CASE("every named synth patch renders as its catalog preset", "[project][synth_patch]") {
  // A patch naming a preset of the engine it selects must reach the voice as
  // that preset, whatever else the conversion seeds or overlays.
  for (size_t i = 0; i < sonare::midi::synth::synth_preset_count(); ++i) {
    const sonare::midi::synth::SynthPreset* preset = sonare::midi::synth::synth_preset_at(i);
    REQUIRE(preset != nullptr);
    INFO(preset->name);
    SonareSynthPatch patch{};
    patch.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
    std::strncpy(patch.preset, preset->name, SONARE_SYNTH_PRESET_NAME_MAX - 1);
    sonare::midi::synth::NativeSynthConfig cfg;
    const char* error = nullptr;
    REQUIRE(sonare_c_detail::synth_config_from_patch_c(patch, &cfg, &error));
    const sonare::test::StereoRender direct = render_config(preset->config);
    const sonare::test::StereoRender converted = render_config(cfg);
    REQUIRE(converted.left == direct.left);
    REQUIRE(converted.right == direct.right);
  }
}

TEST_CASE("a synth patch takes a missing engine section from the base preset",
          "[project][synth_patch]") {
  using sonare::midi::synth::SynthEngineMode;
  // Bare engine_mode: the init patch's wrapper with the base preset's section.
  for (int mode = SONARE_SYNTH_ENGINE_DEFAULT + 1; mode < SONARE_SYNTH_ENGINE_MODE_COUNT; ++mode) {
    const auto engine = static_cast<SynthEngineMode>(mode - 1);
    const char* base = sonare::midi::synth::base_preset_name(engine);
    if (base == nullptr) continue;
    CAPTURE(mode, base);
    SonareSynthPatch patch{};
    patch.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
    patch.engine_mode = mode;
    sonare::midi::synth::NativeSynthConfig cfg;
    const char* error = nullptr;
    REQUIRE(sonare_c_detail::synth_config_from_patch_c(patch, &cfg, &error));
    require_section_read_back(cfg.patch, base);
    const sonare::test::StereoRender expected =
        render_config(with_section_of(sonare::midi::synth::NativeSynthConfig{}, engine, base));
    const sonare::test::StereoRender converted = render_config(cfg);
    REQUIRE(converted.left == expected.left);
    REQUIRE(converted.right == expected.right);
  }

  // A preset of another engine keeps its wrapper and takes the selected
  // engine's base section: the violin's wrapper over the e-piano's operators.
  REQUIRE(std::string(sonare::midi::synth::base_preset_name(SynthEngineMode::kFm)) == "e-piano");
  SonareSynthPatch violin_fm{};
  violin_fm.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
  std::strcpy(violin_fm.preset, "violin");
  violin_fm.engine_mode = SONARE_SYNTH_ENGINE_FM;
  sonare::midi::synth::NativeSynthConfig cfg;
  const char* error = nullptr;
  REQUIRE(sonare_c_detail::synth_config_from_patch_c(violin_fm, &cfg, &error));
  require_section_read_back(cfg.patch, "e-piano");
  const sonare::midi::synth::SynthPreset* violin = sonare::midi::synth::find_synth_preset("violin");
  REQUIRE(violin != nullptr);
  const sonare::test::StereoRender expected =
      render_config(with_section_of(violin->config, SynthEngineMode::kFm, "e-piano"));
  const sonare::test::StereoRender converted = render_config(cfg);
  REQUIRE(converted.left == expected.left);
  REQUIRE(converted.right == expected.right);
  REQUIRE(peak_of(converted.left) > 1.0e-3f);
}

TEST_CASE("synth patch engine params are refused through the C ABI with the key named",
          "[project][synth_patch]") {
  SonareProject* project = make_synth_project(3);
  SonareProjectBounceOptions options{};
  options.total_frames = 1024;
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = 3;
  float* out = nullptr;
  size_t out_len = 0;

  // bowForce: a bounded number of the bowed-string section.
  const sonare::util::json::Value bowed =
      parse_info(sonare_synth_engine_param_info(SONARE_SYNTH_ENGINE_BOWED_STRING));
  const sonare::util::json::Value* bow_force = nullptr;
  for (const auto& d : bowed.as_array()) {
    if (d["name"].as_string() == "bowForce") bow_force = &d;
  }
  REQUIRE(bow_force != nullptr);
  REQUIRE(bow_force->contains("max"));
  const double max = (*bow_force)["max"].as_number();
  const double mid = 0.5 * ((*bow_force)["min"].as_number() + max);

  // An integer field of whichever engine has one first.
  int integer_mode = -1;
  std::string integer_key;
  for (int mode = 1; mode < SONARE_SYNTH_ENGINE_MODE_COUNT && integer_mode < 0; ++mode) {
    const sonare::util::json::Value info = parse_info(sonare_synth_engine_param_info(mode));
    for (const auto& d : info.as_array()) {
      if (d.contains("integer")) {
        integer_mode = mode;
        integer_key = d["name"].as_string();
        break;
      }
    }
  }
  REQUIRE(integer_mode > 0);

  const auto bounce_with = [&](int mode, const char* key, double value) {
    SonareSynthEngineParam param{key, value};
    SonareSynthPatch patch{};
    patch.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
    patch.engine_mode = mode;
    patch.engine_params = &param;
    patch.engine_param_count = 1;
    binding.patch = patch;
    out = nullptr;
    const SonareError status = sonare_project_bounce_with_synth_instruments(
        project, &options, &binding, 1, &out, &out_len);
    sonare_free_floats(out);
    return status;
  };
  const auto refused = [&](int mode, const char* key, double value, const char* fragment) {
    CAPTURE(key, value);
    REQUIRE(bounce_with(mode, key, value) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
    const std::string message = sonare_last_error_message();
    CAPTURE(message);
    REQUIRE(message.find(std::string("'") + key + "'") != std::string::npos);
    REQUIRE(message.find(fragment) != std::string::npos);
  };
  const int bowed_mode = SONARE_SYNTH_ENGINE_BOWED_STRING;
  refused(bowed_mode, "noSuchField", 1.0, "engine param");
  refused(bowed_mode, "ops1.level", 0.5, "engine param");   // the FM section's
  refused(bowed_mode, "cutoffHz", 1000.0, "engine param");  // a wrapper field
  refused(bowed_mode, "bowForce", std::numeric_limits<double>::quiet_NaN(), "engine param");
  refused(bowed_mode, "bowForce", max + 1.0, "[");
  refused(integer_mode, integer_key.c_str(), 1.5, "engine param");
  // Not vacuous: an in-range value of the same field is accepted.
  REQUIRE(bounce_with(bowed_mode, "bowForce", mid) == SONARE_OK);

  // A count with no array is refused rather than read.
  SonareSynthPatch dangling{};
  dangling.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
  dangling.engine_param_count = 1;
  binding.patch = dangling;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_ERROR_INVALID_PARAMETER);
  // A version-7 caller's struct ends before the params, so whatever sits there is not read.
  SonareSynthPatch older = dangling;
  older.struct_version = 7;
  binding.patch = older;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_OK);
  sonare_free_floats(out);

  sonare_project_destroy(project);
}

TEST_CASE("an engine param reaches the render", "[project][synth_patch]") {
  SonareProject* project = make_synth_project(3);
  SonareSynthPatch base{};
  base.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
  std::strcpy(base.preset, "violin");
  const std::vector<float> reference = bounce_synth(project, base);
  REQUIRE(peak_of(reference) > 0.0f);
  // The middle of bowForce's range, which the violin does not sit on.
  double mid = 0.0;
  const sonare::util::json::Value bowed =
      parse_info(sonare_synth_engine_param_info(SONARE_SYNTH_ENGINE_BOWED_STRING));
  for (const auto& d : bowed.as_array()) {
    if (d["name"].as_string() == "bowForce") {
      mid = 0.5 * (d["min"].as_number() + d["max"].as_number());
      REQUIRE(mid != d["default"].as_number());
    }
  }
  const SonareSynthEngineParam param{"bowForce", mid};
  SonareSynthPatch lighter = base;
  lighter.engine_params = &param;
  lighter.engine_param_count = 1;
  REQUIRE(bounce_synth(project, lighter) != reference);
  sonare_project_destroy(project);
}

TEST_CASE("synth engine param info lists exactly the walker's engine section",
          "[project][synth_patch]") {
  for (int mode = 0; mode < SONARE_SYNTH_ENGINE_MODE_COUNT; ++mode) {
    CAPTURE(mode);
    const sonare::util::json::Value info = parse_info(sonare_synth_engine_param_info(mode));
    std::set<std::string> names;
    for (const auto& d : info.as_array()) {
      require_descriptor_shape(d);
      names.insert(d["name"].as_string());
    }
    REQUIRE(names.size() == info.size());
    const char* base = mode == SONARE_SYNTH_ENGINE_DEFAULT
                           ? nullptr
                           : sonare::midi::synth::base_preset_name(
                                 static_cast<sonare::midi::synth::SynthEngineMode>(mode - 1));
    if (base == nullptr) {
      REQUIRE(info.size() == 0);
      continue;
    }
    std::set<std::string> walked;
    for (const auto& site : sonare::midi::synth::patch_tuning_detail::engine_field_sites(
             sonare::midi::synth::find_synth_preset(base)->config.patch)) {
      walked.insert(public_key_of(site.path));
    }
    REQUIRE(names == walked);
  }
  // Exactly these three have no section of their own.
  for (int mode :
       {SONARE_SYNTH_ENGINE_DEFAULT, SONARE_SYNTH_ENGINE_SUBTRACTIVE, SONARE_SYNTH_ENGINE_SAMPLE}) {
    REQUIRE(std::string(sonare_synth_engine_param_info(mode)) == "[]");
  }
  for (int mode : {-1, SONARE_SYNTH_ENGINE_MODE_COUNT}) {
    CAPTURE(mode);
    REQUIRE(sonare_synth_engine_param_info(mode) == nullptr);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("synth patch param info describes every numeric patch field", "[project][synth_patch]") {
  // The numeric SonareSynthPatch fields under their binding names; enums, the
  // preset name, the mod matrix, the presence mask and the engine params are not
  // numeric parameters.
  const std::set<std::string> fields = {"unison",          "detuneCents",       "driftCents",
                                        "drive",           "cutoffHz",          "resonanceQ",
                                        "keyTrack",        "envToCutoffCents",  "velToCutoffCents",
                                        "ampAttackMs",     "ampDecayMs",        "ampSustain",
                                        "ampReleaseMs",    "filterAttackMs",    "filterDecayMs",
                                        "filterSustain",   "filterReleaseMs",   "lfoRateHz",
                                        "lfoToPitchCents", "lfo2RateHz",        "glideMs",
                                        "bodyMix",         "stereoSpread",      "gain",
                                        "polyphony",       "busDrive",          "sampleSet",
                                        "sampleLevel",     "sampleStartOffset", "hpCutoffHz",
                                        "sampleHoldHz",    "bitDepth",          "pitchOffsetCents"};
  const sonare::util::json::Value info = parse_info(sonare_synth_patch_param_info());
  std::set<std::string> names;
  const auto ends_with = [](const std::string& name, const std::string& suffix) {
    return name.size() > suffix.size() &&
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  for (const auto& d : info.as_array()) {
    require_descriptor_shape(d);
    const std::string& name = d["name"].as_string();
    const std::string& unit = d["unit"].as_string();
    CAPTURE(name, unit);
    names.insert(name);
    if (ends_with(name, "Hz")) REQUIRE(unit == "Hz");
    if (ends_with(name, "Ms")) REQUIRE(unit == "ms");
    if (ends_with(name, "Cents")) REQUIRE(unit == "cents");
  }
  REQUIRE(names.size() == info.size());
  REQUIRE(names == fields);
  // The automation table's names are this table's names.
  for (size_t i = 0; i < sonare::midi::synth::native_synth_param_count(); ++i) {
    REQUIRE(fields.count(sonare::midi::synth::native_synth_param_name_at(i)) == 1);
  }
}

TEST_CASE("synth patch conversion rejects out-of-range enum fields", "[project][synth_patch]") {
  SonareSynthPatch patch{};
  sonare::midi::synth::NativeSynthConfig cfg;
  const char* error = nullptr;

  auto rejected = [&](auto mutate) {
    SonareSynthPatch invalid{};
    mutate(invalid);
    error = nullptr;
    return !sonare_c_detail::synth_config_from_patch_c(invalid, &cfg, &error) && error != nullptr;
  };

  REQUIRE(rejected([](SonareSynthPatch& p) { p.engine_mode = SONARE_SYNTH_ENGINE_MODE_COUNT; }));
  REQUIRE(rejected([](SonareSynthPatch& p) { p.waveform = SONARE_SYNTH_OSC_WAVEFORM_COUNT; }));
  REQUIRE(rejected([](SonareSynthPatch& p) { p.filter_model = SONARE_SYNTH_FILTER_MODEL_COUNT; }));
  REQUIRE(
      rejected([](SonareSynthPatch& p) { p.filter_output = SONARE_SYNTH_FILTER_OUTPUT_COUNT; }));
  REQUIRE(rejected([](SonareSynthPatch& p) { p.body = SONARE_SYNTH_BODY_TYPE_COUNT; }));
  REQUIRE(rejected([](SonareSynthPatch& p) {
    p.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
    p.retrigger = SONARE_SYNTH_RETRIGGER_COUNT;
  }));
  REQUIRE(rejected([](SonareSynthPatch& p) {
    p.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
    p.retrigger = -1;
  }));

  REQUIRE(sonare_c_detail::synth_config_from_patch_c(patch, &cfg, &error));
}

TEST_CASE("bounce_with_synth_instruments renders preset patches deterministically",
          "[project][synth_patch]") {
  SonareProject* project = make_synth_project(3);

  // Every catalog preset must produce audio through the bounce.
  const char* names = sonare_synth_preset_names();
  std::string catalog(names);
  size_t pos = 0;
  while (pos < catalog.size()) {
    size_t next = catalog.find('\n', pos);
    if (next == std::string::npos) next = catalog.size();
    const std::string name = catalog.substr(pos, next - pos);
    pos = next + 1;
    INFO(name);
    REQUIRE(name.size() < SONARE_SYNTH_PRESET_NAME_MAX);
    SonareSynthPatch patch{};
    REQUIRE(sonare_synth_preset_patch(name.c_str(), &patch) == SONARE_OK);
    REQUIRE(std::strlen(patch.preset) < SONARE_SYNTH_PRESET_NAME_MAX);
    REQUIRE(peak_of(bounce_synth(project, patch)) > 0.0f);
  }

  // Determinism: bit-identical renders for a fixed patch.
  SonareSynthPatch patch{};
  REQUIRE(sonare_synth_preset_patch("saw-lead", &patch) == SONARE_OK);
  REQUIRE(bounce_synth(project, patch) == bounce_synth(project, patch));

  sonare_project_destroy(project);
}

TEST_CASE("synth patch presence bits express an explicit zero", "[project][synth_patch]") {
  SonareProject* project = make_synth_project(3);

  // warm-pad carries a non-zero stereo spread and bus drive, so turning either
  // off is a real edit that a bare zero cannot express: without a presence bit
  // the zero reads as the "keep base" sentinel and the render is unchanged.
  SonareSynthPatch base{};
  base.struct_version = 2;
  std::strncpy(base.preset, "warm-pad", SONARE_SYNTH_PRESET_NAME_MAX - 1);
  const std::vector<float> reference = bounce_synth(project, base);
  REQUIRE(peak_of(reference) > 0.0f);

  SonareSynthPatch bare_zero = base;
  bare_zero.stereo_spread = 0.0f;
  bare_zero.bus_drive = 0.0f;
  REQUIRE(bounce_synth(project, bare_zero) == reference);

  SonareSynthPatch explicit_zero = bare_zero;
  explicit_zero.present_fields = SONARE_SYNTH_FIELD_STEREO_SPREAD | SONARE_SYNTH_FIELD_BUS_DRIVE;
  REQUIRE(bounce_synth(project, explicit_zero) != reference);

  // A version-1 caller predates the mask, so its bits stay inert.
  SonareSynthPatch version_one = explicit_zero;
  version_one.struct_version = 1;
  REQUIRE(bounce_synth(project, version_one) == reference);

  // An empty mod-routing table clears the base matrix only when marked.
  SonareSynthPatch wobble = base;
  wobble.num_mod_routings = 1;
  wobble.mod_routings[0] = {3 /*lfo1*/, 1 /*pitchCents*/, 80.0f};
  wobble.lfo_rate_hz = 6.0f;
  const std::vector<float> with_matrix = bounce_synth(project, wobble);
  REQUIRE(with_matrix != reference);

  SonareSynthPatch cleared = wobble;
  cleared.num_mod_routings = 0;
  cleared.present_fields |= SONARE_SYNTH_FIELD_MOD_ROUTINGS;
  REQUIRE(bounce_synth(project, cleared) != with_matrix);

  sonare_project_destroy(project);
}

TEST_CASE("synth patch presence-bit gain zero renders silence", "[project][synth_patch][gain]") {
  SonareProject* project = make_synth_project(3);

  SonareSynthPatch base{};
  base.struct_version = 2;
  std::strncpy(base.preset, "warm-pad", SONARE_SYNTH_PRESET_NAME_MAX - 1);
  REQUIRE(peak_of(bounce_synth(project, base)) > 0.0f);

  SonareSynthPatch silent = base;
  silent.gain = 0.0f;
  silent.present_fields |= SONARE_SYNTH_FIELD_GAIN;
  REQUIRE(peak_of(bounce_synth(project, silent)) == 0.0f);

  SonareSynthPatch quiet = base;
  quiet.gain = 0.01f;
  quiet.present_fields |= SONARE_SYNTH_FIELD_GAIN;
  REQUIRE(peak_of(bounce_synth(project, quiet)) > 0.0f);

  sonare_project_destroy(project);
}

TEST_CASE("a mod routing naming none on either end is refused",
          "[project][synth_patch][mod_matrix]") {
  SonareProject* project = make_synth_project(3);
  SonareProjectBounceOptions options{};
  options.total_frames = 1024;
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = 3;
  float* out = nullptr;
  size_t out_len = 0;

  SonareSynthPatch none_source{};
  none_source.num_mod_routings = 1;
  none_source.mod_routings[0] = {0 /*none*/, 1 /*pitchCents*/, 80.0f};
  binding.patch = none_source;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_ERROR_INVALID_PARAMETER);

  SonareSynthPatch none_destination{};
  none_destination.num_mod_routings = 1;
  none_destination.mod_routings[0] = {3 /*lfo1*/, 0 /*none*/, 80.0f};
  binding.patch = none_destination;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_ERROR_INVALID_PARAMETER);

  // Not vacuous: a routing naming a real source and destination on both ends
  // still passes.
  SonareSynthPatch valid{};
  valid.num_mod_routings = 1;
  valid.mod_routings[0] = {3 /*lfo1*/, 1 /*pitchCents*/, 80.0f};
  binding.patch = valid;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_OK);
  sonare_free_floats(out);

  sonare_project_destroy(project);
}

TEST_CASE("synth patch field overrides shape the preset", "[project][synth_patch]") {
  SonareProject* project = make_synth_project(3);

  // A zero-init patch (no preset) is the default subtractive patch.
  SonareSynthPatch init{};
  const std::vector<float> plain = bounce_synth(project, init);
  REQUIRE(peak_of(plain) > 0.0f);

  // Overriding the filter audibly changes the render.
  SonareSynthPatch dark = init;
  dark.cutoff_hz = 300.0f;
  dark.resonance_q = 4.0f;
  const std::vector<float> filtered = bounce_synth(project, dark);
  REQUIRE(filtered != plain);

  // The mod matrix table is applied (vibrato via LFO1 -> pitch).
  SonareSynthPatch wobble = init;
  wobble.num_mod_routings = 1;
  wobble.mod_routings[0] = {3 /*lfo1*/, 1 /*pitchCents*/, 80.0f};
  wobble.lfo_rate_hz = 6.0f;
  REQUIRE(bounce_synth(project, wobble) != plain);

  // Invalid patches are rejected.
  SonareSynthPatch bad_version{};
  bad_version.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION + 1;
  SonareProjectBounceOptions options{};
  options.total_frames = 1024;
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = 3;
  binding.patch = bad_version;
  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_ERROR_INVALID_PARAMETER);
  SonareSynthPatch bad_name{};
  std::strcpy(bad_name.preset, "no-such-preset");
  binding.patch = bad_name;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("synth patch numeric zero fields keep the base preset", "[project][synth_patch]") {
  SonareSynthPatch preset_only{};
  std::strcpy(preset_only.preset, "warm-pad");

  sonare::midi::synth::NativeSynthConfig base;
  const char* error = nullptr;
  REQUIRE(sonare_c_detail::synth_config_from_patch_c(preset_only, &base, &error));
  REQUIRE(error == nullptr);
  REQUIRE(base.patch.amp_env.sustain > 0.0f);
  REQUIRE(base.patch.filter_env.sustain > 0.0f);
  REQUIRE(base.gain > 0.0f);

  SonareSynthPatch explicit_zero = preset_only;
  explicit_zero.amp_sustain = 0.0f;
  explicit_zero.filter_sustain = 0.0f;
  explicit_zero.gain = 0.0f;

  sonare::midi::synth::NativeSynthConfig kept;
  REQUIRE(sonare_c_detail::synth_config_from_patch_c(explicit_zero, &kept, &error));
  REQUIRE(error == nullptr);
  REQUIRE(kept.patch.amp_env.sustain == base.patch.amp_env.sustain);
  REQUIRE(kept.patch.filter_env.sustain == base.patch.filter_env.sustain);
  REQUIRE(kept.gain == base.gain);

  SonareSynthPatch non_zero = preset_only;
  non_zero.amp_sustain = 0.25f;
  non_zero.filter_sustain = 0.125f;
  non_zero.gain = 0.5f;

  sonare::midi::synth::NativeSynthConfig overridden;
  REQUIRE(sonare_c_detail::synth_config_from_patch_c(non_zero, &overridden, &error));
  REQUIRE(error == nullptr);
  REQUIRE(overridden.patch.amp_env.sustain == 0.25f);
  REQUIRE(overridden.patch.filter_env.sustain == 0.125f);
  REQUIRE(overridden.gain == 0.5f);
}

TEST_CASE("the patch pitch offset is read only by a caller that declares it",
          "[project][synth_patch]") {
  SonareSynthPatch asked{};
  asked.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
  std::strcpy(asked.preset, "clarinet");
  asked.pitch_offset_cents = 700.0f;

  sonare::midi::synth::NativeSynthConfig read;
  const char* error = nullptr;
  REQUIRE(sonare_c_detail::synth_config_from_patch_c(asked, &read, &error));
  REQUIRE(error == nullptr);
  REQUIRE(read.patch.pitch_offset_cents == 700.0f);

  // The version gate is what makes a tail append safe: a caller compiled against
  // the previous layout has whatever its own struct ended with sitting where
  // this field now is, so the field must stay unread at its version.
  SonareSynthPatch older = asked;
  older.struct_version = 5;  // the layout before the pitch offset
  sonare::midi::synth::NativeSynthConfig ignored;
  REQUIRE(sonare_c_detail::synth_config_from_patch_c(older, &ignored, &error));
  REQUIRE(error == nullptr);
  REQUIRE(ignored.patch.pitch_offset_cents == 0.0f);
}

TEST_CASE("the patch retrigger mode is read only by a caller that declares it",
          "[project][synth_patch][retrigger]") {
  using sonare::midi::synth::SynthRetrigger;
  SonareSynthPatch asked{};
  asked.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
  std::strcpy(asked.preset, "saw-lead");
  asked.retrigger = SONARE_SYNTH_RETRIGGER_NOTE;
  sonare::midi::synth::NativeSynthConfig read;
  const char* error = nullptr;
  REQUIRE(sonare_c_detail::synth_config_from_patch_c(asked, &read, &error));
  REQUIRE(read.patch.retrigger == SynthRetrigger::kNote);

  // Zero keeps the base, which for every catalog preset is free running.
  SonareSynthPatch base = asked;
  base.retrigger = SONARE_SYNTH_RETRIGGER_BASE;
  REQUIRE(sonare_c_detail::synth_config_from_patch_c(base, &read, &error));
  REQUIRE(read.patch.retrigger == SynthRetrigger::kFree);

  // A version-6 caller's struct ends before the field, so whatever sits there
  // (here an out-of-range value that would otherwise be refused) is not read.
  SonareSynthPatch older = asked;
  older.struct_version = 6;
  older.retrigger = 99;
  REQUIRE(sonare_c_detail::synth_config_from_patch_c(older, &read, &error));
  REQUIRE(error == nullptr);
  REQUIRE(read.patch.retrigger == SynthRetrigger::kFree);

  SonareSynthPatch newer = asked;
  newer.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION + 1;
  REQUIRE_FALSE(sonare_c_detail::synth_config_from_patch_c(newer, &read, &error));
  REQUIRE(error != nullptr);

  // The read direction reports the resolved mode rather than the keep-base zero.
  SonareSynthPatch preset{};
  REQUIRE(sonare_synth_preset_patch("saw-lead", &preset) == SONARE_OK);
  REQUIRE(preset.retrigger == SONARE_SYNTH_RETRIGGER_FREE);
}

TEST_CASE("the patch pitch offset reaches both of the engine pitch sources",
          "[project][synth_patch]") {
  SonareProject* project = make_synth_project(3);

  // The two engine classes take their pitch from different places — the
  // subtractive oscillator from its base frequency, the physical models from
  // the per-sample pitch factor — and the offset is applied at each. One preset
  // per side, because a field wired into only one of them moves only one.
  for (const char* preset : {"clarinet", "saw-lead"}) {
    SonareSynthPatch base{};
    base.struct_version = SONARE_SYNTH_PATCH_STRUCT_VERSION;
    std::strcpy(base.preset, preset);
    const std::vector<float> reference = bounce_synth(project, base);
    REQUIRE(peak_of(reference) > 0.0f);

    SonareSynthPatch transposed = base;
    transposed.pitch_offset_cents = 700.0f;
    REQUIRE(bounce_synth(project, transposed) != reference);
  }

  sonare_project_destroy(project);
}

TEST_CASE("synth patch mod routing ordinals are clamped at the C ABI boundary",
          "[project][synth_patch]") {
  SonareSynthPatch patch{};
  patch.num_mod_routings = 4;
  patch.mod_routings[0] = {-1, 1 /*pitchCents*/, 100.0f};
  patch.mod_routings[1] = {99, 2 /*cutoffCents*/, 100.0f};
  patch.mod_routings[2] = {3 /*lfo1*/, -1, 100.0f};
  patch.mod_routings[3] = {4 /*lfo2*/, 99, 100.0f};

  sonare::midi::synth::NativeSynthConfig cfg;
  const char* error = nullptr;
  REQUIRE(sonare_c_detail::synth_config_from_patch_c(patch, &cfg, &error));
  REQUIRE(error == nullptr);

  const auto& routes = cfg.patch.mod_matrix.routes;
  REQUIRE(routes[0].source == sonare::midi::synth::ModSource::kNone);
  REQUIRE(routes[0].destination == sonare::midi::synth::ModDestination::kPitchCents);
  REQUIRE(routes[1].source == sonare::midi::synth::ModSource::kNone);
  REQUIRE(routes[1].destination == sonare::midi::synth::ModDestination::kCutoffCents);
  REQUIRE(routes[2].source == sonare::midi::synth::ModSource::kLfo1);
  REQUIRE(routes[2].destination == sonare::midi::synth::ModDestination::kNone);
  REQUIRE(routes[3].source == sonare::midi::synth::ModSource::kLfo2);
  REQUIRE(routes[3].destination == sonare::midi::synth::ModDestination::kNone);
}

TEST_CASE("the drum-kit preset plays the GM drum map on any key", "[project][synth_patch]") {
  // Note 38 = acoustic snare in the GM map.
  SonareProject* project = make_synth_project(3, 38);
  SonareSynthPatch kit{};
  REQUIRE(sonare_synth_preset_patch("drum-kit", &kit) == SONARE_OK);
  REQUIRE(kit.engine_mode == SONARE_SYNTH_ENGINE_PERCUSSION);
  const std::vector<float> snare = bounce_synth(project, kit);
  REQUIRE(peak_of(snare) > 0.0f);
  REQUIRE(bounce_synth(project, kit) == snare);
  sonare_project_destroy(project);
}

TEST_CASE("the GS sets that render as Standard are the ones that say so",
          "[project][synth_patch]") {
  // The list is built from the query rather than compared against one, so a set
  // that gains a voicing moves this expectation instead of silently agreeing
  // with a hardcoded exclusion list — which is the thing the query replaces.
  std::string named;
  std::string same_as_standard;
  for (int program = 0; program < 128; ++program) {
    const char* name = sonare_synth_gs_drum_kit_name(program);
    const int apart = sonare_synth_gs_drum_kit_is_voiced_apart(program);
    CAPTURE(program);
    if (name == nullptr) {
      REQUIRE(apart == -1);  // no set here, so there is nothing to compare
      continue;
    }
    REQUIRE((apart == 0 || apart == 1));
    if (!named.empty()) named += ',';
    named += std::to_string(program);
    if (apart == 0) {
      if (!same_as_standard.empty()) same_as_standard += ',';
      same_as_standard += std::to_string(program);
      same_as_standard += ':';
      same_as_standard += name;
    }
  }
  REQUIRE(named == "0,1,2,8,9,10,11,16,24,25,26,27,28,29,30,32,40,48,49,50,52,53,56,57,58,127");
  // Standard is the comparison, and the other four are the sets GS fills with
  // one-shots — a bank of extra pieces over an unchanged kit.
  REQUIRE(same_as_standard == "0:Standard,53:Cymbal & Claps,56:SFX,57:Rhythm FX,58:Rhythm FX 2");
}

TEST_CASE("a GS variation bank reports whether it is voiced or falls back",
          "[project][synth_patch]") {
  // Bank 0 is the capital tone itself, so it is never apart from itself.
  REQUIRE(sonare_synth_gs_variation_is_voiced_apart(0, 0) == 0);
  // Piano 1w: the wide variation carries its own patch.
  REQUIRE(sonare_synth_gs_variation_is_voiced_apart(8, 0) == 1);
  // The same variation reached through the GM2 bank-select LSB.
  REQUIRE(sonare_synth_gs_variation_is_voiced_apart(1, 0) == 1);
  // Program 16 MSB 24 renders as the capital: GS resolving a variation this
  // build does not voice, which is the specified behaviour and not a gap.
  REQUIRE(sonare_synth_gs_variation_is_voiced_apart(24, 16) == 0);
  // Both arguments are seven-bit MIDI values, so both ends of both are refused.
  // The bank's upper end is the one that matters: bounding it by what a uint16_t
  // holds rather than by what a Bank Select means let 128 and up reach a
  // resolution with no variation to find, which answers 0 -- the single wrong
  // answer a caller cannot tell from a real capital-tone result.
  REQUIRE(sonare_synth_gs_variation_is_voiced_apart(-1, 0) == -1);
  REQUIRE(sonare_synth_gs_variation_is_voiced_apart(128, 0) == -1);
  REQUIRE(sonare_synth_gs_variation_is_voiced_apart(0xFFFF, 0) == -1);
  REQUIRE(sonare_synth_gs_variation_is_voiced_apart(127, 0) != -1);  // the last in range
  REQUIRE(sonare_synth_gs_variation_is_voiced_apart(0, -1) == -1);
  REQUIRE(sonare_synth_gs_variation_is_voiced_apart(0, 128) == -1);
}

TEST_CASE("sonare_engine synth instrument renders live MIDI input", "[c_api][synth_patch]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  SonareSynthPatch patch{};
  REQUIRE(sonare_synth_preset_patch("saw-lead", &patch) == SONARE_OK);
  REQUIRE(sonare_engine_set_synth_instrument(engine, 7, &patch) == SONARE_OK);
  size_t count = 0;
  REQUIRE(sonare_engine_midi_instrument_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 1);

  REQUIRE(sonare_engine_push_midi_note_on(engine, 7, 0, 0, 60, 100, -1) == SONARE_OK);
  std::vector<float> left(128, 0.0f);
  std::vector<float> right(128, 0.0f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
  float peak = 0.0f;
  for (float s : left) peak = std::max(peak, std::abs(s));
  for (float s : right) peak = std::max(peak, std::abs(s));
  REQUIRE(peak > 0.0f);

  // Invalid patches are rejected without disturbing the binding.
  SonareSynthPatch bad{};
  bad.struct_version = 9;
  REQUIRE(sonare_engine_set_synth_instrument(engine, 7, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_synth_instrument(engine, 7, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_midi_instrument_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 1);

  REQUIRE(sonare_engine_clear_midi_instrument(engine, 7) == SONARE_OK);
  sonare_engine_destroy(engine);
}

TEST_CASE("every mod source ordinal survives the C conversion", "[project][synth_patch]") {
  // The clamp in mod_source_from_c is the one place a new ordinal can be lost
  // without anything going red: a value past its upper bound comes back as
  // kNone, so the route is silently disabled and the render is merely quiet.
  // Walking the whole domain is what catches an upper bound left behind.
  using sonare::midi::synth::ModSource;
  for (int ordinal = 0; ordinal < SONARE_SYNTH_MOD_SOURCE_COUNT; ++ordinal) {
    CAPTURE(ordinal);
    REQUIRE(static_cast<int>(sonare_c_detail::mod_source_from_c(ordinal)) == ordinal);
  }
  REQUIRE(sonare_c_detail::mod_source_from_c(SONARE_SYNTH_MOD_SOURCE_COUNT) == ModSource::kNone);
  REQUIRE(sonare_c_detail::mod_source_from_c(-1) == ModSource::kNone);
}

#endif  // SONARE_WITH_ARRANGEMENT
