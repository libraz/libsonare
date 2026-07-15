#include "effects/native_spectral_stretch.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>

#include "core/resample.h"
#include "effects/phase_vocoder.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare {

Audio native_spectral_time_stretch(const Audio& audio, float rate, int n_fft, int hop_length) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  std::size_t output_size = 0;
  constexpr std::size_t kMaxOutputSamples =
      std::min<std::size_t>(kMaxAudioBufferSize, static_cast<std::size_t>(INT_MAX));
  SONARE_CHECK(
      numeric::checked_projected_count(audio.size(), rate, kMaxOutputSamples, &output_size),
      ErrorCode::InvalidParameter);

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
  const int output_samples = std::max(1, static_cast<int>(output_size));
  return stretched.to_audio(output_samples);
}

Audio native_spectral_pitch_shift_ratio(const Audio& audio, float ratio, int n_fft,
                                        int hop_length) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(numeric::finite_positive(ratio), ErrorCode::InvalidParameter);

  const long double effective_sr_wide =
      std::round(static_cast<long double>(audio.sample_rate()) * static_cast<long double>(ratio));
  constexpr int kMinEffectiveSr = 1000;
  constexpr int kMaxEffectiveSr = 192000;
  SONARE_CHECK(std::isfinite(effective_sr_wide) && effective_sr_wide >= kMinEffectiveSr &&
                   effective_sr_wide <= kMaxEffectiveSr,
               ErrorCode::InvalidParameter);
  const int effective_sr = static_cast<int>(effective_sr_wide);

  // Time-stretch longer by 1/ratio (preserves pitch), then resample as if
  // sampled at sr*ratio back to sr: raises pitch by ratio, restores length.
  Audio stretched = native_spectral_time_stretch(audio, 1.0f / ratio, n_fft, hop_length);

  // Reject ratios whose effective rate falls outside the supported resampler
  // range instead of clamping (which silently changed the ratio -> wrong pitch).
  std::vector<float> result_samples =
      resample(stretched.data(), stretched.size(), effective_sr, audio.sample_rate());
  return Audio::from_vector(std::move(result_samples), audio.sample_rate());
}

}  // namespace sonare
