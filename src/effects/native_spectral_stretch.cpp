#include "effects/native_spectral_stretch.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>

#include "core/resample.h"
#include "effects/phase_vocoder.h"
#include "effects/pitch_shift.h"
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
  // Checked after the fallback so the substituted defaults are what gets
  // validated; a caller-supplied geometry that cannot be overlap-added is an
  // error rather than something to repair.
  validate_cola_geometry(n_fft, hop_length);

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
  PitchShiftPlan plan;
  SONARE_CHECK(make_pitch_shift_ratio_plan(audio.size(), audio.sample_rate(), ratio, &plan),
               ErrorCode::InvalidParameter);

  // Time-stretch longer by 1/ratio (preserves pitch), then resample as if
  // sampled at sr*ratio back to sr: raises pitch by ratio, restores length.
  Audio stretched = native_spectral_time_stretch(audio, 1.0f / ratio, n_fft, hop_length);

  // Reject ratios whose effective rate falls outside the supported resampler
  // range instead of clamping (which silently changed the ratio -> wrong pitch).
  std::vector<float> result_samples =
      resample(stretched.data(), stretched.size(), plan.effective_sample_rate, audio.sample_rate());
  return Audio::from_vector(std::move(result_samples), audio.sample_rate());
}

}  // namespace sonare
