#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "editing/voice_changer/realtime.h"
#include "editing/voice_changer/realtime/config_ranges.h"
#include "util/json_schema.h"

namespace sonare::editing::voice_changer {
namespace detail {

/// Forces @p input into range (non-finite fields take their default); only preset construction may
/// clamp, since a preset's values are the library's own and never the caller's.
RealtimeVoiceChangerConfig normalize_config(const RealtimeVoiceChangerConfig& input) {
  RealtimeVoiceChangerConfig c = input;
  visit_scalar_fields(c, [](const char*, float& v, const Range& r, float fallback) {
    if (!std::isfinite(v)) v = fallback;
    v = std::clamp(v, static_cast<float>(r.lo), static_cast<float>(r.hi));
  });
  c.retune.grain_size = std::clamp(c.retune.grain_size, 0, kMaxRetuneGrainSize);
  if (c.formant_mode != FormantMode::Absolute) c.formant_mode = FormantMode::Relative;
  visit_integer_fields(c, [](const char*, int& v, const Range& r) {
    v = std::clamp(v, static_cast<int>(r.lo), static_cast<int>(r.hi));
  });
  const RealtimeVoiceChangerConfig defaults;
  default_if_undeclared(c.voice_engine.engine, defaults.voice_engine.engine);
  default_if_undeclared(c.pitch_correction.mode, defaults.pitch_correction.mode);
  default_if_undeclared(c.pitch_correction.scale, defaults.pitch_correction.scale);
  default_if_undeclared(c.vocoder.carrier_source, defaults.vocoder.carrier_source);
  default_if_undeclared(c.vocoder.carrier_wave, defaults.vocoder.carrier_wave);
  default_if_undeclared(c.character.type, defaults.character.type);
  default_if_undeclared(c.reverb_variation.variation, defaults.reverb_variation.variation);
  default_if_undeclared(c.reverb_variation.division, defaults.reverb_variation.division);
  default_if_undeclared(c.midi.release, defaults.midi.release);
  return c;
}

}  // namespace detail

using namespace detail;

namespace {

namespace schema = sonare::util::json::schema;

}  // namespace

std::string realtime_voice_changer_field_path(std::string_view member,
                                              ConfigFieldSpelling spelling) {
  switch (spelling) {
    case ConfigFieldSpelling::Member:
      return std::string(member);
    case ConfigFieldSpelling::PresetDocument:
      return schema::child_path("dsp", member);
    case ConfigFieldSpelling::FlatPod:
    case ConfigFieldSpelling::CStruct:
      break;
  }
  // A flat POD key joins section and field in camel case; an enum named after its section is
  // the section alone.
  // The C struct's field is the flat key in snake case.
  const bool snake = spelling == ConfigFieldSpelling::CStruct;
  if (member == "voiceEngine.engine") return snake ? "voice_engine" : "voiceEngine";
  if (member == "reverbVariation.variation") return snake ? "reverb_variation" : "reverbVariation";
  std::string out;
  bool upper = false;
  for (const char c : member) {
    if (c == '.') {
      upper = true;
      continue;
    }
    const bool cap = upper || std::isupper(static_cast<unsigned char>(c));
    if (snake && cap) {
      out += '_';
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else {
      out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
    }
    upper = false;
  }
  return out;
}

namespace {

std::string field_path(std::string_view member, ConfigFieldSpelling spelling) {
  return realtime_voice_changer_field_path(member, spelling);
}

}  // namespace

namespace {

void publish_refusal(const SonareParameterRefusal& refusal, std::string* error,
                     std::optional<SonareParameterRefusal>* out_refusal) {
  if (error) *error = refusal.what();
  if (out_refusal) *out_refusal = refusal;
}

}  // namespace

bool validate_realtime_voice_changer_config(const RealtimeVoiceChangerConfig& config,
                                            RealtimeVoiceChangerConfig* normalized,
                                            std::string* error, ConfigFieldSpelling spelling,
                                            std::optional<SonareParameterRefusal>* out_refusal) {
  if (error) error->clear();
  if (out_refusal) out_refusal->reset();
  bool ok = true;
  const auto path = [spelling](std::string_view member) { return field_path(member, spelling); };
  const auto reject = [&](const SonareParameterRefusal& refusal) {
    ok = false;
    publish_refusal(refusal, error, out_refusal);
  };

  visit_scalar_fields(config, [&](const char* name, const float& value, const Range& range, float) {
    if (!ok || (value >= static_cast<float>(range.lo) && value <= static_cast<float>(range.hi))) {
      return;
    }
    reject(SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, path(name),
                                             schema::range_predicate(range.lo, range.hi)));
  });
  if (ok && config.formant_mode != FormantMode::Relative &&
      config.formant_mode != FormantMode::Absolute) {
    reject(SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, path("formant.mode"),
                                             "must be 'relative' or 'absolute'"));
  }
  if (ok) {
    std::optional<SonareParameterRefusal> refusal;
    streaming_retune_config_problem(
        config.retune, nullptr, {"retune.pitchSemitones", "retune.mix", "retune.grainSizeSamples"},
        &refusal);
    if (refusal) {
      reject(refusal->respelled(
          refusal->code(), [&](const std::string& subject) { return path(subject); }, false));
    }
  }
  visit_enum_fields(config, [&](const char* name, bool declared) {
    if (!ok || declared) return;
    reject(SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, path(name),
                                             "must name a declared value"));
  });
  visit_integer_fields(config, [&](const char* name, const int& value, const Range& range) {
    if (!ok || (value >= static_cast<int>(range.lo) && value <= static_cast<int>(range.hi))) return;
    reject(SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, path(name),
                                             schema::integer_range_predicate(range.lo, range.hi)));
  });

  // The grain engine has no f0, so name both it and the selected feature that requires one.
  if (ok && config.voice_engine.engine == PitchEngine::Grain) {
    std::string field;
    std::string requirement;
    if (config.pitch_correction.mode != PitchCorrectionMode::Off) {
      field = "pitchCorrection.mode";
      requirement = " other than 'off'";
    } else if (config.harmony.voices > 0) {
      field = "harmony.voices";
      requirement = " above 0";
    } else if (config.vocoder.enabled) {
      field = "vocoder.enabled";
    }
    if (!field.empty()) {
      reject(SonareParameterRefusal::relation(
          ErrorCode::InvalidParameter, {path("voiceEngine.engine"), path(field)},
          "{0} must be 'psola' for {1}" + requirement + ": the grain engine tracks no f0"));
    }
  }
  // Only a tempo delay can outrun the line: an echo's own delay range ends at its length.
  if (ok && space_delay_exceeds_limit(config.reverb_variation)) {
    reject(SonareParameterRefusal::relation(
        ErrorCode::InvalidParameter,
        {path("reverbVariation.tempoBpm"), path("reverbVariation.division")},
        "{0} must give a delay of at most " + schema::bound_text(kMaxSpaceDelayMs) +
            " ms at {1}, the delay line's length"));
  }
  // Only the grain engine warps the formants ahead of the shift. Psola resamples its grains by the
  // factor itself, whatever the shift, so the factor's own range is its whole contract there.
  if (ok && config.voice_engine.engine == PitchEngine::Grain) {
    std::optional<SonareParameterRefusal> refusal;
    if (!formant_warp_is_reachable(config, nullptr, spelling, &refusal) && refusal) {
      reject(*refusal);
    }
  }
  if (normalized) *normalized = ok ? config : RealtimeVoiceChangerConfig{};
  return ok;
}

bool formant_warp_is_reachable(const RealtimeVoiceChangerConfig& config, std::string* error,
                               ConfigFieldSpelling spelling,
                               std::optional<SonareParameterRefusal>* out_refusal) {
  if (error) error->clear();
  if (out_refusal) out_refusal->reset();
  if (config.formant_mode != FormantMode::Absolute ||
      absolute_formant_reachable(config.formant.factor, config.retune.semitones,
                                 config.retune.mix)) {
    return true;
  }
  const auto refusal = absolute_formant_reach_refusal(
      config.formant.factor, config.retune.semitones, config.retune.mix,
      {field_path("formant.factor", spelling), field_path("formant.mode", spelling),
       field_path("retune.pitchSemitones", spelling), field_path("retune.mix", spelling)});
  if (refusal) publish_refusal(*refusal, error, out_refusal);
  return false;
}

}  // namespace sonare::editing::voice_changer
