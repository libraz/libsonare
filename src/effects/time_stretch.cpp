#include "effects/time_stretch.h"

#include <algorithm>
#include <climits>
#include <limits>

#include "core/spectrum.h"
#include "effects/native_spectral_stretch.h"
#include "effects/phase_vocoder.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

namespace sonare {

Audio time_stretch(const Audio& audio, float rate, const TimeStretchConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  std::size_t expected_size = 0;
  constexpr std::size_t kMaxOutputSamples =
      std::min<std::size_t>(kMaxAudioBufferSize, static_cast<std::size_t>(INT_MAX));
  validate_stretch_rate(rate);
  util::check::in_computed_range(
      rate, static_cast<double>(audio.size()) / static_cast<double>(kMaxOutputSamples),
      static_cast<double>(std::numeric_limits<float>::max()), "rate");
  SONARE_CHECK(
      numeric::checked_projected_count(audio.size(), rate, kMaxOutputSamples, &expected_size),
      ErrorCode::InvalidParameter);

  const int hop_length = resolve_resynthesis_hop(config.hop_length, config.n_fft);
  if (config.backend == StretchBackend::NativeSpectral) {
    return native_spectral_time_stretch(audio, rate, config.n_fft, hop_length);
  }

  Spectrogram spec = phase_vocoder_analysis(audio, config.n_fft, hop_length);

  /// Apply phase vocoder
  PhaseVocoderConfig pv_config;
  pv_config.hop_length = hop_length;

  Spectrogram stretched = phase_vocoder(spec, rate, pv_config);

  /// Calculate expected output length
  const int expected_length = static_cast<int>(expected_size);

  /// Convert back to audio
  return stretched.to_audio(expected_length);
}

}  // namespace sonare
