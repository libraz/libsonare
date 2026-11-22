#pragma once

/// @file voice_changer.h
/// @brief Offline voice changer facade combining pitch and formant controls.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/audio.h"
#include "effects/formant_warp.h"
#include "effects/time_stretch.h"
#include "util/exception.h"

namespace sonare::editing::voice_changer {

/// How @ref VoiceChangerConfig::formant_factor relates to the pitch shift.
enum class FormantMode : int32_t {
  /// The factor is the warp applied after the pitch shift, so the formants end up
  /// at the factor times the shift's own ratio.
  Relative = 0,
  /// The factor is the formant shift relative to the input: the warp applied is
  /// the factor divided by the pitch ratio 2^(semitones/12), so 1 keeps the
  /// formants where they were. That warp must also lie in the factor range.
  Absolute = 1,
};

/// @brief Parses "relative" or "absolute".
/// @throws SonareException InvalidParameter for any other spelling.
FormantMode parse_formant_mode(std::string_view name);

/// @brief The canonical spelling of @p mode.
const char* formant_mode_name(FormantMode mode) noexcept;

/// @brief The warp an absolute-mode change applies: @p factor over 2^(semitones * mix / 12),
///        narrowed to the float the warp takes.
float absolute_formant_warp(float factor, float semitones, float mix) noexcept;

/// @brief Whether that warp lies in the factor range. Allocates nothing, so a control-thread
///        caller checks this before building a refusal.
bool absolute_formant_reachable(float factor, float semitones, float mix) noexcept;

/// @brief The members an absolute-mode reach refusal names, in the caller's spelling.
struct FormantReachSubjects {
  std::string factor;
  std::string mode;
  std::string semitones;
  /// Empty where the whole shift is applied.
  std::string mix;
};

/// @brief Refusal of an absolute-mode @p factor whose warp falls outside the warp's range.
/// @details @p factor already lies in [kFormantFactorMin, kFormantFactorMax]; this is the second,
///          conditional refusal, naming the mode, the shift and the factors reachable there. When
///          none are, it names the mode instead of the factor.
std::optional<SonareParameterRefusal> absolute_formant_reach_refusal(
    float factor, float semitones, float mix, const FormantReachSubjects& subjects);

struct VoiceChangerConfig {
  float pitch_semitones = 0.0f;
  /// In [kFormantFactorMin, kFormantFactorMax] in either mode; see @ref FormantMode.
  float formant_factor = 1.0f;
  StretchBackend backend = StretchBackend::NativeSpectral;
  FormantMode formant_mode = FormantMode::Relative;
};

class VoiceChanger {
 public:
  explicit VoiceChanger(VoiceChangerConfig config = {});

  Audio process(const Audio& audio) const;
  const VoiceChangerConfig& config() const noexcept { return config_; }

 private:
  VoiceChangerConfig config_{};
};

}  // namespace sonare::editing::voice_changer
