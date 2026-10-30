#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "editing/voice_changer/realtime.h"
#include "editing/voice_changer/realtime/config_ranges.h"
#include "effects/formant_warp.h"

namespace sonare::editing::voice_changer {
namespace detail {

/// Forces @p input into range (non-finite fields take their default); only preset construction and
/// the tolerant JSON entry point may clamp.
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

bool validate_realtime_voice_changer_config(const RealtimeVoiceChangerConfig& config,
                                            RealtimeVoiceChangerConfig* normalized,
                                            std::string* error) {
  if (error) error->clear();
  bool ok = true;
  // The success path builds no strings: text is composed only once a field fails.
  visit_scalar_fields(config, [&](const char* name, const float& v, const Range&, float) {
    if (!ok || std::isfinite(v)) return;
    ok = false;
    if (error) *error = std::string("config field is not finite: ") + name;
  });
  if (ok && config.formant_mode != FormantMode::Relative &&
      config.formant_mode != FormantMode::Absolute) {
    ok = false;
    if (error) *error = "formant mode must be 'relative' or 'absolute'";
  }
  visit_scalar_fields(config, [&](const char* name, const float& v, const Range& r, float) {
    if (!ok || (v >= static_cast<float>(r.lo) && v <= static_cast<float>(r.hi))) return;
    ok = false;
    if (error) {
      char text[160];
      std::snprintf(text, sizeof(text), "%s out of range [%.6g, %.6g], got %.6g", name, r.lo, r.hi,
                    static_cast<double>(v));
      *error = text;
    }
  });
  if (ok && (config.retune.grain_size < 0 || config.retune.grain_size > kMaxRetuneGrainSize)) {
    ok = false;
    if (error) {
      char text[160];
      std::snprintf(text, sizeof(text), "retune.grainSize out of range [0, %d], got %d",
                    kMaxRetuneGrainSize, config.retune.grain_size);
      *error = text;
    }
  }
  visit_enum_fields(config, [&](const char* name, bool declared) {
    if (!ok || declared) return;
    ok = false;
    if (error) *error = std::string("config field names no declared value: ") + name;
  });
  visit_integer_fields(config, [&](const char* name, const int& v, const Range& r) {
    if (!ok || (v >= static_cast<int>(r.lo) && v <= static_cast<int>(r.hi))) return;
    ok = false;
    if (error) {
      char text[160];
      std::snprintf(text, sizeof(text), "%s out of range [%d, %d], got %d", name,
                    static_cast<int>(r.lo), static_cast<int>(r.hi), v);
      *error = text;
    }
  });
  // The grain engine has no f0, so nothing that needs one can run on it.
  if (ok && config.voice_engine.engine == PitchEngine::Grain) {
    const char* needs_f0 = nullptr;
    if (config.pitch_correction.mode != PitchCorrectionMode::Off) {
      needs_f0 = "pitchCorrection.mode other than 'off'";
    } else if (config.harmony.voices > 0) {
      needs_f0 = "harmony.voices above 0";
    } else if (config.vocoder.enabled) {
      needs_f0 = "vocoder.enabled";
    }
    if (needs_f0 != nullptr) {
      ok = false;
      if (error) {
        *error = std::string(needs_f0) +
                 " needs voiceEngine.engine 'psola': the grain engine tracks no f0";
      }
    }
  }
  if (ok && space_delay_exceeds_limit(config.reverb_variation)) {
    ok = false;
    if (error) {
      char text[200];
      std::snprintf(text, sizeof(text),
                    "reverbVariation delay out of range: %.6g ms exceeds the %.6g ms delay line",
                    static_cast<double>(space_delay_ms(config.reverb_variation)),
                    static_cast<double>(kMaxSpaceDelayMs));
      *error = text;
    }
  }
  // Only the grain engine warps the formants ahead of the shift; psola resamples its grains.
  if (ok && config.voice_engine.engine == PitchEngine::Grain &&
      !formant_warp_is_reachable(config, error)) {
    ok = false;
  }
  if (normalized) *normalized = ok ? config : RealtimeVoiceChangerConfig{};
  return ok;
}

bool formant_warp_is_reachable(const RealtimeVoiceChangerConfig& config, std::string* error) {
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
    char text[360];
    const double semitones = static_cast<double>(config.retune.semitones);
    if (named_lo > named_hi) {
      std::snprintf(text, sizeof(text),
                    "absolute formant mode: no formant factor is reachable at a pitch shift of "
                    "%.4g semitones (the warp reaches [%.4g, %.4g] there, outside formantFactor's "
                    "[%.4g, %.4g])",
                    semitones, lo, hi, static_cast<double>(kFormantFactorMin),
                    static_cast<double>(kFormantFactorMax));
    } else {
      std::snprintf(text, sizeof(text),
                    "absolute formant mode: formant factor must be in [%.4g, %.4g] at a pitch "
                    "shift of %.4g semitones (the warp reaches [%.4g, %.4g] there, limited by "
                    "formantFactor's [%.4g, %.4g]), got %.4g",
                    named_lo, named_hi, semitones, lo, hi, static_cast<double>(kFormantFactorMin),
                    static_cast<double>(kFormantFactorMax), factor);
    }
    *error = text;
  }
  return false;
}

}  // namespace sonare::editing::voice_changer
