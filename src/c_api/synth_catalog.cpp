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
#include <utility>
#include <vector>

#include "c_api/project_internal.h"
#include "util/parameter_checks.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/synth_enum_names.h"
#include "c_api/synth_patch_common.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/patch_tuning.h"
#include "midi/synth/synth_presets.h"
#include "util/number_format.h"
#endif

const char* sonare_synth_preset_names(void) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
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
  SONARE_C_CATCH_RETURN("")
}

const char* sonare_synth_enum_names(int kind) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
#if defined(SONARE_WITH_ARRANGEMENT)
  namespace names = sonare::c_api::synth_names;
  static const std::vector<std::string> kJoined = [] {
    std::vector<std::string> joined;
    for (const names::NameTable& table : names::kTablesByKind) {
      std::string text;
      for (size_t i = 0; i < table.count; ++i) {
        if (i > 0) text += '\n';
        text += table.names[i];
      }
      joined.push_back(std::move(text));
    }
    return joined;
  }();
  if (kind < 0 || static_cast<size_t>(kind) >= kJoined.size()) return "";
  return kJoined[static_cast<size_t>(kind)].c_str();
#else
  static_cast<void>(kind);
  return "";
#endif
  SONARE_C_CATCH_RETURN("")
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
  if (out != nullptr) *out = {};
  SONARE_C_REQUIRE_NONNULL(name);
  const sonare::midi::synth::SynthPreset* preset = nullptr;
  if (const SonareError err = run_validation([&] {
        preset = sonare::midi::synth::find_synth_preset(name);
        if (preset == nullptr) {
          sonare::util::check::refuse("name", "must be the name of a synth preset");
        }
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
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

/// `"activeWhen":[[...],...]`, the gate's alternatives and each one's
/// conditions in table order.
void append_active_when(const std::vector<sonare::midi::synth::ActiveAlternative>& alternatives,
                        std::string* out) {
  using sonare::midi::synth::ActiveCondition;
  *out += ",\"activeWhen\":[";
  for (size_t a = 0; a < alternatives.size(); ++a) {
    if (a > 0) *out += ',';
    *out += '[';
    const std::vector<ActiveCondition>& conditions = alternatives[a];
    for (size_t i = 0; i < conditions.size(); ++i) {
      const ActiveCondition& c = conditions[i];
      if (i > 0) *out += ',';
      *out += "{\"key\":\"";
      *out += c.key;
      *out += "\",\"scope\":\"";
      *out += sonare::midi::synth::active_scope_name(c.scope);
      *out += "\",\"relation\":\"";
      *out += sonare::midi::synth::active_relation_name(c.relation);
      *out += "\",\"value\":";
      switch (c.kind) {
        case ActiveCondition::Kind::kBoolean:
          *out += c.number != 0.0 ? "true" : "false";
          break;
        case ActiveCondition::Kind::kString:
          *out += '"';
          *out += c.text;
          *out += '"';
          break;
        case ActiveCondition::Kind::kNumber:
          *out += descriptor_number(static_cast<float>(c.number));
          break;
      }
      *out += '}';
    }
    *out += ']';
  }
  *out += ']';
}

void append_descriptor(const DescriptorRow& row,
                       const std::vector<sonare::midi::synth::ActiveAlternative>* active_when,
                       std::string* out) {
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
  *out += '"';
  if (active_when != nullptr && !active_when->empty()) append_active_when(*active_when, out);
  *out += '}';
}

/// @p gates, when given, holds each row's activeWhen alternatives.
std::string descriptor_array(
    const std::vector<DescriptorRow>& rows,
    const std::vector<std::vector<sonare::midi::synth::ActiveAlternative>>* gates = nullptr) {
  std::string json = "[";
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i > 0) json += ',';
    append_descriptor(rows[i], gates != nullptr ? &(*gates)[i] : nullptr, &json);
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
    {sonare::midi::synth::NativeSynthParamId::kDriftRateHz, Unit::Hz},
    {sonare::midi::synth::NativeSynthParamId::kAmpDelayMs, Unit::Ms},
    {sonare::midi::synth::NativeSynthParamId::kAmpHoldMs, Unit::Ms},
    {sonare::midi::synth::NativeSynthParamId::kFilterDelayMs, Unit::Ms},
    {sonare::midi::synth::NativeSynthParamId::kFilterHoldMs, Unit::Ms},
};

/// Every numeric and switch SonareSynthPatch field: the automated ones with the automation
/// table's name and range, the rest under their facade names with the range the
/// conversion's clamps apply. Defaults are the init patch's.
std::string synth_patch_param_info_json() {
  using sonare::midi::synth::NativeSynth;
  using sonare::midi::synth::NativeSynthConfig;
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
  // The ranges of the fields not automated are the patch clamp's, read back from its extremes.
  const auto& sample = sonare_c_detail::synth_patch_sample_bounds();
  rows.push_back({"unison", true, false, true, 1.0f,
                  static_cast<float>(sonare::midi::synth::kMaxUnisonOscs),
                  static_cast<float>(init.patch.unison), Unit::Count});
  rows.push_back({"polyphony", true, false, true, 1.0f,
                  static_cast<float>(sonare::midi::kMaxSynthVoices),
                  static_cast<float>(init.polyphony), Unit::Count});
  // Negative selects no keymap; the upper end is whatever the bound bank holds.
  rows.push_back({"sampleSet", true, false, false, 0.0f, 0.0f,
                  static_cast<float>(init.patch.sample.set_index), Unit::None});
  rows.push_back({"sampleLevel", false, false, true, sample.level_lo, sample.level_hi,
                  init.patch.sample.level, Unit::Ratio});
  rows.push_back({"sampleStartOffset", false, false, true, sample.start_offset_lo,
                  sample.start_offset_hi, init.patch.sample.start_offset01, Unit::None});
  rows.push_back(
      {"oneShot", false, true, false, 0.0f, 0.0f, init.patch.one_shot ? 1.0f : 0.0f, Unit::None});
  return descriptor_array(rows);
}

}  // namespace
#endif

SonareError sonare_synth_engine_param_info(int engine_mode, char** out_json) {
  SONARE_C_API_ENTRY;
  if (out_json != nullptr) *out_json = nullptr;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (const SonareError err = run_validation([&] {
        sonare::util::check::integer_in_range(engine_mode, 0, SONARE_SYNTH_ENGINE_MODE_COUNT - 1,
                                              "engine_mode");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_json);
  SONARE_C_TRY
  if (engine_mode == SONARE_SYNTH_ENGINE_DEFAULT) {
    *out_json = sonare_c_detail::copy_string("[]");
    return SONARE_OK;
  }
  const auto mode = sonare_c_detail::admitted_ordinal_to_enum<sonare::midi::synth::SynthEngineMode>(
      engine_mode, 1);
  const char* base_name = sonare::midi::synth::base_preset_name(mode);
  if (base_name == nullptr) {
    *out_json = sonare_c_detail::copy_string("[]");
    return SONARE_OK;
  }
  const sonare::midi::synth::SynthPreset* base = sonare::midi::synth::find_synth_preset(base_name);
  if (base == nullptr) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "synth engine base preset is missing from the catalog");
  }
  std::vector<DescriptorRow> rows;
  std::vector<std::vector<sonare::midi::synth::ActiveAlternative>> gates;
  for (sonare::midi::synth::EngineParamDescriptor& d :
       sonare::midi::synth::engine_param_descriptors(base->config.patch)) {
    rows.push_back({d.key, d.integer, d.boolean, d.bounded, d.lo, d.hi, d.value, d.unit});
    gates.push_back(std::move(d.active_when));
  }
  *out_json = sonare_c_detail::copy_string(descriptor_array(rows, &gates));
  return SONARE_OK;
  SONARE_C_CATCH
#else
  (void)engine_mode;
  SONARE_C_REQUIRE_NONNULL(out_json);
  sonare_c_detail::set_last_error(SONARE_ERROR_NOT_SUPPORTED, "built without arrangement support");
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

SonareError sonare_synth_patch_param_info(char** out_json) {
  SONARE_C_API_ENTRY;
  if (out_json != nullptr) *out_json = nullptr;
  SONARE_C_REQUIRE_NONNULL(out_json);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  static const std::string kInfo = synth_patch_param_info_json();
  *out_json = sonare_c_detail::copy_string(kInfo);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  sonare_c_detail::set_last_error(SONARE_ERROR_NOT_SUPPORTED, "built without arrangement support");
  return SONARE_ERROR_NOT_SUPPORTED;
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
