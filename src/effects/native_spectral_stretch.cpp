#include "effects/native_spectral_stretch.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/resample.h"
#include "effects/phase_vocoder.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

Audio native_spectral_time_stretch(const Audio& audio, float rate) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(rate > 0.0f, ErrorCode::InvalidParameter);

  StftConfig stft_config;
  stft_config.n_fft = constants::kDefaultNFft;
  stft_config.hop_length = constants::kDefaultHopLength;
  stft_config.window = WindowType::Hann;
  stft_config.center = true;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = stft_config.hop_length;

  Spectrogram stretched = phase_vocoder_phaselocked(spec, rate, pv_config);
  const int output_samples =
      std::max(1, static_cast<int>(std::ceil(static_cast<float>(audio.size()) / rate)));
  return stretched.to_audio(output_samples);
}

Audio native_spectral_pitch_shift_ratio(const Audio& audio, float ratio) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(ratio > 0.0f, ErrorCode::InvalidParameter);

  // Time-stretch longer by 1/ratio (preserves pitch), then resample as if
  // sampled at sr*ratio back to sr: raises pitch by ratio, restores length.
  Audio stretched = native_spectral_time_stretch(audio, 1.0f / ratio);

  int effective_sr = static_cast<int>(std::round(static_cast<float>(audio.sample_rate()) * ratio));
  effective_sr = std::clamp(effective_sr, 1000, 192000);

  std::vector<float> result_samples =
      resample(stretched.data(), stretched.size(), effective_sr, audio.sample_rate());
  return Audio::from_vector(std::move(result_samples), audio.sample_rate());
}

}  // namespace sonare
