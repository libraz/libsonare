#include "editing/voice_changer/voice_changer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "effects/pitch_shift.h"
#include "util/exception.h"

namespace sonare::editing::voice_changer {

namespace {

double pitch_ratio(float semitones) { return std::exp2(static_cast<double>(semitones) / 12.0); }

}  // namespace

FormantMode parse_formant_mode(std::string_view name) {
  if (name == "relative") return FormantMode::Relative;
  if (name == "absolute") return FormantMode::Absolute;
  throw SonareException(
      ErrorCode::InvalidParameter,
      "formant mode must be 'relative' or 'absolute', got '" + std::string(name) + "'");
}

const char* formant_mode_name(FormantMode mode) noexcept {
  return mode == FormantMode::Absolute ? "absolute" : "relative";
}

void reachable_formant_factor_range(float pitch_semitones, double* lo, double* hi) noexcept {
  const double ratio = pitch_ratio(pitch_semitones);
  *lo = static_cast<double>(kFormantFactorMin) * ratio;
  *hi = static_cast<double>(kFormantFactorMax) * ratio;
}

VoiceChanger::VoiceChanger(VoiceChangerConfig config) : config_(config) {}

Audio VoiceChanger::process(const Audio& audio) const {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  // The epsilon test below answers "is this near zero", and a NaN answers false
  // to every comparison -- so an unguarded NaN skipped the pitch branch
  // entirely and returned the input unchanged while reporting success. The
  // formant sibling is already checked inside FormantWarp; check this one here,
  // in the core, so all four surfaces and both CLIs inherit it rather than each
  // growing its own copy. +/-Inf needs no case here: it enters the branch and
  // pitch_shift rejects it.
  SONARE_CHECK(std::isfinite(config_.pitch_semitones), ErrorCode::InvalidParameter);

  // The warp factor is resolved, and an unreachable absolute request refused, before the
  // pitch shift is paid for.
  float warp_factor = config_.formant_factor;
  if (config_.formant_mode == FormantMode::Absolute) {
    SONARE_CHECK_MSG(std::isfinite(config_.formant_factor), ErrorCode::InvalidParameter,
                     "formant factor must be finite");
    double lo = 0.0;
    double hi = 0.0;
    reachable_formant_factor_range(config_.pitch_semitones, &lo, &hi);
    const double requested = static_cast<double>(config_.formant_factor);
    if (!(requested >= lo && requested <= hi)) {
      char text[240];
      std::snprintf(text, sizeof(text),
                    "absolute formant mode: formant factor must be in [%.4g, %.4g] at a pitch "
                    "shift of %.4g semitones (the formant warp covers [%.2f, %.2f]), got %.4g",
                    lo, hi, static_cast<double>(config_.pitch_semitones),
                    static_cast<double>(kFormantFactorMin), static_cast<double>(kFormantFactorMax),
                    requested);
      throw SonareException(ErrorCode::InvalidParameter, text);
    }
    warp_factor = static_cast<float>(std::clamp(requested / pitch_ratio(config_.pitch_semitones),
                                                static_cast<double>(kFormantFactorMin),
                                                static_cast<double>(kFormantFactorMax)));
  }

  PitchShiftConfig pitch_config;
  pitch_config.backend = config_.backend;
  const bool shifts = std::abs(config_.pitch_semitones) > 1.0e-6f;

  FormantWarpConfig formant_config;
  formant_config.factor = warp_factor;

  if (config_.formant_mode == FormantMode::Absolute) {
    // The envelope is estimated on the unshifted voice and the pitch shift then carries it to
    // the requested factor. The LPC order follows the rate: 12 only resolves formants near 22 kHz.
    formant_config.lpc_order = 0;
    formant_config.frame_in_time = true;
    const Audio warped =
        std::abs(warp_factor - 1.0f) < 1.0e-6f ? audio : FormantWarp(formant_config).process(audio);
    return shifts ? pitch_shift(warped, config_.pitch_semitones, pitch_config) : warped;
  }

  Audio shifted = shifts ? pitch_shift(audio, config_.pitch_semitones, pitch_config) : audio;

  if (std::abs(warp_factor - 1.0f) < 1.0e-6f) return shifted;

  FormantWarp warp(formant_config);
  return warp.process(shifted);
}

}  // namespace sonare::editing::voice_changer
