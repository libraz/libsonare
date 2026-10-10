/// @file synth_catalog.cpp
/// @brief The synth-catalogue C ABI: which presets, enum values and built-in
///        waveforms exist, the patch behind a preset name, and what each
///        engine-section and patch field accepts.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/synth_patch_common.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/patch_tuning.h"
#include "midi/synth/synth_presets.h"
#include "util/number_format.h"

namespace {

/// Entries in one of the newline-separated tables below.
constexpr int name_count(const char* names) {
  int count = *names == '\0' ? 0 : 1;
  for (const char* p = names; *p != '\0'; ++p) {
    if (*p == '\n') ++count;
  }
  return count;
}

}  // namespace
#endif

const char* sonare_synth_preset_names(void) {
#if defined(SONARE_WITH_ARRANGEMENT)
  static const std::string kNames = [] {
    std::string names;
    for (size_t i = 0; i < sonare::midi::synth::synth_preset_count(); ++i) {
      if (!names.empty()) names += '\n';
      names += sonare::midi::synth::synth_preset_at(i)->name;
    }
    return names;
  }();
  return kNames.c_str();
#else
  return "";
#endif
}

const char* sonare_synth_enum_names(int kind) {
#if defined(SONARE_WITH_ARRANGEMENT)
  // These tables are the only place a surface learns an enum value's name, and
  // nothing compares them against the enums for you: a value added without a
  // name here reaches a binding as a short list rather than as a build failure.
  // Hence the counts below, one per table that has a count macro to check
  // against.
  constexpr const char* kEngineModes =
      "default\nsubtractive\nfm\nkarplus-strong\nmodal\nadditive\npercussion\npiano\npipe-organ\n"
      "bowed-string\nreed\nbrass\nflute\nplucked-string\nvocal\nfree-reed\nharpsichord\n"
      "sample";
  constexpr const char* kWaveforms = "default\nsine\nsaw\nsquare\ntriangle\nnoise";
  constexpr const char* kFilterModels = "default\nsvf\nmoog-ladder\ndiode-ladder\nsallen-key";
  constexpr const char* kFilterOutputs = "default\nlowpass\nbandpass\nhighpass";
  constexpr const char* kBodyTypes = "default\nnone\nguitar\nviolin\nwood-tube\nbrass-bell\nvocal";
  constexpr const char* kModSources =
      "none\namp-env\nfilter-env\nlfo1\nlfo2\nvelocity\nkey-track\nmod-wheel\nrandom\n"
      "breath\naftertouch\nexpression-cc\npitch-bend";
  constexpr const char* kModDestinations =
      "none\npitch-cents\ncutoff-cents\namp-gain\npan-units\nresonance-q\n"
      "vibrato-depth-cents\nfilter-env-depth\nlfo1-rate-scale\n"
      "excitation-force\nexcitation-position\nexcitation-brightness\nspectrum-morph";
  // No count macro applies: this table names the BuiltinSynth waveforms and
  // carries "sawtooth" as a second spelling of "saw", so its entry count is
  // deliberately one more than the enum's.
  constexpr const char* kBuiltinWaveforms = "sine\nsaw\nsawtooth\nsquare\ntriangle";
  constexpr const char* kControllerInputs =
      "control-change\nchannel-pressure\npoly-pressure\npitch-bend\nvelocity";
  constexpr const char* kControllerAxes =
      "none\nexcitation\nposition\nbrightness\nmorph\nloudness\npitch-cents\nvibrato-depth";
  constexpr const char* kArticulations = "poly\nmono-retrigger\nmono-legato";
  constexpr const char* kMpeDimensions = "bend\npressure\ntimbre";
  constexpr const char* kNoteTrackings = "last\nlowest\nhighest\nall";

  static_assert(name_count(kEngineModes) == SONARE_SYNTH_ENGINE_MODE_COUNT,
                "engine mode names out of step with the enum");
  static_assert(name_count(kWaveforms) == SONARE_SYNTH_OSC_WAVEFORM_COUNT,
                "oscillator waveform names out of step with the enum");
  static_assert(name_count(kFilterModels) == SONARE_SYNTH_FILTER_MODEL_COUNT,
                "filter model names out of step with the enum");
  static_assert(name_count(kFilterOutputs) == SONARE_SYNTH_FILTER_OUTPUT_COUNT,
                "filter output names out of step with the enum");
  static_assert(name_count(kBodyTypes) == SONARE_SYNTH_BODY_TYPE_COUNT,
                "body type names out of step with the enum");
  static_assert(name_count(kModSources) == SONARE_SYNTH_MOD_SOURCE_COUNT,
                "mod source names out of step with the enum");
  static_assert(name_count(kModDestinations) == SONARE_SYNTH_MOD_DESTINATION_COUNT,
                "mod destination names out of step with the enum");
  static_assert(name_count(kControllerInputs) == SONARE_CONTROLLER_INPUT_COUNT,
                "controller input names out of step with the enum");
  static_assert(name_count(kControllerAxes) == SONARE_CONTROLLER_AXIS_COUNT,
                "controller axis names out of step with the enum");
  static_assert(name_count(kArticulations) == SONARE_ARTICULATION_COUNT,
                "articulation names out of step with the enum");
  static_assert(name_count(kMpeDimensions) == SONARE_MPE_DIMENSION_COUNT,
                "MPE dimension names out of step with the enum");
  static_assert(name_count(kNoteTrackings) == SONARE_NOTE_TRACKING_COUNT,
                "note tracking names out of step with the enum");

  switch (kind) {
    case SONARE_SYNTH_ENUM_ENGINE_MODE:
      return kEngineModes;
    case SONARE_SYNTH_ENUM_OSC_WAVEFORM:
      return kWaveforms;
    case SONARE_SYNTH_ENUM_FILTER_MODEL:
      return kFilterModels;
    case SONARE_SYNTH_ENUM_FILTER_OUTPUT:
      return kFilterOutputs;
    case SONARE_SYNTH_ENUM_BODY_TYPE:
      return kBodyTypes;
    case SONARE_SYNTH_ENUM_MOD_SOURCE:
      return kModSources;
    case SONARE_SYNTH_ENUM_MOD_DESTINATION:
      return kModDestinations;
    case SONARE_SYNTH_ENUM_CONTROLLER_INPUT:
      return kControllerInputs;
    case SONARE_SYNTH_ENUM_CONTROLLER_AXIS:
      return kControllerAxes;
    case SONARE_SYNTH_ENUM_ARTICULATION:
      return kArticulations;
    case SONARE_SYNTH_ENUM_MPE_DIMENSION:
      return kMpeDimensions;
    case SONARE_SYNTH_ENUM_NOTE_TRACKING:
      return kNoteTrackings;
    case SONARE_SYNTH_ENUM_BUILTIN_WAVEFORM:
      return kBuiltinWaveforms;
    default:
      return "";
  }
#else
  (void)kind;
  return "";
#endif
}

