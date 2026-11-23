#include "editing/voice_changer/voice_changer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "effects/pitch_shift.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/parameter_checks.h"
#include "util/refusal_predicates.h"

namespace sonare::editing::voice_changer {

namespace check = sonare::util::check;

namespace {

double pitch_ratio(float semitones) {
  return std::exp2(static_cast<double>(semitones) /
                   static_cast<double>(sonare::constants::kSemitonesPerOctave));
}

bool is_unity_factor(float factor) { return std::abs(factor - 1.0f) < 1.0e-6f; }

}  // namespace

FormantMode parse_formant_mode(std::string_view name) {
  if (name == "relative") return FormantMode::Relative;
  if (name == "absolute") return FormantMode::Absolute;
  throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, "formant_mode",
                                          util::refusal::kDeclaredValuePredicate);
}

const char* formant_mode_name(FormantMode mode) noexcept {
  return mode == FormantMode::Absolute ? "absolute" : "relative";
}

float absolute_formant_warp(float factor, float semitones, float mix) noexcept {
  return static_cast<float>(
      static_cast<double>(factor) /
      pitch_ratio(static_cast<float>(static_cast<double>(semitones) * static_cast<double>(mix))));
}

bool absolute_formant_reachable(float factor, float semitones, float mix) noexcept {
  return formant_factor_in_range(absolute_formant_warp(factor, semitones, mix));
}

std::optional<SonareParameterRefusal> absolute_formant_reach_refusal(
    float factor, float semitones, float mix, const FormantReachSubjects& subjects) {
  if (absolute_formant_reachable(factor, semitones, mix)) return std::nullopt;
  using util::refusal::bound_text;
  const double ratio =
      pitch_ratio(static_cast<float>(static_cast<double>(semitones) * static_cast<double>(mix)));
  const double range_lo = static_cast<double>(kFormantFactorMin);
  const double range_hi = static_cast<double>(kFormantFactorMax);
  const double lo = std::max(range_lo, range_lo * ratio);
  const double hi = std::min(range_hi, range_hi * ratio);
  const std::string range = "[" + bound_text(range_lo) + ", " + bound_text(range_hi) + "]";
  std::vector<std::string> names{subjects.factor, subjects.mode, subjects.semitones};
  std::string at = " at {2} " + bound_text(static_cast<double>(semitones));
  if (!subjects.mix.empty()) {
    names.push_back(subjects.mix);
    at += " and {3} " + bound_text(static_cast<double>(mix));
  }
  if (lo > hi) {
    return SonareParameterRefusal::relation(ErrorCode::InvalidParameter, std::move(names),
                                            "{1} must be relative" + at + ": no {0} in " + range +
                                                " has a warp in " + range + " there");
  }
  return SonareParameterRefusal::relation(ErrorCode::InvalidParameter, std::move(names),
                                          "{0} " + util::refusal::computed_range_predicate(lo, hi) +
                                              " when {1} is absolute" + at +
                                              " (the warp it needs must lie in " + range + ")");
}

VoiceChanger::VoiceChanger(VoiceChangerConfig config) : config_(config) {}

Audio VoiceChanger::process(const Audio& audio) const {
  if (audio.empty()) check::refuse("audio", util::refusal::kNonEmptyPredicate);
  // The epsilon test below answers "is this near zero", and a NaN answers false
  // to every comparison -- so an unguarded NaN skipped the pitch branch
  // entirely and returned the input unchanged while reporting success. The
  // formant sibling is already checked inside FormantWarp; check this one here,
  // in the core, so all four surfaces and both CLIs inherit it rather than each
  // growing its own copy. +/-Inf needs no case here: it enters the branch and
  // pitch_shift rejects it.
  check::finite_float(config_.pitch_semitones, "pitch_semitones");

  // The warp factor is resolved, and an unreachable absolute request refused, before the
  // pitch shift is paid for.
  check::in_range(config_.formant_factor, kFormantFactorMin, kFormantFactorMax, "formant_factor");
  float warp_factor = config_.formant_factor;
  if (config_.formant_mode == FormantMode::Absolute) {
    if (auto refusal = absolute_formant_reach_refusal(
            config_.formant_factor, config_.pitch_semitones, 1.0f,
            {"formant_factor", "formant_mode", "pitch_semitones", std::string()})) {
      throw *refusal;
    }
    warp_factor = absolute_formant_warp(config_.formant_factor, config_.pitch_semitones, 1.0f);
  }

  PitchShiftConfig pitch_config;
  pitch_config.backend = config_.backend;
  const bool shifts = std::abs(config_.pitch_semitones) > 1.0e-6f;

  FormantWarpConfig formant_config;
  formant_config.factor = warp_factor;

  if (config_.formant_mode == FormantMode::Absolute) {
    // The envelope is estimated on the unshifted voice and the pitch shift then carries it to
    // the requested factor.
    const Audio warped =
        is_unity_factor(warp_factor) ? audio : FormantWarp(formant_config).process(audio);
    return shifts ? pitch_shift(warped, config_.pitch_semitones, pitch_config) : warped;
  }

  Audio shifted = shifts ? pitch_shift(audio, config_.pitch_semitones, pitch_config) : audio;

  if (is_unity_factor(warp_factor)) return shifted;

  FormantWarp warp(formant_config);
  return warp.process(shifted);
}

}  // namespace sonare::editing::voice_changer
