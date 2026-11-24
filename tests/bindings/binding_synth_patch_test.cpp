/// @file binding_synth_patch_test.cpp
/// @brief NativeSynth C ABI surface: the preset catalog
///        (sonare_synth_preset_names / sonare_synth_preset_patch), the
///        patch-driven bounce (the synth_instruments list of sonare_project_bounce)
///        and the realtime engine entry (sonare_engine_set_synth_instrument).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "binding_project_parity_test_helpers.h"
#include "c_api/synth_patch_common.h"
#include "support/c_strings.h"
#include "support/midi_render.h"
#include "support/project_bounce.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/json.h"

namespace {

// The validator throws a refusal naming the field; these cases read its verdict and its text.
bool synth_patch_converts(const SonareSynthPatch& patch,
                          sonare::midi::synth::NativeSynthConfig* cfg, std::string* error) {
  try {
    *cfg = sonare_c_detail::synth_config_from_patch_c(patch, "patch");
    error->clear();
    return true;
  } catch (const sonare::SonareException& e) {
    *error = e.what();
    return false;
  }
}

// The C field behind an automation-table key: cutoffHz is cutoff_hz.
std::string snake_key(const std::string& key) {
  std::string out;
  for (const char ch : key) {
    if (ch >= 'A' && ch <= 'Z') {
      out += '_';
      out += static_cast<char>(ch - 'A' + 'a');
    } else {
      out += ch;
    }
  }
  return out;
}

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

// Binds a patch-only NativeSynth (no GM programs, no sample bank) on a destination.
SonareError set_synth_patch(SonareEngine* engine, uint32_t destination_id,
                            const SonareSynthPatch* patch) {
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = destination_id;
  binding.patch = *patch;
  return sonare_engine_set_synth_instrument(engine, &binding);
}

}  // namespace

namespace {

/// A project with one MIDI track routed to destination @p dest playing a held
/// note (on at 0, off at 2 ppq).
SonareProject* make_synth_project(uint32_t dest, uint8_t note = 60) {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000) == SONARE_OK);
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);
  SonareMidiEventPod events[2]{};
  events[0].ppq = 0.0;
  events[0].word0 = 0x20900040u | (static_cast<uint32_t>(note) << 8);  // note-on, vel 64
  events[0].word1 = 0u;
  events[1].ppq = 2.0;
  events[1].word0 = 0x20800000u | (static_cast<uint32_t>(note) << 8);  // note-off
  events[1].word1 = 0u;
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, dest) == SONARE_OK);
  return project;
}

std::vector<float> bounce_synth(SonareProject* project, const SonareSynthPatch& patch,
                                uint32_t dest = 3) {
  SonareProjectBounceOptions options;
  sonare_project_bounce_options_init(&options);
  options.total_frames = 24000;
  options.block_size = 128;
  options.num_channels = 2;

  SonareSynthInstrumentBinding binding{};
  binding.destination_id = dest;
  binding.patch = patch;
  float* out = nullptr;
  size_t out_len = 0;
  int bounce_channels1 = 0;
  REQUIRE(bounce_with_synth_instruments(project, &options, &binding, 1, &out, &out_len,
                                        &bounce_channels1) == SONARE_OK);
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

sonare::util::json::Value parse_info(const std::string& info) {
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
  // Present only when set, like `integer`: alternatives, none of them empty.
  if (d.contains("activeWhen")) {
    REQUIRE(d["activeWhen"].is_array());
    REQUIRE(d["activeWhen"].size() > 0);
    for (const auto& alternative : d["activeWhen"].as_array()) {
      REQUIRE(alternative.is_array());
      REQUIRE(alternative.size() > 0);
      for (const auto& c : alternative.as_array()) {
        REQUIRE(c["key"].is_string());
        const std::string& scope = c["scope"].as_string();
        REQUIRE((scope == "engine" || scope == "patch"));
        const std::set<std::string> relations = {"eq", "ne", "gt", "ge", "lt", "le"};
        REQUIRE(relations.count(c["relation"].as_string()) == 1);
        REQUIRE((c["value"].is_number() || c["value"].is_bool() || c["value"].is_string()));
      }
    }
  }
}
#endif

}  // namespace

TEST_CASE("synth preset catalog lists every §E entry", "[project][synth_patch]") {
  const char* names = sonare_synth_preset_names();
  REQUIRE(names != nullptr);
#if defined(SONARE_WITH_ARRANGEMENT)
  const std::string joined = std::string("\n") + names + "\n";
  for (const char* expected :
       {"sine", "sawLead", "squareLead", "subBass", "warmPad", "ePiano", "bell", "brass", "pluck",
        "electricGuitar", "harp", "marimba", "glass", "organ", "drumKit", "acousticPiano"}) {
    INFO(expected);
    REQUIRE(joined.find(std::string("\n") + expected + "\n") != std::string::npos);
  }
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)

TEST_CASE("synth preset patches round-trip through the versioned struct",
          "[project][synth_patch]") {
  SonareSynthPatch patch{};
  // warm-pad: two subtractive saws spread across the field.
  REQUIRE(sonare_synth_preset_patch("warmPad", &patch) == SONARE_OK);
  REQUIRE(patch.struct_size == sizeof(SonareSynthPatch));
  REQUIRE(std::string(patch.preset) == "warmPad");
  REQUIRE(patch.engine_mode == SONARE_SYNTH_ENGINE_SUBTRACTIVE);
  REQUIRE(patch.waveform == SONARE_SYNTH_OSC_SAW);
  REQUIRE(patch.unison == 2);
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
  REQUIRE(sonare_synth_preset_patch("ePiano", &patch) == SONARE_OK);
  REQUIRE(patch.engine_mode == SONARE_SYNTH_ENGINE_FM);
  REQUIRE(sonare_synth_preset_patch("electricGuitar", &patch) == SONARE_OK);
  REQUIRE(patch.engine_mode == SONARE_SYNTH_ENGINE_KARPLUS_STRONG);
  REQUIRE(sonare_synth_preset_patch("acousticPiano", &patch) == SONARE_OK);
  REQUIRE(patch.engine_mode == SONARE_SYNTH_ENGINE_PIANO);
  // Unknown names and NULL args are rejected.
  REQUIRE(sonare_synth_preset_patch("minimoog", &patch) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_synth_preset_patch(nullptr, &patch) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_synth_preset_patch("sine", nullptr) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("synth patch enum counts match the public C ordinals", "[project][synth_patch]") {
  REQUIRE(SONARE_SYNTH_ENGINE_DEFAULT == 0);
  REQUIRE(SONARE_SYNTH_ENGINE_SINGING + 1 == SONARE_SYNTH_ENGINE_MODE_COUNT);
  REQUIRE(SONARE_SYNTH_OSC_DEFAULT == 0);
  REQUIRE(SONARE_SYNTH_OSC_NOISE + 1 == SONARE_SYNTH_OSC_WAVEFORM_COUNT);
  REQUIRE(SONARE_SYNTH_FILTER_DEFAULT == 0);
  REQUIRE(SONARE_SYNTH_FILTER_SVF_4POLE + 1 == SONARE_SYNTH_FILTER_MODEL_COUNT);
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
    sonare_synth_patch_init(&patch);
    patch.engine_mode = mode;
    CAPTURE(mode);
    const float peak = peak_of(bounce_synth(project, patch));
    if (!(peak > 1.0e-3f)) inaudible.insert(mode);
    if (mode == SONARE_SYNTH_ENGINE_SAMPLE) REQUIRE(peak == 0.0f);
  }
  REQUIRE(inaudible == std::set<int>{SONARE_SYNTH_ENGINE_SAMPLE});

  sonare_project_destroy(project);
}

TEST_CASE("every catalog preset name fits the patch preset field whole", "[project][synth_patch]") {
  // A patch read back from a preset copies its name into the fixed field, so no name may be cut.
  for (size_t i = 0; i < sonare::midi::synth::synth_preset_count(); ++i) {
    const sonare::midi::synth::SynthPreset* preset = sonare::midi::synth::synth_preset_at(i);
    REQUIRE(preset != nullptr);
    INFO(preset->name);
    CHECK(std::strlen(preset->name) < SONARE_SYNTH_PRESET_NAME_MAX);
  }
}

TEST_CASE("every named synth patch renders as its catalog preset", "[project][synth_patch]") {
  // A patch naming a preset of the engine it selects must reach the voice as
  // that preset, whatever else the conversion seeds or overlays.
  for (size_t i = 0; i < sonare::midi::synth::synth_preset_count(); ++i) {
    const sonare::midi::synth::SynthPreset* preset = sonare::midi::synth::synth_preset_at(i);
    REQUIRE(preset != nullptr);
    INFO(preset->name);
    SonareSynthPatch patch{};
    sonare_synth_patch_init(&patch);
    std::strncpy(patch.preset, preset->name, SONARE_SYNTH_PRESET_NAME_MAX - 1);
    sonare::midi::synth::NativeSynthConfig cfg;
    std::string error;
    REQUIRE(synth_patch_converts(patch, &cfg, &error));
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
    sonare_synth_patch_init(&patch);
    patch.engine_mode = mode;
    sonare::midi::synth::NativeSynthConfig cfg;
    std::string error;
    REQUIRE(synth_patch_converts(patch, &cfg, &error));
    require_section_read_back(cfg.patch, base);
    const sonare::test::StereoRender expected =
        render_config(with_section_of(sonare::midi::synth::NativeSynthConfig{}, engine, base));
    const sonare::test::StereoRender converted = render_config(cfg);
    REQUIRE(converted.left == expected.left);
    REQUIRE(converted.right == expected.right);
  }

  // A preset of another engine keeps its wrapper and takes the selected
  // engine's base section: the violin's wrapper over the e-piano's operators.
  REQUIRE(std::string(sonare::midi::synth::base_preset_name(SynthEngineMode::kFm)) == "ePiano");
  SonareSynthPatch violin_fm{};
  sonare_synth_patch_init(&violin_fm);
  std::strcpy(violin_fm.preset, "violin");
  violin_fm.engine_mode = SONARE_SYNTH_ENGINE_FM;
  sonare::midi::synth::NativeSynthConfig cfg;
  std::string error;
  REQUIRE(synth_patch_converts(violin_fm, &cfg, &error));
  require_section_read_back(cfg.patch, "ePiano");
  const sonare::midi::synth::SynthPreset* violin = sonare::midi::synth::find_synth_preset("violin");
  REQUIRE(violin != nullptr);
  const sonare::test::StereoRender expected =
      render_config(with_section_of(violin->config, SynthEngineMode::kFm, "ePiano"));
  const sonare::test::StereoRender converted = render_config(cfg);
  REQUIRE(converted.left == expected.left);
  REQUIRE(converted.right == expected.right);
  REQUIRE(peak_of(converted.left) > 1.0e-3f);
}

TEST_CASE("synth patch engine params are refused through the C ABI with the key named",
          "[project][synth_patch]") {
  SonareProject* project = make_synth_project(3);
  SonareProjectBounceOptions options;
  sonare_project_bounce_options_init(&options);
  options.total_frames = 1024;
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = 3;
  float* out = nullptr;
  size_t out_len = 0;

  // bowForce: a bounded number of the bowed-string section.
  const sonare::util::json::Value bowed = parse_info(
      sonare_test::c_string(sonare_synth_engine_param_info, SONARE_SYNTH_ENGINE_BOWED_STRING));
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
    const sonare::util::json::Value info =
        parse_info(sonare_test::c_string(sonare_synth_engine_param_info, mode));
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
    sonare_synth_patch_init(&patch);
    patch.engine_mode = mode;
    patch.engine_params = &param;
    patch.engine_param_count = 1;
    binding.patch = patch;
    out = nullptr;
    int bounce_channels2 = 0;
    const SonareError status = bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                             &out_len, &bounce_channels2);
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
  sonare_synth_patch_init(&dangling);
  dangling.engine_param_count = 1;
  binding.patch = dangling;
  int bounce_channels3 = 0;
  REQUIRE(bounce_with_synth_instruments(project, &options, &binding, 1, &out, &out_len,
                                        &bounce_channels3) == SONARE_ERROR_INVALID_PARAMETER);
  // A count of zero needs no array.
  SonareSynthPatch none = dangling;
  none.engine_param_count = 0;
  binding.patch = none;
  int bounce_channels4 = 0;
  REQUIRE(bounce_with_synth_instruments(project, &options, &binding, 1, &out, &out_len,
                                        &bounce_channels4) == SONARE_OK);
  sonare_free_floats(out);

  sonare_project_destroy(project);
}