int sonare_synth_builtin_waveform_from_name(const char* name) {
  if (name == nullptr) return -1;
  if (std::strcmp(name, "sine") == 0) return SONARE_SYNTH_WAVEFORM_SINE;
  if (std::strcmp(name, "saw") == 0 || std::strcmp(name, "sawtooth") == 0) {
    return SONARE_SYNTH_WAVEFORM_SAW;
  }
  if (std::strcmp(name, "square") == 0) return SONARE_SYNTH_WAVEFORM_SQUARE;
  if (std::strcmp(name, "triangle") == 0) return SONARE_SYNTH_WAVEFORM_TRIANGLE;
  return -1;
}

SonareError sonare_synth_preset_patch(const char* name, SonareSynthPatch* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (!name || !out) return SONARE_ERROR_INVALID_PARAMETER;
  const sonare::midi::synth::SynthPreset* preset = sonare::midi::synth::find_synth_preset(name);
  if (preset == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "unknown synth preset name");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  sonare_c_detail::synth_patch_to_c(*preset, out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(name, out);
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

using sonare::mastering::api::detail::Unit;
using sonare::mastering::api::detail::unit_name;

/// Shortest text that reads back as the same float; a value with an integer
/// part keeps every integer digit rather than switching to exponent form.
std::string descriptor_number(float value) {
  const double magnitude = std::fabs(static_cast<double>(value));
  const int integer_digits =
      magnitude >= 1.0 ? static_cast<int>(std::floor(std::log10(magnitude))) + 1 : 1;
  std::string text;
  for (int precision = 1; precision <= std::numeric_limits<float>::max_digits10; ++precision) {
    text = sonare::util::format_general(value, std::max(precision, integer_digits));
    double read = 0.0;
    if (sonare::util::parse_double(text.data(), text.data() + text.size(), &read) &&
        static_cast<float>(read) == value) {
      break;
    }
  }
  return text;
}

/// One descriptor in the insert descriptor field names: `min` / `max` only
/// where the field is bounded, `integer` only where it is set.
struct DescriptorRow {
  std::string name;
  bool integer;
  bool boolean;
  bool bounded;
  float lo, hi;
  float value;
  Unit unit;
};

void append_descriptor(const DescriptorRow& row, std::string* out) {
  *out += "{\"name\":\"";
  *out += row.name;
  *out += "\",\"type\":\"";
  *out += row.boolean ? "boolean" : "number";
  *out += '"';
  if (row.integer) *out += ",\"integer\":true";
  if (row.bounded && !row.boolean) {
    *out += ",\"min\":";
    *out += descriptor_number(row.lo);
    *out += ",\"max\":";
    *out += descriptor_number(row.hi);
  }
  *out += ",\"default\":";
  if (row.boolean) {
    *out += row.value != 0.0f ? "true" : "false";
  } else {
    *out += descriptor_number(row.value);
  }
  *out += ",\"unit\":\"";
  *out += unit_name(row.unit);
  *out += "\"}";
}

std::string descriptor_array(const std::vector<DescriptorRow>& rows) {
  std::string json = "[";
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i > 0) json += ',';
    append_descriptor(rows[i], &json);
  }
  json += ']';
  return json;
}

