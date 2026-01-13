#include "effects/time_stretch.h"

#include "effects/phase_vocoder.h"
#include "util/exception.h"

namespace sonare {

Audio time_stretch(const Audio& audio, float rate, const TimeStretchConfig& config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(rate > 0.0f, ErrorCode::InvalidParameter);

  // Compute STFT
  StftConfig stft_config;
  stft_config.n_fft = config.n_fft;
  stft_config.hop_length = config.hop_length;
  stft_config.window = WindowType::Hann;
  stft_config.center = true;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  // Apply phase vocoder
  PhaseVocoderConfig pv_config;
  pv_config.hop_length = config.hop_length;

  Spectrogram stretched = phase_vocoder(spec, rate, pv_config);

  // Calculate expected output length
  int expected_length = static_cast<int>(std::ceil(static_cast<float>(audio.size()) / rate));

  // Convert back to audio
  return stretched.to_audio(expected_length);
}

}  // namespace sonare