TEST_CASE("an engine param reaches the render", "[project][synth_patch]") {
  SonareProject* project = make_synth_project(3);
  SonareSynthPatch base{};
  sonare_synth_patch_init(&base);
  std::strcpy(base.preset, "violin");
  const std::vector<float> reference = bounce_synth(project, base);
  REQUIRE(peak_of(reference) > 0.0f);
  // The middle of bowForce's range, which the violin does not sit on.
  double mid = 0.0;
  const sonare::util::json::Value bowed = parse_info(
      sonare_test::c_string(sonare_synth_engine_param_info, SONARE_SYNTH_ENGINE_BOWED_STRING));
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

TEST_CASE("the Karplus-Strong second plane's detune is a key in cents that reaches the render",
          "[project][synth_patch]") {
  const sonare::util::json::Value info = parse_info(
      sonare_test::c_string(sonare_synth_engine_param_info, SONARE_SYNTH_ENGINE_KARPLUS_STRONG));
  bool listed = false;
  for (const auto& d : info.as_array()) {
    if (d["name"].as_string() != "polDetuneCents") continue;
    listed = true;
    CHECK(d["unit"].as_string() == "cents");
    CHECK(d["min"].as_number() == -50.0);
    CHECK(d["max"].as_number() == 50.0);
  }
  REQUIRE(listed);

  SonareProject* project = make_synth_project(3);
  SonareSynthPatch base{};
  sonare_synth_patch_init(&base);
  std::strcpy(base.preset, "harp");
  const SonareSynthEngineParam plane{"polarization", 0.3};
  SonareSynthPatch two_planes = base;
  two_planes.engine_params = &plane;
  two_planes.engine_param_count = 1;
  const std::vector<float> reference = bounce_synth(project, two_planes);
  REQUIRE(peak_of(reference) > 0.0f);
  const SonareSynthEngineParam unison[] = {{"polarization", 0.3}, {"polDetuneCents", 0.5}};
  SonareSynthPatch close = base;
  close.engine_params = unison;
  close.engine_param_count = 2;
  REQUIRE(bounce_synth(project, close) != reference);
  sonare_project_destroy(project);
}

TEST_CASE("the subtractive section is set by key and reaches the render",
          "[project][synth_patch]") {
  const sonare::util::json::Value info = parse_info(
      sonare_test::c_string(sonare_synth_engine_param_info, SONARE_SYNTH_ENGINE_SUBTRACTIVE));
  std::set<std::string> names;
  const std::set<std::string> levels{"osc2Level", "noiseLevel", "noiseAm", "bandNoiseLevel"};
  for (const auto& d : info.as_array()) {
    names.insert(d["name"].as_string());
    // Every source defaults to absent.
    if (levels.count(d["name"].as_string()) != 0) REQUIRE(d["default"].as_number() == 0.0);
  }
  REQUIRE(names == std::set<std::string>{
                       "osc2IntervalCents", "osc2Level", "osc2Dry", "noiseLevel", "noiseAm",
                       "bandNoiseLevel", "bandNoiseLowHz", "bandNoiseLowKeyTrack",
                       "bandNoiseHighHz", "bandNoiseHighKeyTrack", "pulseWidth", "unisonWidth",
                       "velocityExponent", "sustainKeyBreak", "sustainKeySlopeDb"});

  SonareProject* project = make_synth_project(3);
  SonareSynthPatch base{};
  sonare_synth_patch_init(&base);
  std::strcpy(base.preset, "saw");
  const std::vector<float> reference = bounce_synth(project, base);
  REQUIRE(peak_of(reference) > 0.0f);
  for (const char* key : {"osc2Level", "noiseLevel", "noiseAm", "bandNoiseLevel"}) {
    CAPTURE(key);
    const SonareSynthEngineParam param{key, 0.5};
    SonareSynthPatch with = base;
    with.engine_params = &param;
    with.engine_param_count = 1;
    REQUIRE(bounce_synth(project, with) != reference);
  }
  // The interval alone places an oscillator nobody hears, as its gate states.
  const SonareSynthEngineParam interval{"osc2IntervalCents", 702.0};
  SonareSynthPatch interval_only = base;
  interval_only.engine_params = &interval;
  interval_only.engine_param_count = 1;
  REQUIRE(bounce_synth(project, interval_only) == reference);
  const SonareSynthEngineParam beyond{"osc2IntervalCents", 2401.0};
  SonareSynthPatch refused = base;
  refused.engine_params = &beyond;
  refused.engine_param_count = 1;
  sonare::midi::synth::NativeSynthConfig cfg;
  std::string error;
  REQUIRE_FALSE(synth_patch_converts(refused, &cfg, &error));
  REQUIRE(std::string(error).find("osc2IntervalCents") != std::string::npos);
  sonare_project_destroy(project);
}

TEST_CASE("synth engine param info lists exactly the walker's engine section",
          "[project][synth_patch]") {
  for (int mode = 0; mode < SONARE_SYNTH_ENGINE_MODE_COUNT; ++mode) {
    CAPTURE(mode);
    const sonare::util::json::Value info =
        parse_info(sonare_test::c_string(sonare_synth_engine_param_info, mode));
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
  // Exactly these two have no section of their own.
  for (int mode : {SONARE_SYNTH_ENGINE_DEFAULT, SONARE_SYNTH_ENGINE_SAMPLE}) {
    REQUIRE(sonare_test::c_string(sonare_synth_engine_param_info, mode) == "[]");
  }
  for (int mode : {-1, SONARE_SYNTH_ENGINE_MODE_COUNT}) {
    CAPTURE(mode);
    REQUIRE(sonare_test::c_string_error(sonare_synth_engine_param_info, mode) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("synth engine param gates name a key of their scope with a value it can take",
          "[project][synth_patch]") {
  // The wrapper enum fields a gate may name, with their vocabulary.
  const auto vocabulary = [](int kind) {
    std::set<std::string> names;
    std::string joined = sonare_synth_enum_names(kind);
    size_t start = 0;
    while (start <= joined.size()) {
      const size_t end = std::min(joined.find('\n', start), joined.size());
      names.insert(joined.substr(start, end - start));
      start = end + 1;
    }
    return names;
  };
  const std::map<std::string, std::set<std::string>> enum_fields{
      {"body", vocabulary(SONARE_SYNTH_ENUM_BODY_TYPE)},
      {"waveform", vocabulary(SONARE_SYNTH_ENUM_OSC_WAVEFORM)}};
  REQUIRE(enum_fields.at("body").count("violin") == 1);
  REQUIRE(enum_fields.at("waveform").count("square") == 1);
  const sonare::util::json::Value patch_info =
      parse_info(sonare_test::c_string(sonare_synth_patch_param_info));

  size_t conditions = 0;
  size_t patch_conditions = 0;
  for (int mode = 0; mode < SONARE_SYNTH_ENGINE_MODE_COUNT; ++mode) {
    CAPTURE(mode);
    const sonare::util::json::Value info =
        parse_info(sonare_test::c_string(sonare_synth_engine_param_info, mode));
    const auto named = [](const sonare::util::json::Value& table, const std::string& key) {
      const sonare::util::json::Value* found = nullptr;
      for (const auto& d : table.as_array()) {
        if (d["name"].as_string() == key) found = &d;
      }
      return found;
    };
    for (const auto& d : info.as_array()) {
      if (!d.contains("activeWhen")) continue;
      for (const auto& alternative : d["activeWhen"].as_array()) {
        for (const auto& c : alternative.as_array()) {
          const std::string& key = c["key"].as_string();
          CAPTURE(d["name"].as_string(), key);
          ++conditions;
          REQUIRE(key != d["name"].as_string());
          const sonare::util::json::Value& value = c["value"];
          if (c["scope"].as_string() == "patch") {
            ++patch_conditions;
            const auto field = enum_fields.find(key);
            if (field != enum_fields.end()) {
              REQUIRE(value.is_string());
              REQUIRE(field->second.count(value.as_string()) == 1);
              continue;
            }
            const sonare::util::json::Value* target = named(patch_info, key);
            REQUIRE(target != nullptr);
            REQUIRE(value.is_number() != ((*target)["type"].as_string() == "boolean"));
            if (target->contains("min")) {
              REQUIRE((*target)["min"].as_number() <= value.as_number());
              REQUIRE(value.as_number() <= (*target)["max"].as_number());
            }
            continue;
          }
          const sonare::util::json::Value* target = named(info, key);
          REQUIRE(target != nullptr);
          REQUIRE(value.is_bool() == ((*target)["type"].as_string() == "boolean"));
          if (value.is_number() && target->contains("min")) {
            REQUIRE((*target)["min"].as_number() <= value.as_number());
            REQUIRE(value.as_number() <= (*target)["max"].as_number());
          }
        }
      }
    }
  }
  REQUIRE(conditions > 0);
  REQUIRE(patch_conditions > 0);

  // The indexed threshold reads the slot's own index, and the emitted order is the table's.
  const std::string percussion =
      sonare_test::c_string(sonare_synth_engine_param_info, SONARE_SYNTH_ENGINE_PERCUSSION);
  const size_t at = percussion.find("{\"name\":\"modeRatios6\"");
  REQUIRE(at != std::string::npos);
  const std::string row = percussion.substr(at, percussion.find("]]}", at) + 3 - at);
  REQUIRE(row.find(",\"activeWhen\":[[{\"key\":\"gmKit\",\"scope\":\"engine\",\"relation\":"
                   "\"eq\",\"value\":false},{\"key\":\"numModes\",\"scope\":\"engine\","
                   "\"relation\":\"gt\",\"value\":6}]]}") != std::string::npos);
  // A field reached through either of two paths states both.
  const sonare::util::json::Value parsed = parse_info(percussion.c_str());
  for (const auto& d : parsed.as_array()) {
    if (d["name"].as_string() != "contactMs") continue;
    REQUIRE(d["activeWhen"].size() == 2);
  }
}

TEST_CASE("synth patch param info describes every numeric patch field", "[project][synth_patch]") {
  // The numeric and switch SonareSynthPatch fields under their binding names;
  // enums, the preset name, the mod matrix, the presence masks and the engine
  // params are not parameters of their own.
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
                                        "sampleHoldHz",    "bitDepth",          "pitchOffsetCents",
                                        "driftRateHz",     "ampDelayMs",        "ampHoldMs",
                                        "filterDelayMs",   "filterHoldMs",      "oneShot"};
  const sonare::util::json::Value info =
      parse_info(sonare_test::c_string(sonare_synth_patch_param_info));
  std::set<std::string> names;
  const auto ends_with = [](const std::string& name, const std::string& suffix) {
    return name.size() > suffix.size() &&
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  for (const auto& d : info.as_array()) {
    if (d["type"].as_string() == "enum") continue;
    require_descriptor_shape(d);
    const std::string& name = d["name"].as_string();
    const std::string& unit = d["unit"].as_string();
    CAPTURE(name, unit);
    names.insert(name);
    if (ends_with(name, "Hz")) REQUIRE(unit == "Hz");
    if (ends_with(name, "Ms")) REQUIRE(unit == "ms");
    if (ends_with(name, "Cents")) REQUIRE(unit == "cents");
  }
  REQUIRE(names.size() == fields.size());
  REQUIRE(names == fields);
  // The automation table's names are this table's names.
  for (size_t i = 0; i < sonare::midi::synth::native_synth_param_count(); ++i) {
    REQUIRE(fields.count(sonare::midi::synth::native_synth_param_name_at(i)) == 1);
  }
}

TEST_CASE("synth patch enum catalog rows use the shared C vocabularies",
          "[project][synth_patch][catalog]") {
  const std::vector<std::pair<std::string, int>> fields = {
      {"waveform", SONARE_SYNTH_ENUM_OSC_WAVEFORM},
      {"filterModel", SONARE_SYNTH_ENUM_FILTER_MODEL},
      {"filterOutput", SONARE_SYNTH_ENUM_FILTER_OUTPUT},
      {"body", SONARE_SYNTH_ENUM_BODY_TYPE},
      {"retrigger", SONARE_SYNTH_ENUM_RETRIGGER},
      {"lfoShape", SONARE_SYNTH_ENUM_LFO_SHAPE},
      {"sampleLoop", SONARE_SYNTH_ENUM_SAMPLE_LOOP_MODE},
      {"sampleKeyTrack", SONARE_SYNTH_ENUM_SAMPLE_KEY_TRACK},
  };
  const sonare::util::json::Value info =
      parse_info(sonare_test::c_string(sonare_synth_patch_param_info));
  std::map<std::string, const sonare::util::json::Value*> rows;
  for (const auto& d : info.as_array()) {
    if (d["type"].as_string() == "enum") {
      REQUIRE(rows.emplace(d["name"].as_string(), &d).second);
    }
  }
  REQUIRE(rows.size() == fields.size());

  for (const auto& [name, kind] : fields) {
    CAPTURE(name, kind);
    const auto row = rows.find(name);
    REQUIRE(row != rows.end());
    const sonare::util::json::Value& d = *row->second;
    REQUIRE(d["type"].as_string() == "enum");
    REQUIRE(d["values"].is_array());
    REQUIRE(d["values"].size() > 0);
    REQUIRE(d["default"].is_string());
    REQUIRE(d["values"][0].as_string() == "default");
    REQUIRE(d["default"].as_string() == d["values"][0].as_string());

    const std::string joined = sonare_synth_enum_names(kind);
    size_t start = 0;
    size_t i = 0;
    while (start < joined.size()) {
      const size_t end = joined.find('\n', start);
      const size_t length = end == std::string::npos ? joined.size() - start : end - start;
      REQUIRE(i < d["values"].size());
      REQUIRE(d["values"][i].as_string() == joined.substr(start, length));
      ++i;
      if (end == std::string::npos) break;
      start = end + 1;
    }
    REQUIRE(i == d["values"].size());
  }
}

TEST_CASE("synth patch conversion rejects out-of-range enum fields", "[project][synth_patch]") {
  SonareSynthPatch patch{};
  sonare_synth_patch_init(&patch);
  sonare::midi::synth::NativeSynthConfig cfg;
  std::string error;

  auto rejected = [&](auto mutate) {
    SonareSynthPatch invalid{};
    sonare_synth_patch_init(&invalid);
    mutate(invalid);
    error.clear();
    return !synth_patch_converts(invalid, &cfg, &error) && !error.empty();
  };

  REQUIRE(rejected([](SonareSynthPatch& p) { p.engine_mode = SONARE_SYNTH_ENGINE_MODE_COUNT; }));
  REQUIRE(rejected([](SonareSynthPatch& p) { p.waveform = SONARE_SYNTH_OSC_WAVEFORM_COUNT; }));
  REQUIRE(rejected([](SonareSynthPatch& p) { p.filter_model = SONARE_SYNTH_FILTER_MODEL_COUNT; }));
  REQUIRE(
      rejected([](SonareSynthPatch& p) { p.filter_output = SONARE_SYNTH_FILTER_OUTPUT_COUNT; }));
  REQUIRE(rejected([](SonareSynthPatch& p) { p.body = SONARE_SYNTH_BODY_TYPE_COUNT; }));
  REQUIRE(rejected([](SonareSynthPatch& p) { p.retrigger = SONARE_SYNTH_RETRIGGER_COUNT; }));
  REQUIRE(rejected([](SonareSynthPatch& p) { p.retrigger = -1; }));
  REQUIRE(rejected([](SonareSynthPatch& p) { p.lfo_shape = SONARE_SYNTH_LFO_SHAPE_COUNT; }));
  REQUIRE(rejected([](SonareSynthPatch& p) { p.lfo_shape = -1; }));

  REQUIRE(synth_patch_converts(patch, &cfg, &error));
}

TEST_CASE("synth patch conversion refuses a master gain outside [0, 4]",
          "[project][synth_patch][gain]") {
  sonare::midi::synth::NativeSynthConfig cfg;
  std::string error;
  auto convert = [&](float gain) {
    SonareSynthPatch patch{};
    sonare_synth_patch_init(&patch);
    patch.gain = gain;
    patch.present_fields |= SONARE_SYNTH_FIELD_GAIN;
    error.clear();
    return synth_patch_converts(patch, &cfg, &error);
  };

  for (const float bad : {4.5f, 16.0f, -0.5f, std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity()}) {
    REQUIRE_FALSE(convert(bad));
    REQUIRE(!error.empty());
    REQUIRE(std::string(error).find("gain") != std::string::npos);
  }
  REQUIRE(convert(4.0f));
  REQUIRE(cfg.gain == 4.0f);
  REQUIRE(convert(0.0f));
  REQUIRE(cfg.gain == 0.0f);
}

namespace {

struct BoundedPatchField {
  const char* key;
  float SonareSynthPatch::*member;
  uint32_t bit;
  bool second_word;
};

// Every SonareSynthPatch float the automation table bounds, under its facade name.
const BoundedPatchField kBoundedPatchFields[] = {
    {"gain", &SonareSynthPatch::gain, SONARE_SYNTH_FIELD_GAIN, false},
    {"busDrive", &SonareSynthPatch::bus_drive, SONARE_SYNTH_FIELD_BUS_DRIVE, false},
    {"cutoffHz", &SonareSynthPatch::cutoff_hz, SONARE_SYNTH_FIELD_CUTOFF_HZ, false},
    {"resonanceQ", &SonareSynthPatch::resonance_q, SONARE_SYNTH_FIELD_RESONANCE_Q, false},
    {"drive", &SonareSynthPatch::drive, SONARE_SYNTH_FIELD_DRIVE, false},
    {"keyTrack", &SonareSynthPatch::key_track, SONARE_SYNTH_FIELD_KEY_TRACK, false},
    {"envToCutoffCents", &SonareSynthPatch::env_to_cutoff_cents,
     SONARE_SYNTH_FIELD_ENV_TO_CUTOFF_CENTS, false},
    {"velToCutoffCents", &SonareSynthPatch::vel_to_cutoff_cents,
     SONARE_SYNTH_FIELD_VEL_TO_CUTOFF_CENTS, false},
    {"ampAttackMs", &SonareSynthPatch::amp_attack_ms, SONARE_SYNTH_FIELD_AMP_ATTACK_MS, false},
    {"ampDecayMs", &SonareSynthPatch::amp_decay_ms, SONARE_SYNTH_FIELD_AMP_DECAY_MS, false},
    {"ampSustain", &SonareSynthPatch::amp_sustain, SONARE_SYNTH_FIELD_AMP_SUSTAIN, false},
    {"ampReleaseMs", &SonareSynthPatch::amp_release_ms, SONARE_SYNTH_FIELD_AMP_RELEASE_MS, false},
    {"filterAttackMs", &SonareSynthPatch::filter_attack_ms, SONARE_SYNTH_FIELD_FILTER_ATTACK_MS,
     false},
    {"filterDecayMs", &SonareSynthPatch::filter_decay_ms, SONARE_SYNTH_FIELD_FILTER_DECAY_MS,
     false},
    {"filterSustain", &SonareSynthPatch::filter_sustain, SONARE_SYNTH_FIELD_FILTER_SUSTAIN, false},
    {"filterReleaseMs", &SonareSynthPatch::filter_release_ms, SONARE_SYNTH_FIELD_FILTER_RELEASE_MS,
     false},
    {"lfoRateHz", &SonareSynthPatch::lfo_rate_hz, SONARE_SYNTH_FIELD_LFO_RATE_HZ, false},
    {"lfoToPitchCents", &SonareSynthPatch::lfo_to_pitch_cents,
     SONARE_SYNTH_FIELD_LFO_TO_PITCH_CENTS, false},
    {"lfo2RateHz", &SonareSynthPatch::lfo2_rate_hz, SONARE_SYNTH_FIELD_LFO2_RATE_HZ, false},
    {"glideMs", &SonareSynthPatch::glide_ms, SONARE_SYNTH_FIELD_GLIDE_MS, false},
    {"bodyMix", &SonareSynthPatch::body_mix, SONARE_SYNTH_FIELD_BODY_MIX, false},
    {"stereoSpread", &SonareSynthPatch::stereo_spread, SONARE_SYNTH_FIELD_STEREO_SPREAD, false},
    {"detuneCents", &SonareSynthPatch::detune_cents, SONARE_SYNTH_FIELD_DETUNE_CENTS, false},
    {"driftCents", &SonareSynthPatch::drift_cents, SONARE_SYNTH_FIELD_DRIFT_CENTS, false},
    {"pitchOffsetCents", &SonareSynthPatch::pitch_offset_cents,
     SONARE_SYNTH_FIELD_PITCH_OFFSET_CENTS, false},
    {"hpCutoffHz", &SonareSynthPatch::hp_cutoff_hz, SONARE_SYNTH_FIELD_HP_CUTOFF_HZ, false},
    {"sampleHoldHz", &SonareSynthPatch::sample_hold_hz, SONARE_SYNTH_FIELD_SAMPLE_HOLD_HZ, false},
    {"bitDepth", &SonareSynthPatch::bit_depth, SONARE_SYNTH_FIELD_BIT_DEPTH, false},
    {"driftRateHz", &SonareSynthPatch::drift_rate_hz, SONARE_SYNTH_FIELD2_DRIFT_RATE_HZ, true},
    {"ampDelayMs", &SonareSynthPatch::amp_delay_ms, SONARE_SYNTH_FIELD2_AMP_DELAY_MS, true},
    {"ampHoldMs", &SonareSynthPatch::amp_hold_ms, SONARE_SYNTH_FIELD2_AMP_HOLD_MS, true},
    {"filterDelayMs", &SonareSynthPatch::filter_delay_ms, SONARE_SYNTH_FIELD2_FILTER_DELAY_MS,
     true},
    {"filterHoldMs", &SonareSynthPatch::filter_hold_ms, SONARE_SYNTH_FIELD2_FILTER_HOLD_MS, true},
};

const BoundedPatchField* find_bounded_patch_field(const std::string& key) {
  for (const BoundedPatchField& f : kBoundedPatchFields) {
    if (key == f.key) return &f;
  }
  return nullptr;
}

// Converts a current-version patch with @p f set to @p value and its presence bit raised.
bool convert_with_field(const BoundedPatchField& f, float value, std::string* error) {
  SonareSynthPatch patch{};
  sonare_synth_patch_init(&patch);
  patch.*(f.member) = value;
  (f.second_word ? patch.present_fields2 : patch.present_fields) |= f.bit;
  sonare::midi::synth::NativeSynthConfig cfg;
  return synth_patch_converts(patch, &cfg, error);
}

}  // namespace

TEST_CASE("synth patch conversion refuses each bounded field at the range the info publishes",
          "[project][synth_patch]") {
  const sonare::util::json::Value info =
      parse_info(sonare_test::c_string(sonare_synth_patch_param_info));
  const auto published = [&](const std::string& name) -> const sonare::util::json::Value* {
    for (const auto& d : info.as_array()) {
      if (d["name"].as_string() == name) return &d;
    }
    return nullptr;
  };

  const size_t count = sonare::midi::synth::native_synth_param_count();
  REQUIRE(count == std::size(kBoundedPatchFields));
  for (size_t i = 0; i < count; ++i) {
    const std::string key = sonare::midi::synth::native_synth_param_name_at(i);
    CAPTURE(key);
    const BoundedPatchField* field = find_bounded_patch_field(key);
    REQUIRE(field != nullptr);
    const sonare::util::json::Value* row = published(key);
    REQUIRE(row != nullptr);
    const float lo = static_cast<float>((*row)["min"].as_number());
    const float hi = static_cast<float>((*row)["max"].as_number());

    std::string error;
    REQUIRE(convert_with_field(*field, lo, &error));
    REQUIRE(convert_with_field(*field, hi, &error));
    for (const float bad : {std::nextafter(lo, -std::numeric_limits<float>::infinity()),
                            std::nextafter(hi, std::numeric_limits<float>::infinity())}) {
      REQUIRE_FALSE(convert_with_field(*field, bad, &error));
      REQUIRE(!error.empty());
      REQUIRE(error.find("patch." + snake_key(key) + " must be finite and in [") == 0);
    }
    for (const float bad :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
      REQUIRE_FALSE(convert_with_field(*field, bad, &error));
      REQUIRE(!error.empty());
      REQUIRE(error.find("patch." + snake_key(key) + " ") == 0);
    }
  }
}

TEST_CASE("synth patch conversion refuses out-of-range values rather than clamping them",
          "[project][synth_patch]") {
  sonare::midi::synth::NativeSynthConfig cfg;
  std::string error;
  const auto refused = [&](auto mutate, const char* key) {
    SonareSynthPatch patch{};
    sonare_synth_patch_init(&patch);
    mutate(patch);
    error.clear();
    return !synth_patch_converts(patch, &cfg, &error) && !error.empty() &&
           error.find(std::string("patch.") + key + " ") == 0;
  };

  REQUIRE(refused([](SonareSynthPatch& p) { p.cutoff_hz = 1e6f; }, "cutoff_hz"));
  REQUIRE(refused([](SonareSynthPatch& p) { p.stereo_spread = 5.0f; }, "stereo_spread"));
  REQUIRE(refused(
      [](SonareSynthPatch& p) {
        p.amp_release_ms = 0.0f;
        p.present_fields |= SONARE_SYNTH_FIELD_AMP_RELEASE_MS;
      },
      "amp_release_ms"));
  REQUIRE(refused([](SonareSynthPatch& p) { p.polyphony = 500; }, "polyphony"));
  REQUIRE(refused([](SonareSynthPatch& p) { p.polyphony = -3; }, "polyphony"));
  REQUIRE(refused([](SonareSynthPatch& p) { p.unison = sonare::midi::synth::kMaxUnisonOscs + 1; },
                  "unison"));
  REQUIRE(refused([](SonareSynthPatch& p) { p.unison = -1; }, "unison"));
  REQUIRE(refused(
      [](SonareSynthPatch& p) {
        p.unison = 0;
        p.present_fields |= SONARE_SYNTH_FIELD_UNISON;
      },
      "unison"));
  REQUIRE(
      refused([](SonareSynthPatch& p) { p.num_mod_routings = SONARE_SYNTH_PATCH_MOD_ROUTINGS + 1; },
              "num_mod_routings"));
  REQUIRE(refused(
      [](SonareSynthPatch& p) {
        p.num_mod_routings = -1;
        p.present_fields |= SONARE_SYNTH_FIELD_MOD_ROUTINGS;
      },
      "num_mod_routings"));
  REQUIRE(refused([](SonareSynthPatch& p) { p.num_mod_routings = -1; }, "num_mod_routings"));
  REQUIRE(refused(
      [](SonareSynthPatch& p) {
        p.engine_mode = SONARE_SYNTH_ENGINE_SAMPLE;
        p.sample_level = 100.0f;
      },
      "sample_level"));
  REQUIRE(refused(
      [](SonareSynthPatch& p) {
        p.engine_mode = SONARE_SYNTH_ENGINE_SAMPLE;
        p.sample_start_offset = 2.0f;
      },
      "sample_start_offset"));

  // The ends of the integer ranges and a full routing table are accepted.
  SonareSynthPatch edge{};
  sonare_synth_patch_init(&edge);
  edge.unison = sonare::midi::synth::kMaxUnisonOscs;
  edge.polyphony = sonare::midi::kMaxSynthVoices;
  edge.num_mod_routings = SONARE_SYNTH_PATCH_MOD_ROUTINGS;
  for (int i = 0; i < SONARE_SYNTH_PATCH_MOD_ROUTINGS; ++i) {
    edge.mod_routings[i] = {3 /*lfo1*/, 1 /*pitchCents*/, 10.0f};
  }
  REQUIRE(synth_patch_converts(edge, &cfg, &error));
  REQUIRE(cfg.polyphony == sonare::midi::kMaxSynthVoices);
  edge.unison = 1;
  edge.polyphony = 1;
  REQUIRE(synth_patch_converts(edge, &cfg, &error));
  REQUIRE(cfg.polyphony == 1);

  // A sample patch accepts the published ends of its level and start offset.
  const sonare::util::json::Value info =
      parse_info(sonare_test::c_string(sonare_synth_patch_param_info));
  for (const auto& d : info.as_array()) {
    const std::string& name = d["name"].as_string();
    if (name != "sampleLevel" && name != "sampleStartOffset") continue;
    for (const char* edge_name : {"min", "max"}) {
      SonareSynthPatch patch{};
      sonare_synth_patch_init(&patch);
      patch.engine_mode = SONARE_SYNTH_ENGINE_SAMPLE;
      const float value = static_cast<float>(d[edge_name].as_number());
      (name == "sampleLevel" ? patch.sample_level : patch.sample_start_offset) = value;
      INFO(name << " " << edge_name);
      REQUIRE(synth_patch_converts(patch, &cfg, &error));
    }
  }
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
  REQUIRE(sonare_synth_preset_patch("sawLead", &patch) == SONARE_OK);
  REQUIRE(bounce_synth(project, patch) == bounce_synth(project, patch));

  sonare_project_destroy(project);
}

TEST_CASE("synth patch presence bits express an explicit zero", "[project][synth_patch]") {
  SonareProject* project = make_synth_project(3);

  // warm-pad carries a non-zero stereo spread and bus drive, so turning either
  // off is a real edit that a bare zero cannot express: without a presence bit
  // the zero reads as the "keep base" sentinel and the render is unchanged.
  SonareSynthPatch base{};
  sonare_synth_patch_init(&base);
  std::strncpy(base.preset, "warmPad", SONARE_SYNTH_PRESET_NAME_MAX - 1);
  const std::vector<float> reference = bounce_synth(project, base);
  REQUIRE(peak_of(reference) > 0.0f);

  SonareSynthPatch bare_zero = base;
  bare_zero.stereo_spread = 0.0f;
  bare_zero.bus_drive = 0.0f;
  REQUIRE(bounce_synth(project, bare_zero) == reference);

  SonareSynthPatch explicit_zero = bare_zero;
  explicit_zero.present_fields = SONARE_SYNTH_FIELD_STEREO_SPREAD | SONARE_SYNTH_FIELD_BUS_DRIVE;
  REQUIRE(bounce_synth(project, explicit_zero) != reference);

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
  sonare_synth_patch_init(&base);
  std::strncpy(base.preset, "warmPad", SONARE_SYNTH_PRESET_NAME_MAX - 1);
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
  SonareProjectBounceOptions options;
  sonare_project_bounce_options_init(&options);
  options.total_frames = 1024;
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = 3;
  float* out = nullptr;
  size_t out_len = 0;

  SonareSynthPatch none_source{};
  sonare_synth_patch_init(&none_source);
  none_source.num_mod_routings = 1;
  none_source.mod_routings[0] = {0 /*none*/, 1 /*pitchCents*/, 80.0f};
  binding.patch = none_source;
  int bounce_channels5 = 0;
  REQUIRE(bounce_with_synth_instruments(project, &options, &binding, 1, &out, &out_len,
                                        &bounce_channels5) == SONARE_ERROR_INVALID_PARAMETER);

  SonareSynthPatch none_destination{};
  sonare_synth_patch_init(&none_destination);
  none_destination.num_mod_routings = 1;
  none_destination.mod_routings[0] = {3 /*lfo1*/, 0 /*none*/, 80.0f};
  binding.patch = none_destination;
  int bounce_channels6 = 0;
  REQUIRE(bounce_with_synth_instruments(project, &options, &binding, 1, &out, &out_len,
                                        &bounce_channels6) == SONARE_ERROR_INVALID_PARAMETER);

  // Not vacuous: a routing naming a real source and destination on both ends
  // still passes.
  SonareSynthPatch valid{};
  sonare_synth_patch_init(&valid);
  valid.num_mod_routings = 1;
  valid.mod_routings[0] = {3 /*lfo1*/, 1 /*pitchCents*/, 80.0f};
  binding.patch = valid;
  int bounce_channels7 = 0;
  REQUIRE(bounce_with_synth_instruments(project, &options, &binding, 1, &out, &out_len,
                                        &bounce_channels7) == SONARE_OK);
  sonare_free_floats(out);

  sonare_project_destroy(project);
}

TEST_CASE("synth patch field overrides shape the preset", "[project][synth_patch]") {
  SonareProject* project = make_synth_project(3);

  // A zero-init patch (no preset) is the default subtractive patch.
  SonareSynthPatch init{};
  sonare_synth_patch_init(&init);
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
  SonareSynthPatch too_small{};
  sonare_synth_patch_init(&too_small);
  too_small.struct_size = sizeof(SonareSynthPatch) - 1;
  SonareProjectBounceOptions options;
  sonare_project_bounce_options_init(&options);
  options.total_frames = 1024;
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = 3;
  binding.patch = too_small;
  float* out = nullptr;
  size_t out_len = 0;
  int bounce_channels8 = 0;
  REQUIRE(bounce_with_synth_instruments(project, &options, &binding, 1, &out, &out_len,
                                        &bounce_channels8) == SONARE_ERROR_INVALID_PARAMETER);
  SonareSynthPatch bad_name{};
  sonare_synth_patch_init(&bad_name);
  std::strcpy(bad_name.preset, "no-such-preset");
  binding.patch = bad_name;
  int bounce_channels9 = 0;
  REQUIRE(bounce_with_synth_instruments(project, &options, &binding, 1, &out, &out_len,
                                        &bounce_channels9) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("synth patch numeric zero fields keep the base preset", "[project][synth_patch]") {
  SonareSynthPatch preset_only{};
  sonare_synth_patch_init(&preset_only);
  std::strcpy(preset_only.preset, "warmPad");

  sonare::midi::synth::NativeSynthConfig base;
  std::string error;
  REQUIRE(synth_patch_converts(preset_only, &base, &error));
  REQUIRE(error.empty());
  REQUIRE(base.patch.amp_env.sustain > 0.0f);
  REQUIRE(base.patch.filter_env.sustain > 0.0f);
  REQUIRE(base.gain > 0.0f);

  SonareSynthPatch explicit_zero = preset_only;
  explicit_zero.amp_sustain = 0.0f;
  explicit_zero.filter_sustain = 0.0f;
  explicit_zero.gain = 0.0f;

  sonare::midi::synth::NativeSynthConfig kept;
  REQUIRE(synth_patch_converts(explicit_zero, &kept, &error));
  REQUIRE(error.empty());
  REQUIRE(kept.patch.amp_env.sustain == base.patch.amp_env.sustain);
  REQUIRE(kept.patch.filter_env.sustain == base.patch.filter_env.sustain);
  REQUIRE(kept.gain == base.gain);

  SonareSynthPatch non_zero = preset_only;
  non_zero.amp_sustain = 0.25f;
  non_zero.filter_sustain = 0.125f;
  non_zero.gain = 0.5f;

  sonare::midi::synth::NativeSynthConfig overridden;
  REQUIRE(synth_patch_converts(non_zero, &overridden, &error));
  REQUIRE(error.empty());
  REQUIRE(overridden.patch.amp_env.sustain == 0.25f);
  REQUIRE(overridden.patch.filter_env.sustain == 0.125f);
  REQUIRE(overridden.gain == 0.5f);
}

TEST_CASE("the patch pitch offset is read only by a caller that declares it",
          "[project][synth_patch]") {
  SonareSynthPatch asked{};
  sonare_synth_patch_init(&asked);
  std::strcpy(asked.preset, "clarinet");
  asked.pitch_offset_cents = 700.0f;

  sonare::midi::synth::NativeSynthConfig read;
  std::string error;
  REQUIRE(synth_patch_converts(asked, &read, &error));
  REQUIRE(error.empty());
  REQUIRE(read.patch.pitch_offset_cents == 700.0f);
}

TEST_CASE("the patch retrigger mode is read from a full-size struct",
          "[project][synth_patch][retrigger]") {
  using sonare::midi::synth::SynthRetrigger;
  SonareSynthPatch asked{};
  sonare_synth_patch_init(&asked);
  std::strcpy(asked.preset, "sawLead");
  asked.retrigger = SONARE_SYNTH_RETRIGGER_NOTE;
  sonare::midi::synth::NativeSynthConfig read;
  std::string error;
  REQUIRE(synth_patch_converts(asked, &read, &error));
  REQUIRE(read.patch.retrigger == SynthRetrigger::kNote);

  // Zero keeps the base, which for every catalog preset is free running.
  SonareSynthPatch base = asked;
  base.retrigger = SONARE_SYNTH_RETRIGGER_BASE;
  REQUIRE(synth_patch_converts(base, &read, &error));
  REQUIRE(read.patch.retrigger == SynthRetrigger::kFree);

  // A struct sized below sizeof is refused by name rather than read past its end.
  SonareSynthPatch undersized = asked;
  undersized.struct_size = sizeof(SonareSynthPatch) - 1;
  REQUIRE_FALSE(synth_patch_converts(undersized, &read, &error));
  REQUIRE(std::string(error) == "patch.struct_size must be set by sonare_synth_patch_init");

  // The read direction reports the resolved mode rather than the keep-base zero.
  SonareSynthPatch preset{};
  REQUIRE(sonare_synth_preset_patch("sawLead", &preset) == SONARE_OK);
  REQUIRE(preset.retrigger == SONARE_SYNTH_RETRIGGER_FREE);
}

TEST_CASE("the patch LFO shape is read from a full-size struct",
          "[project][synth_patch][lfo_shape]") {
  using sonare::midi::synth::LfoShape;
  SonareSynthPatch asked{};
  sonare_synth_patch_init(&asked);
  std::strcpy(asked.preset, "sawLead");
  asked.lfo_shape = SONARE_SYNTH_LFO_SHAPE_SINE;
  sonare::midi::synth::NativeSynthConfig read;
  std::string error;
  REQUIRE(synth_patch_converts(asked, &read, &error));
  REQUIRE(read.patch.lfo_shape == LfoShape::kSine);
  SonareSynthPatch random = asked;
  random.lfo_shape = SONARE_SYNTH_LFO_SHAPE_RANDOM;
  REQUIRE(synth_patch_converts(random, &read, &error));
  REQUIRE(read.patch.lfo_shape == LfoShape::kRandom);
  SonareSynthPatch written{};
  sonare_c_detail::synth_patch_to_c(sonare::midi::synth::SynthPreset{"sawLead", read}, &written);
  REQUIRE(written.lfo_shape == SONARE_SYNTH_LFO_SHAPE_RANDOM);

  // Zero keeps the base, which for every catalog preset is the triangle.
  SonareSynthPatch base = asked;
  base.lfo_shape = SONARE_SYNTH_LFO_SHAPE_BASE;
  REQUIRE(synth_patch_converts(base, &read, &error));
  REQUIRE(read.patch.lfo_shape == LfoShape::kTriangle);

  // The read direction reports the resolved shape rather than the keep-base zero.
  SonareSynthPatch preset{};
  REQUIRE(sonare_synth_preset_patch("sawLead", &preset) == SONARE_OK);
  REQUIRE(preset.lfo_shape == SONARE_SYNTH_LFO_SHAPE_TRIANGLE);
}

TEST_CASE("the second presence word's fields are read from a full-size struct",
          "[project][synth_patch]") {
  static_assert(SONARE_SYNTH_FIELD2_ONE_SHOT == 1u << 5, "second presence word bits moved");
  SonareSynthPatch asked{};
  sonare_synth_patch_init(&asked);
  std::strcpy(asked.preset, "sawLead");
  asked.drift_rate_hz = 4.5f;
  asked.amp_delay_ms = 30.0f;
  asked.amp_hold_ms = 40.0f;
  asked.filter_delay_ms = 50.0f;
  asked.filter_hold_ms = 60.0f;
  asked.one_shot = 1;
  sonare::midi::synth::NativeSynthConfig read;
  std::string error;
  REQUIRE(synth_patch_converts(asked, &read, &error));
  REQUIRE(error.empty());
  REQUIRE(read.patch.drift_rate_hz == 4.5f);
  REQUIRE(read.patch.amp_env.delay_ms == 30.0f);
  REQUIRE(read.patch.amp_env.hold_ms == 40.0f);
  REQUIRE(read.patch.filter_env.delay_ms == 50.0f);
  REQUIRE(read.patch.filter_env.hold_ms == 60.0f);
  REQUIRE(read.patch.one_shot);

  // Zero keeps the base unless the second word says it was meant.
  SonareSynthPatch zero{};
  sonare_synth_patch_init(&zero);
  std::strcpy(zero.preset, "sawLead");
  const sonare::midi::synth::SynthPreset* saw = sonare::midi::synth::find_synth_preset("sawLead");
  REQUIRE(saw != nullptr);
  REQUIRE(saw->config.patch.drift_rate_hz != 0.0f);
  REQUIRE(synth_patch_converts(zero, &read, &error));
  REQUIRE(read.patch.drift_rate_hz == saw->config.patch.drift_rate_hz);
  // The rate's accepted range starts above zero, so an explicit zero is refused by name.
  zero.present_fields2 = SONARE_SYNTH_FIELD2_DRIFT_RATE_HZ;
  REQUIRE_FALSE(synth_patch_converts(zero, &read, &error));
  REQUIRE(error.find("patch.drift_rate_hz ") == 0);
  zero.present_fields2 = 0;

  // The read direction reports every field with its presence bit set.
  SonareSynthPatch preset{};
  REQUIRE(sonare_synth_preset_patch("sawLead", &preset) == SONARE_OK);
  REQUIRE(preset.present_fields2 == sonare_c_detail::kSynthPatchAllFields2);
  REQUIRE(preset.drift_rate_hz == saw->config.patch.drift_rate_hz);
  REQUIRE(preset.one_shot == 0);
  REQUIRE(synth_patch_converts(preset, &read, &error));
  REQUIRE(read.patch.drift_rate_hz == saw->config.patch.drift_rate_hz);
}

namespace {

/// One note held for 0.1 s, so a release lands inside the census render.
SonareProject* make_census_project(uint8_t note) {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000) == SONARE_OK);
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);
  SonareMidiEventPod events[2]{};
  events[0].ppq = 0.0;
  events[0].word0 = 0x20900064u | (static_cast<uint32_t>(note) << 8);  // note-on, vel 100
  events[0].word1 = 0u;
  events[1].ppq = 0.2;
  events[1].word0 = 0x20800000u | (static_cast<uint32_t>(note) << 8);  // note-off
  events[1].word1 = 0u;
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, 3) == SONARE_OK);
  return project;
}

std::vector<float> census_render(const SonareSynthPatch& patch, SonareSampleBank* bank = nullptr) {
  SonareProject* project = make_census_project(60);
  SonareProjectBounceOptions options;
  sonare_project_bounce_options_init(&options);
  options.total_frames = 9600;
  options.block_size = 128;
  options.num_channels = 2;

  SonareSynthInstrumentBinding binding{};
  binding.destination_id = 3;
  binding.patch = patch;
  binding.sample_bank = bank;
  float* out = nullptr;
  size_t out_len = 0;
  int bounce_channels10 = 0;
  REQUIRE(bounce_with_synth_instruments(project, &options, &binding, 1, &out, &out_len,
                                        &bounce_channels10) == SONARE_OK);
  REQUIRE(out != nullptr);
  std::vector<float> result(out, out + out_len);
  sonare_free_floats(out);
  sonare_project_destroy(project);
  return result;
}

struct CensusEdit {
  const char* field;
  void (*apply)(SonareSynthPatch&);
};

}  // namespace

TEST_CASE("each wrapper field group sounds on the engines the header says honour it",
          "[project][synth_patch]") {
  // Each base carries the settings that make every edit below audible where it is
  // honoured: a drift depth for its rate, a filter envelope for its delay and hold,
  // an amplitude decay for its hold, an LFO rate for its depth.
  const auto base = [](int engine_mode, const char* preset) {
    SonareSynthPatch p{};
    sonare_synth_patch_init(&p);
    std::strncpy(p.preset, preset, SONARE_SYNTH_PRESET_NAME_MAX - 1);
    p.engine_mode = engine_mode;
    p.unison = 3;
    p.drift_cents = 40.0f;
    p.cutoff_hz = 1500.0f;
    p.env_to_cutoff_cents = 2400.0f;
    p.filter_decay_ms = 200.0f;
    p.filter_sustain = 0.3f;
    p.amp_decay_ms = 200.0f;
    p.amp_sustain = 0.5f;
    p.lfo_rate_hz = 5.0f;
    return p;
  };

  // Each base renders once; every edit is compared against that render.
  struct Base {
    SonareSynthPatch patch;
    SonareSampleBank* bank;
    std::vector<float> render;
  };
  const auto rendered = [](const SonareSynthPatch& patch, SonareSampleBank* with_bank) {
    Base b{patch, with_bank, census_render(patch, with_bank)};
    REQUIRE(peak_of(b.render) > 0.0f);
    return b;
  };
  const auto edited_render = [](const Base& on, const CensusEdit& edit) {
    SonareSynthPatch edited = on.patch;
    edit.apply(edited);
    return census_render(edited, on.bank);
  };
  const auto require_heard = [&](const Base& on, const CensusEdit& edit) {
    INFO(edit.field << " heard on engine " << on.patch.engine_mode);
    REQUIRE(edited_render(on, edit) != on.render);
  };
  const auto require_ignored = [&](const Base& on, const CensusEdit& edit) {
    INFO(edit.field << " ignored on engine " << on.patch.engine_mode << " preset '"
                    << on.patch.preset << "'");
    REQUIRE(edited_render(on, edit) == on.render);
  };

  std::vector<float> pcm(24000);
  for (size_t i = 0; i < pcm.size(); ++i) {
    pcm[i] = 0.5f *
             std::sin(2.0f * sonare::constants::kPi * 261.6256f * static_cast<float>(i) / 48000.0f);
  }
  SonareSampleBank* bank = nullptr;
  REQUIRE(sonare_sample_bank_create(&bank) == SONARE_OK);
  REQUIRE(bank != nullptr);
  SonareSampleDesc desc{};
  REQUIRE(sonare_sample_bank_add_sample(bank, pcm.data(), pcm.size(), &desc, nullptr) == SONARE_OK);
  SonareSampleZoneDesc zone{};
  REQUIRE(sonare_sample_bank_add_zone(bank, 0, &zone) == SONARE_OK);

  const Base subtractive = rendered(base(SONARE_SYNTH_ENGINE_SUBTRACTIVE, ""), nullptr);
  const Base reed = rendered(base(SONARE_SYNTH_ENGINE_REED, ""), nullptr);
  const Base fm = rendered(base(SONARE_SYNTH_ENGINE_FM, ""), nullptr);
  const Base sample = rendered(base(SONARE_SYNTH_ENGINE_SAMPLE, ""), bank);
  // A percussion channel: each note runs the per-note kit patch instead of this one.
  const Base kit = rendered(base(0, "drumKit"), nullptr);

  // Subtractive only: the oscillator stack.
  const CensusEdit kOscillator[] = {
      {"waveform", [](SonareSynthPatch& p) { p.waveform = SONARE_SYNTH_OSC_SQUARE; }},
      {"unison", [](SonareSynthPatch& p) { p.unison = 6; }},
      {"detune_cents", [](SonareSynthPatch& p) { p.detune_cents = 35.0f; }},
  };
  for (const CensusEdit& edit : kOscillator) {
    require_heard(subtractive, edit);
    require_ignored(reed, edit);
    require_ignored(fm, edit);
  }

  // Sample only: the sample block.
  const CensusEdit kSampleBlock[] = {
      {"sample_level", [](SonareSynthPatch& p) { p.sample_level = 0.3f; }},
      {"sample_start_offset", [](SonareSynthPatch& p) { p.sample_start_offset = 0.25f; }},
  };
  for (const CensusEdit& edit : kSampleBlock) {
    require_heard(sample, edit);
    require_ignored(reed, edit);
  }

  // Every engine, and none of them on a percussion channel.
  const CensusEdit kEveryEngine[] = {
      {"pitch_offset_cents", [](SonareSynthPatch& p) { p.pitch_offset_cents = 300.0f; }},
      {"lfo_to_pitch_cents", [](SonareSynthPatch& p) { p.lfo_to_pitch_cents = 60.0f; }},
      {"drift_cents", [](SonareSynthPatch& p) { p.drift_cents = 90.0f; }},
      {"drift_rate_hz",
       [](SonareSynthPatch& p) {
         p.drift_rate_hz = 7.0f;
         p.present_fields2 |= SONARE_SYNTH_FIELD2_DRIFT_RATE_HZ;
       }},
      {"cutoff_hz", [](SonareSynthPatch& p) { p.cutoff_hz = 500.0f; }},
      {"filter_attack_ms", [](SonareSynthPatch& p) { p.filter_attack_ms = 150.0f; }},
      {"filter_delay_ms",
       [](SonareSynthPatch& p) {
         p.filter_delay_ms = 80.0f;
         p.present_fields2 |= SONARE_SYNTH_FIELD2_FILTER_DELAY_MS;
       }},
      {"filter_hold_ms",
       [](SonareSynthPatch& p) {
         p.filter_hold_ms = 80.0f;
         p.present_fields2 |= SONARE_SYNTH_FIELD2_FILTER_HOLD_MS;
       }},
      {"amp_attack_ms", [](SonareSynthPatch& p) { p.amp_attack_ms = 150.0f; }},
      {"amp_delay_ms",
       [](SonareSynthPatch& p) {
         p.amp_delay_ms = 80.0f;
         p.present_fields2 |= SONARE_SYNTH_FIELD2_AMP_DELAY_MS;
       }},
      {"amp_hold_ms",
       [](SonareSynthPatch& p) {
         p.amp_hold_ms = 80.0f;
         p.present_fields2 |= SONARE_SYNTH_FIELD2_AMP_HOLD_MS;
       }},
      {"one_shot",
       [](SonareSynthPatch& p) {
         p.one_shot = 1;
         p.present_fields2 |= SONARE_SYNTH_FIELD2_ONE_SHOT;
       }},
      {"body",
       [](SonareSynthPatch& p) {
         p.body = SONARE_SYNTH_BODY_GUITAR;
         p.body_mix = 0.7f;
       }},
      {"drive", [](SonareSynthPatch& p) { p.drive = 0.9f; }},
      {"hp_cutoff_hz", [](SonareSynthPatch& p) { p.hp_cutoff_hz = 1500.0f; }},
      {"sample_hold_hz", [](SonareSynthPatch& p) { p.sample_hold_hz = 3000.0f; }},
      {"bit_depth", [](SonareSynthPatch& p) { p.bit_depth = 3.0f; }},
      {"stereo_spread", [](SonareSynthPatch& p) { p.stereo_spread = 1.0f; }},
  };
  for (const CensusEdit& edit : kEveryEngine) {
    require_heard(reed, edit);
    require_ignored(kit, edit);
  }

  // A percussion channel keeps the instrument's own level and bus.
  const CensusEdit kInstrument[] = {
      {"gain", [](SonareSynthPatch& p) { p.gain = 0.2f; }},
      {"bus_drive", [](SonareSynthPatch& p) { p.bus_drive = 0.9f; }},
  };
  for (const CensusEdit& edit : kInstrument) {
    require_heard(kit, edit);
    require_heard(reed, edit);
  }

  sonare_sample_bank_destroy(bank);
}

TEST_CASE("the patch pitch offset reaches both of the engine pitch sources",
          "[project][synth_patch]") {
  SonareProject* project = make_synth_project(3);

  // The two engine classes take their pitch from different places — the
  // subtractive oscillator from its base frequency, the physical models from
  // the per-sample pitch factor — and the offset is applied at each. One preset
  // per side, because a field wired into only one of them moves only one.
  for (const char* preset : {"clarinet", "sawLead"}) {
    SonareSynthPatch base{};
    sonare_synth_patch_init(&base);
    std::strcpy(base.preset, preset);
    const std::vector<float> reference = bounce_synth(project, base);
    REQUIRE(peak_of(reference) > 0.0f);

    SonareSynthPatch transposed = base;
    transposed.pitch_offset_cents = 700.0f;
    REQUIRE(bounce_synth(project, transposed) != reference);
  }

  sonare_project_destroy(project);
}

TEST_CASE("synth patch mod routing ordinals outside the enum are refused at the C ABI boundary",
          "[project][synth_patch]") {
  const SonareSynthModRouting bad[] = {{-1, 1 /*pitchCents*/, 100.0f},
                                       {99, 2 /*cutoffCents*/, 100.0f},
                                       {3 /*lfo1*/, -1, 100.0f},
                                       {4 /*lfo2*/, 99, 100.0f}};
  for (const SonareSynthModRouting& routing : bad) {
    CAPTURE(routing.source, routing.destination);
    SonareSynthPatch patch{};
    sonare_synth_patch_init(&patch);
    patch.num_mod_routings = 1;
    patch.mod_routings[0] = routing;
    sonare::midi::synth::NativeSynthConfig cfg;
    std::string error;
    REQUIRE_FALSE(synth_patch_converts(patch, &cfg, &error));
    REQUIRE(!error.empty());
  }
}

TEST_CASE("the drum-kit preset plays the GM drum map on any key", "[project][synth_patch]") {
  // Note 38 = acoustic snare in the GM map.
  SonareProject* project = make_synth_project(3, 38);
  SonareSynthPatch kit{};
  REQUIRE(sonare_synth_preset_patch("drumKit", &kit) == SONARE_OK);
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
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);

  SonareSynthPatch patch{};
  REQUIRE(sonare_synth_preset_patch("sawLead", &patch) == SONARE_OK);
  REQUIRE(set_synth_patch(engine, 7, &patch) == SONARE_OK);
  size_t count = 0;
  REQUIRE(sonare_engine_midi_instrument_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 1);

  REQUIRE(sonare_engine_push_midi_note_on(engine, 7, 0, 0, 60, 100, -1) == SONARE_OK);
  std::vector<float> left(128, 0.0f);
  std::vector<float> right(128, 0.0f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, nullptr, 2, 128) == SONARE_OK);
  float peak = 0.0f;
  for (float s : left) peak = std::max(peak, std::abs(s));
  for (float s : right) peak = std::max(peak, std::abs(s));
  REQUIRE(peak > 0.0f);

  // Invalid patches are rejected without disturbing the binding.
  SonareSynthPatch bad{};
  sonare_synth_patch_init(&bad);
  bad.struct_size = sizeof(SonareSynthPatch) - 1;
  REQUIRE(set_synth_patch(engine, 7, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_synth_instrument(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_midi_instrument_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 1);

  REQUIRE(sonare_engine_clear_midi_instrument(engine, 7) == SONARE_OK);
  sonare_engine_destroy(engine);
}

TEST_CASE("every mod source ordinal survives the C conversion", "[project][synth_patch]") {
  // A conversion whose upper bound lags the enum would lose the newest source;
  // walking the whole domain catches that, and the ends of it must be refused.
  const auto convert = [](int source, sonare::midi::synth::NativeSynthConfig* cfg) {
    SonareSynthPatch patch{};
    sonare_synth_patch_init(&patch);
    patch.num_mod_routings = 1;
    patch.mod_routings[0] = {source, 1, 1.0f};
    std::string error;
    const bool ok = synth_patch_converts(patch, cfg, &error);
    REQUIRE(ok == error.empty());
    return ok;
  };
  for (int ordinal = 1; ordinal < SONARE_SYNTH_MOD_SOURCE_COUNT; ++ordinal) {
    CAPTURE(ordinal);
    sonare::midi::synth::NativeSynthConfig cfg;
    REQUIRE(convert(ordinal, &cfg));
    REQUIRE(static_cast<int>(cfg.patch.mod_matrix.routes[0].source) == ordinal);
  }
  sonare::midi::synth::NativeSynthConfig cfg;
  REQUIRE_FALSE(convert(SONARE_SYNTH_MOD_SOURCE_COUNT, &cfg));
  REQUIRE_FALSE(convert(-1, &cfg));
}

TEST_CASE("every mod destination ordinal survives the C conversion", "[project][synth_patch]") {
  const auto convert = [](int destination, sonare::midi::synth::NativeSynthConfig* cfg) {
    SonareSynthPatch patch{};
    sonare_synth_patch_init(&patch);
    patch.num_mod_routings = 1;
    patch.mod_routings[0] = {1, destination, 1.0f};
    std::string error;
    const bool ok = synth_patch_converts(patch, cfg, &error);
    REQUIRE(ok == error.empty());
    return ok;
  };
  for (int ordinal = 1; ordinal < SONARE_SYNTH_MOD_DESTINATION_COUNT; ++ordinal) {
    CAPTURE(ordinal);
    sonare::midi::synth::NativeSynthConfig cfg;
    REQUIRE(convert(ordinal, &cfg));
    REQUIRE(static_cast<int>(cfg.patch.mod_matrix.routes[0].destination) == ordinal);
  }
  sonare::midi::synth::NativeSynthConfig cfg;
  REQUIRE_FALSE(convert(SONARE_SYNTH_MOD_DESTINATION_COUNT, &cfg));
  REQUIRE_FALSE(convert(-1, &cfg));
}

#endif  // SONARE_WITH_ARRANGEMENT