/// The SonareSynthPatch numeric fields the instrument automation table names,
/// with the unit each carries in the param_meta vocabulary.
struct AutomatedWrapperField {
  sonare::midi::synth::NativeSynthParamId id;
  Unit unit;
};

constexpr AutomatedWrapperField kAutomatedWrapperFields[] = {
    {sonare::midi::synth::NativeSynthParamId::kGain, Unit::Ratio},
    {sonare::midi::synth::NativeSynthParamId::kBusDrive, Unit::None},
    {sonare::midi::synth::NativeSynthParamId::kCutoffHz, Unit::Hz},
    {sonare::midi::synth::NativeSynthParamId::kResonanceQ, Unit::Ratio},
    {sonare::midi::synth::NativeSynthParamId::kDrive, Unit::None},
    {sonare::midi::synth::NativeSynthParamId::kKeyTrack, Unit::None},
    {sonare::midi::synth::NativeSynthParamId::kEnvToCutoffCents, Unit::Cents},
    {sonare::midi::synth::NativeSynthParamId::kVelToCutoffCents, Unit::Cents},
    {sonare::midi::synth::NativeSynthParamId::kAmpAttackMs, Unit::Ms},
    {sonare::midi::synth::NativeSynthParamId::kAmpDecayMs, Unit::Ms},
    {sonare::midi::synth::NativeSynthParamId::kAmpSustain, Unit::None},
    {sonare::midi::synth::NativeSynthParamId::kAmpReleaseMs, Unit::Ms},
    {sonare::midi::synth::NativeSynthParamId::kFilterAttackMs, Unit::Ms},
    {sonare::midi::synth::NativeSynthParamId::kFilterDecayMs, Unit::Ms},
    {sonare::midi::synth::NativeSynthParamId::kFilterSustain, Unit::None},
    {sonare::midi::synth::NativeSynthParamId::kFilterReleaseMs, Unit::Ms},
    {sonare::midi::synth::NativeSynthParamId::kLfoRateHz, Unit::Hz},
    {sonare::midi::synth::NativeSynthParamId::kLfoToPitchCents, Unit::Cents},
    {sonare::midi::synth::NativeSynthParamId::kLfo2RateHz, Unit::Hz},
    {sonare::midi::synth::NativeSynthParamId::kGlideMs, Unit::Ms},
    {sonare::midi::synth::NativeSynthParamId::kBodyMix, Unit::None},
    {sonare::midi::synth::NativeSynthParamId::kStereoSpread, Unit::None},
    {sonare::midi::synth::NativeSynthParamId::kDetuneCents, Unit::Cents},
    {sonare::midi::synth::NativeSynthParamId::kDriftCents, Unit::Cents},
    {sonare::midi::synth::NativeSynthParamId::kPitchOffsetCents, Unit::Cents},
    {sonare::midi::synth::NativeSynthParamId::kHpCutoffHz, Unit::Hz},
    {sonare::midi::synth::NativeSynthParamId::kSampleHoldHz, Unit::Hz},
    {sonare::midi::synth::NativeSynthParamId::kBitDepth, Unit::Bits},
};

