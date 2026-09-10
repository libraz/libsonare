/// @file synth_catalog.cpp
/// @brief The synth-catalogue C ABI: which presets, enum values and built-in
///        waveforms exist, and the patch behind a preset name.

#include <cstring>
#include <string>

#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/synth_patch_common.h"
#include "midi/synth/synth_presets.h"
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
  static const std::string kEngineModes =
      "default\nsubtractive\nfm\nkarplus-strong\nmodal\nadditive\npercussion\npiano\npipe-organ\n"
      "bowed-string\nreed\nbrass\nflute\nplucked-string\nvocal\nfree-reed\nharpsichord\n"
      "sample";
  static const std::string kWaveforms = "default\nsine\nsaw\nsquare\ntriangle\nnoise";
  static const std::string kBuiltinWaveforms = "sine\nsaw\nsawtooth\nsquare\ntriangle";
  static const std::string kFilterModels = "default\nsvf\nmoog-ladder\ndiode-ladder\nsallen-key";
  static const std::string kFilterOutputs = "default\nlowpass\nbandpass\nhighpass";
  static const std::string kBodyTypes =
      "default\nnone\nguitar\nviolin\nwood-tube\nbrass-bell\nvocal";
  static const std::string kModSources =
      "none\namp-env\nfilter-env\nlfo1\nlfo2\nvelocity\nkey-track\nmod-wheel\nrandom";
  static const std::string kModDestinations =
      "none\npitch-cents\ncutoff-cents\namp-gain\npan-units\nresonance-q\n"
      "vibrato-depth-cents\nfilter-env-depth\nlfo1-rate-scale";

  switch (kind) {
    case SONARE_SYNTH_ENUM_ENGINE_MODE:
      return kEngineModes.c_str();
    case SONARE_SYNTH_ENUM_OSC_WAVEFORM:
      return kWaveforms.c_str();
    case SONARE_SYNTH_ENUM_FILTER_MODEL:
      return kFilterModels.c_str();
    case SONARE_SYNTH_ENUM_FILTER_OUTPUT:
      return kFilterOutputs.c_str();
    case SONARE_SYNTH_ENUM_BODY_TYPE:
      return kBodyTypes.c_str();
    case SONARE_SYNTH_ENUM_MOD_SOURCE:
      return kModSources.c_str();
    case SONARE_SYNTH_ENUM_MOD_DESTINATION:
      return kModDestinations.c_str();
    case SONARE_SYNTH_ENUM_BUILTIN_WAVEFORM:
      return kBuiltinWaveforms.c_str();
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
