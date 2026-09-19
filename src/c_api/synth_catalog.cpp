/// @file synth_catalog.cpp
/// @brief The synth-catalogue C ABI: which presets, enum values and built-in
///        waveforms exist, and the patch behind a preset name.

#include <cstring>
#include <string>

#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/synth_patch_common.h"
#include "midi/synth/synth_presets.h"

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
    set_last_error("unknown synth preset name");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  sonare_c_detail::synth_patch_to_c(*preset, out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(name, out);
#endif
}