/// Every numeric SonareSynthPatch field: the automated ones with the automation
/// table's name and range, the rest under their facade names with the range the
/// conversion's clamps apply. Defaults are the init patch's.
std::string synth_patch_param_info_json() {
  using sonare::midi::synth::NativeSynth;
  using sonare::midi::synth::NativeSynthConfig;
  using sonare::midi::synth::NativeSynthPatch;
  std::vector<DescriptorRow> rows;
  const NativeSynth describer;
  for (const AutomatedWrapperField& field : kAutomatedWrapperFields) {
    sonare::automation::ParameterDescription description;
    if (!describer.describe_parameter(static_cast<unsigned int>(field.id), &description)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "synth automation table lost a SonareSynthPatch field");
    }
    rows.push_back({description.name, false, false, true, description.min_value,
                    description.max_value, description.default_value, field.unit});
  }
  const NativeSynthConfig init{};
  // The sample block's ranges are the patch clamp's, read back from its extremes.
  NativeSynthPatch low = init.patch;
  low.sample.level = -std::numeric_limits<float>::max();
  low.sample.start_offset01 = -std::numeric_limits<float>::max();
  low = sonare::midi::synth::clamp_synth_patch(low);
  NativeSynthPatch high = init.patch;
  high.sample.level = std::numeric_limits<float>::max();
  high.sample.start_offset01 = std::numeric_limits<float>::max();
  high = sonare::midi::synth::clamp_synth_patch(high);
  rows.push_back({"unison", true, false, true, 1.0f,
                  static_cast<float>(sonare::midi::synth::kMaxUnisonOscs),
                  static_cast<float>(init.patch.unison), Unit::Count});
  rows.push_back({"polyphony", true, false, true, 1.0f,
                  static_cast<float>(sonare::midi::kMaxSynthVoices),
                  static_cast<float>(init.polyphony), Unit::Count});
  // Negative selects no keymap; the upper end is whatever the bound bank holds.
  rows.push_back({"sampleSet", true, false, false, 0.0f, 0.0f,
                  static_cast<float>(init.patch.sample.set_index), Unit::None});
  rows.push_back({"sampleLevel", false, false, true, low.sample.level, high.sample.level,
                  init.patch.sample.level, Unit::Ratio});
  rows.push_back({"sampleStartOffset", false, false, true, low.sample.start_offset01,
                  high.sample.start_offset01, init.patch.sample.start_offset01, Unit::None});
  return descriptor_array(rows);
}

}  // namespace
#endif

