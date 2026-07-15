#include "effects/time_stretch.h"

#include <algorithm>
#include <climits>

#include "effects/native_spectral_stretch.h"
#include "effects/phase_vocoder.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare {

Audio time_stretch(const Audio& audio, float rate, const TimeStretchConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  std::size_t expected_size = 0;
  constexpr std::size_t kMaxOutputSamples =
      std::min<std::size_t>(kMaxAudioBufferSize, static_cast<std::size_t>(INT_MAX));
  SONARE_CHECK(
      numeric::checked_projected_count(audio.size(), rate, kMaxOutputSamples, &expected_size),
      ErrorCode::InvalidParameter);

  if (config.backend == StretchBackend::NativeSpectral) {
    return native_spectral_time_stretch(audio, rate, config.n_fft, config.hop_length);
  }

  /// Compute STFT
  StftConfig stft_config;
  stft_config.n_fft = config.n_fft;
  stft_config.hop_length = config.hop_length;
  stft_config.window = WindowType::Hann;
  stft_config.center = true;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  /// Apply phase vocoder
  PhaseVocoderConfig pv_config;
  pv_config.hop_length = config.hop_length;

  Spectrogram stretched = phase_vocoder(spec, rate, pv_config);

  /// Calculate expected output length
  const int expected_length = static_cast<int>(expected_size);

  /// Convert back to audio
  return stretched.to_audio(expected_length);
}

}  // namespace sonare
