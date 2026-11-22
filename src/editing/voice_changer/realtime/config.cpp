#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

#include "editing/voice_changer/realtime.h"
#include "editing/voice_changer/realtime/config_ranges.h"
#include "effects/formant_warp.h"
#include "util/json_schema.h"
#include "util/refusal_predicates.h"

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

bool validate_realtime_voice_changer_config(const RealtimeVoiceChangerConfig& config,
                                            RealtimeVoiceChangerConfig* normalized,
                                            std::string* error, ConfigFieldSpelling spelling) {
  if (error) error->clear();
  bool ok = true;
  const auto path = [spelling](std::string_view member) { return field_path(member, spelling); };
  // The success path builds no strings: text is composed only once a field fails.
  visit_scalar_fields(config, [&](const char* name, const float& v, const Range& r, float) {
    if (!ok || (v >= static_cast<float>(r.lo) && v <= static_cast<float>(r.hi))) return;
    ok = false;
    if (error) *error = schema::refusal(path(name), schema::range_predicate(r.lo, r.hi));
  });
  if (ok && config.formant_mode != FormantMode::Relative &&
      config.formant_mode != FormantMode::Absolute) {
    ok = false;
    if (error) *error = schema::refusal(path("formant.mode"), "must be 'relative' or 'absolute'");
  }
  if (ok) {
    // The retune stage states its own domain; the scalars above already passed, so only the grain
    // can be refused here.
    std::string problem = streaming_retune_config_problem(
        config.retune, nullptr, {"retune.pitchSemitones", "retune.mix", "retune.grainSizeSamples"});
    if (!problem.empty()) {
      ok = false;
      if (error && spelling != ConfigFieldSpelling::Member) {
        const std::string semitones = path("retune.pitchSemitones");
        const std::string mix = path("retune.mix");
        const std::string grain = path("retune.grainSizeSamples");
        problem = streaming_retune_config_problem(config.retune, nullptr,
                                                  {semitones.c_str(), mix.c_str(), grain.c_str()});
      }
      if (error) *error = std::move(problem);
    }
  }
  visit_enum_fields(config, [&](const char* name, bool declared) {
    if (!ok || declared) return;
    ok = false;
    if (error) *error = schema::refusal(path(name), "must name a declared value");
  });
  visit_integer_fields(config, [&](const char* name, const int& v, const Range& r) {
    if (!ok || (v >= static_cast<int>(r.lo) && v <= static_cast<int>(r.hi))) return;
    ok = false;
    if (error) *error = schema::refusal(path(name), schema::integer_range_predicate(r.lo, r.hi));
  });
  // The grain engine has no f0, so nothing that needs one can run on it.
  if (ok && config.voice_engine.engine == PitchEngine::Grain) {
    std::string needs_f0;
    if (config.pitch_correction.mode != PitchCorrectionMode::Off) {
      needs_f0 = path("pitchCorrection.mode") + " other than 'off'";
    } else if (config.harmony.voices > 0) {
      needs_f0 = path("harmony.voices") + " above 0";
    } else if (config.vocoder.enabled) {
      needs_f0 = path("vocoder.enabled");
    }
    if (!needs_f0.empty()) {
      ok = false;
      if (error) {
        *error = schema::refusal(path("voiceEngine.engine"), "must be 'psola' for " + needs_f0 +
                                                                 ": the grain engine tracks no f0");
      }
    }
  }
  // Only a tempo delay can outrun the line: an echo's own delay range ends at its length.
  if (ok && space_delay_exceeds_limit(config.reverb_variation)) {
    ok = false;
    if (error) {
      *error = schema::refusal(path("reverbVariation.tempoBpm"),
                               "must give a delay of at most " +
                                   schema::bound_text(kMaxSpaceDelayMs) + " ms at " +
                                   path("reverbVariation.division") + ", the delay line's length");
    }
  }
  // Only the grain engine warps the formants ahead of the shift; psola resamples its grains.
  if (ok && config.voice_engine.engine == PitchEngine::Grain &&
      !formant_warp_is_reachable(config, error, spelling)) {
    ok = false;
  }
  if (normalized) *normalized = ok ? config : RealtimeVoiceChangerConfig{};
  return ok;
}

bool formant_warp_is_reachable(const RealtimeVoiceChangerConfig& config, std::string* error,
                               ConfigFieldSpelling spelling) {
  if (config.formant_mode != FormantMode::Absolute) return true;
  double lo = 0.0;
  double hi = 0.0;
  reachable_formant_factor_range(config.retune.semitones, &lo, &hi);
  const double factor = static_cast<double>(config.formant.factor);
  if (factor >= lo && factor <= hi) return true;
  // The factor itself lives in the warp's own range, so that is the most that can be named.
  const double named_lo = std::max(lo, static_cast<double>(kFormantFactorMin));
  const double named_hi = std::min(hi, static_cast<double>(kFormantFactorMax));
  if (error) {
    using ::sonare::util::refusal::bound_text;
    using ::sonare::util::refusal::Inward;
    const std::string at = " in absolute formant mode at " +
                           field_path("retune.pitchSemitones", spelling) + " " +
                           bound_text(static_cast<double>(config.retune.semitones));
    const std::string reach = "the warp reaches [" + bound_text(lo, Inward::Up) + ", " +
                              bound_text(hi, Inward::Down) + "] there";
    if (named_lo > named_hi) {
      *error =
          schema::refusal(field_path("formant.mode", spelling),
                          "must be relative: no formant factor is reachable" + at + " (" + reach +
                              ", outside [" + bound_text(static_cast<double>(kFormantFactorMin)) +
                              ", " + bound_text(static_cast<double>(kFormantFactorMax)) + "])");
    } else {
      *error =
          schema::refusal(field_path("formant.factor", spelling),
                          "must be finite and in [" + bound_text(named_lo, Inward::Up) + ", " +
                              bound_text(named_hi, Inward::Down) + "]" + at + " (" + reach + ")");
    }
  }
  return false;
}

}  // namespace sonare::editing::voice_changer