const char* sonare_synth_engine_param_info(int engine_mode) {
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  // Argument-dependent: recomputed into a thread-local on every call.
  static thread_local std::string info;
  if (!sonare_c_detail::valid_c_enum(engine_mode, SONARE_SYNTH_ENGINE_MODE_COUNT)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("engine_mode " + std::to_string(engine_mode) + " is out of range").c_str());
    return nullptr;
  }
  info = "[]";
  if (engine_mode == SONARE_SYNTH_ENGINE_DEFAULT) return info.c_str();
  const auto mode = static_cast<sonare::midi::synth::SynthEngineMode>(engine_mode - 1);
  const char* base_name = sonare::midi::synth::base_preset_name(mode);
  if (base_name == nullptr) return info.c_str();
  const sonare::midi::synth::SynthPreset* base = sonare::midi::synth::find_synth_preset(base_name);
  if (base == nullptr) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "synth engine base preset is missing from the catalog");
  }
  std::vector<DescriptorRow> rows;
  for (const sonare::midi::synth::EngineParamDescriptor& d :
       sonare::midi::synth::engine_param_descriptors(base->config.patch)) {
    rows.push_back({d.key, d.integer, d.boolean, d.bounded, d.lo, d.hi, d.value, d.unit});
  }
  info = descriptor_array(rows);
  return info.c_str();
  SONARE_C_CATCH_RETURN(nullptr)
#else
  (void)engine_mode;
  sonare_c_detail::set_last_error(SONARE_ERROR_NOT_SUPPORTED, "built without arrangement support");
  return nullptr;
#endif
}

const char* sonare_synth_patch_param_info(void) {
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  static const std::string kInfo = synth_patch_param_info_json();
  return kInfo.c_str();
  SONARE_C_CATCH_RETURN(nullptr)
#else
  sonare_c_detail::set_last_error(SONARE_ERROR_NOT_SUPPORTED, "built without arrangement support");
  return nullptr;
#endif
}

const char* sonare_synth_gs_drum_kit_name(int program) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (program < 0 || program > 127) return nullptr;
  // The core hands back a view into a static literal, so the NUL the C surface
  // promises is already there and the empty view is the "no set here" answer.
  const std::string_view name =
      sonare::midi::synth::gs_drum_kit_name(static_cast<uint8_t>(program));
  return name.empty() ? nullptr : name.data();
#else
  (void)program;
  return nullptr;
#endif
}

int sonare_synth_gs_drum_kit_is_voiced_apart(int program) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (program < 0 || program > 127) return -1;
  const auto p = static_cast<uint8_t>(program);
  if (sonare::midi::synth::gs_drum_kit_name(p).empty()) return -1;
  return sonare::midi::synth::gs_drum_kit_is_voiced_apart(
             sonare::midi::synth::gm_fallback_drum_kit(p))
             ? 1
             : 0;
#else
  (void)program;
  return -1;
#endif
}

int sonare_synth_gs_variation_is_voiced_apart(int bank, int program) {
#if defined(SONARE_WITH_ARRANGEMENT)
  // 127, not the 0xFFFF a uint16_t holds: a Bank Select value is seven bits, and
  // bounding by the storage type instead of by the domain let 128 and above
  // through to a resolution that has no variation to find and answers 0 -- the
  // one wrong answer a caller cannot separate from a real capital-tone result.
  if (bank < 0 || bank > 127 || program < 0 || program > 127) return -1;
  return sonare::midi::synth::gs_variation_is_voiced_apart(static_cast<uint16_t>(bank),
                                                           static_cast<uint8_t>(program))
             ? 1
             : 0;
#else
  (void)bank;
  (void)program;
  return -1;
#endif
}
