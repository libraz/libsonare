#pragma once

/// @file voice_changer.h
/// @brief Offline voice changer facade combining pitch and formant controls.

#include <cstdint>
#include <string_view>

#include "core/audio.h"
#include "effects/formant_warp.h"
#include "effects/time_stretch.h"

namespace sonare::editing::voice_changer {

/// How @ref VoiceChangerConfig::formant_factor relates to the pitch shift.
enum class FormantMode : int32_t {
  /// The factor is the warp applied after the pitch shift, so the formants end up
  /// at the factor times the shift's own ratio.
  Relative = 0,
  /// The factor is the formant shift relative to the input: the warp applied is
  /// the factor divided by the pitch ratio 2^(semitones/12), so 1 keeps the
  /// formants where they were.
  Absolute = 1,
};

/// @brief Parses "relative" or "absolute".
/// @throws SonareException InvalidParameter for any other spelling.
FormantMode parse_formant_mode(std::string_view name);

/// @brief The canonical spelling of @p mode.
const char* formant_mode_name(FormantMode mode) noexcept;

/// @brief Range of formant factors an absolute-mode change can reach at the given pitch shift.
/// @details The warp is defined over [kFormantFactorMin, kFormantFactorMax], and the
///          factor it must apply is the requested one divided by 2^(semitones/12).
/// @param[out] lo Smallest reachable formant factor.
/// @param[out] hi Largest reachable formant factor.
void reachable_formant_factor_range(float pitch_semitones, double* lo, double* hi) noexcept;

struct VoiceChangerConfig {
  float pitch_semitones = 0.0f;
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
