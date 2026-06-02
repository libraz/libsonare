#include "effects/native_spectral_stretch.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/resample.h"
#include "effects/phase_vocoder.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

Audio native_spectral_time_stretch(const Audio& audio, float rate, int n_fft, int hop_length) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(rate > 0.0f, ErrorCode::InvalidParameter);

  // Guard invalid analysis sizes by falling back to the librosa-matching
  // defaults so a malformed config never produces an empty/degenerate STFT.
  if (n_fft <= 0) n_fft = constants::kDefaultNFft;
  if (hop_length <= 0) hop_length = constants::kDefaultHopLength;

  StftConfig stft_config;
  stft_config.n_fft = n_fft;
  stft_config.hop_length = hop_length;
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

Audio native_spectral_pitch_shift_ratio(const Audio& audio, float ratio, int n_fft,
                                        int hop_length) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(ratio > 0.0f, ErrorCode::InvalidParameter);

  // Time-stretch longer by 1/ratio (preserves pitch), then resample as if
  // sampled at sr*ratio back to sr: raises pitch by ratio, restores length.
  Audio stretched = native_spectral_time_stretch(audio, 1.0f / ratio, n_fft, hop_length);

  int effective_sr = static_cast<int>(std::round(static_cast<float>(audio.sample_rate()) * ratio));
  // Reject ratios whose effective rate falls outside the supported resampler
  // range instead of clamping (which silently changed the ratio -> wrong pitch).
  constexpr int kMinEffectiveSr = 1000;
  constexpr int kMaxEffectiveSr = 192000;
  if (effective_sr < kMinEffectiveSr || effective_sr > kMaxEffectiveSr) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "native_spectral_pitch_shift: ratio out of supported range for this sample rate");
  }

  std::vector<float> result_samples =
      resample(stretched.data(), stretched.size(), effective_sr, audio.sample_rate());
  return Audio::from_vector(std::move(result_samples), audio.sample_rate());
}

}  // namespace sonare
