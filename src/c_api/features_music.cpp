#include "c_api/features_internal.h"
#include "util/constants.h"

namespace {

SonareError fill_chroma_result(const Chroma& chroma, SonareChromaResult* out) {
  out->n_chroma = chroma.n_chroma();
  out->n_frames = chroma.n_frames();
  out->sample_rate = chroma.sample_rate();
  out->hop_length = chroma.hop_length();

  const size_t total = static_cast<size_t>(chroma.n_chroma()) * chroma.n_frames();
  if (total > 0) {
    std::unique_ptr<float[]> features(new float[total]);
    std::memcpy(features.get(), chroma.data(), total * sizeof(float));
    out->features = release_array(features);
  }

  auto mean = chroma.mean_energy();
  if (!mean.empty()) {
    std::unique_ptr<float[]> mean_out(new float[mean.size()]);
    std::memcpy(mean_out.get(), mean.data(), mean.size() * sizeof(float));
    out->mean_energy = release_array(mean_out);
  }
  return SONARE_OK;
}

// Shared body of the chroma-from-CQT entry points. ChromaCensConfig nests the
// chroma-CQT config under `base`; ChromaCqtConfig is that config.
inline ChromaCqtConfig& chroma_cqt_base(ChromaCqtConfig& config) { return config; }
inline ChromaCqtConfig& chroma_cqt_base(ChromaCensConfig& config) { return config.base; }

template <typename Config, typename Transform>
SonareError run_chroma_cqt_family(const char* fn, const char* name, const float* samples,
                                  size_t length, int sample_rate, int hop_length, int n_chroma,
                                  int bins_per_octave, SonareChromaResult* out,
                                  Transform&& transform) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  Config config;
  if (const SonareError err = run_validation([&] {
        chroma_cqt_base(config) = make_chroma_cqt_config(hop_length, n_chroma, bins_per_octave);
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline(
      fn, name, samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        Chroma chroma = sonare::detail::with_derived_cqt_grid(
            "bins_per_octave", "sample_rate", [&] { return transform(audio, config); });
        return fill_chroma_result(chroma, out);
      });
}

}  // namespace

// Features - Onset
// ============================================================================

SonareError sonare_onset_strength(const float* samples, size_t length, int sample_rate, int n_fft,
                                  int hop_length, int n_mels, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  return run_offline("onset_strength", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       MelConfig mel_config;
                       mel_config.n_fft = n_fft;
                       mel_config.hop_length = hop_length;
                       mel_config.n_mels = n_mels;
                       std::vector<float> env =
                           compute_onset_strength(audio, mel_config, OnsetConfig());

                       return copy_vector(env, out, out_length);
                     });
}

SonareError sonare_onset_strength_multi(const float* samples, size_t length, int sample_rate,
                                        int n_fft, int hop_length, int n_mels, int n_bands,
                                        float** out, size_t* out_length, int* out_n_frames) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_n_frames);
  if (const SonareError err = run_validation([&] { validate_onset_n_bands(n_bands); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_REQUIRE_NONNULL(out_n_frames);

  return run_offline("onset_strength_multi", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       MelConfig mel_config;
                       mel_config.n_fft = n_fft;
                       mel_config.hop_length = hop_length;
                       mel_config.n_mels = n_mels;
                       MelSpectrogram mel = MelSpectrogram::compute(audio, mel_config);
                       std::vector<float> env = onset_strength_multi(mel, n_bands, OnsetConfig());

                       *out_n_frames = mel.n_frames();
                       return copy_vector(env, out, out_length);
                     });
}

// Features - Chroma
// ============================================================================

SonareError sonare_chroma(const float* samples, size_t length, int sample_rate, int n_fft,
                          int hop_length, SonareChromaResult* out) {
  SONARE_C_API_ENTRY;

  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline("chroma", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       ChromaConfig config;
                       config.n_fft = n_fft;
                       config.hop_length = hop_length;
                       Chroma chroma = Chroma::compute(audio, config);

                       return fill_chroma_result(chroma, out);
                     });
}

SonareError sonare_chroma_cens(const float* samples, size_t length, int sample_rate, int hop_length,
                               int n_chroma, SonareChromaResult* out) {
  SONARE_C_API_ENTRY;
  return sonare_chroma_cens_ex(samples, length, sample_rate, hop_length, n_chroma, 36, out);
}

SonareError sonare_chroma_cens_ex(const float* samples, size_t length, int sample_rate,
                                  int hop_length, int n_chroma, int bins_per_octave,
                                  SonareChromaResult* out) {
  SONARE_C_API_ENTRY;
  return run_chroma_cqt_family<ChromaCensConfig>(
      "chroma_cens_ex", "samples", samples, length, sample_rate, hop_length, n_chroma,
      bins_per_octave, out, [](const Audio& audio, const ChromaCensConfig& config) {
        return chroma_cens(audio, config);
      });
}

SonareError sonare_chroma_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                              int n_chroma, SonareChromaResult* out) {
  SONARE_C_API_ENTRY;
  return sonare_chroma_cqt_ex(samples, length, sample_rate, hop_length, n_chroma, 36, out);
}

SonareError sonare_chroma_cqt_ex(const float* samples, size_t length, int sample_rate,
                                 int hop_length, int n_chroma, int bins_per_octave,
                                 SonareChromaResult* out) {
  SONARE_C_API_ENTRY;
  return run_chroma_cqt_family<ChromaCqtConfig>(
      "chroma_cqt_ex", "samples", samples, length, sample_rate, hop_length, n_chroma,
      bins_per_octave, out,
      [](const Audio& audio, const ChromaCqtConfig& config) { return chroma_cqt(audio, config); });
}

SonareError sonare_bass_chroma(const float* samples, size_t length, int sample_rate, int hop_length,
                               int n_chroma, SonareChromaResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  BassChromaConfig config;
  config.cqt.hop_length = hop_length;
  config.n_chroma = n_chroma;
  if (const SonareError err = run_validation([&] { validate_bass_chroma_config(config); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline("bass_chroma", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       Chroma chroma = bass_chroma(audio, config);
                       return fill_chroma_result(chroma, out);
                     });
}

SonareError sonare_nnls_chroma_ex(const float* samples, size_t length, int sample_rate,
                                  int enable_stft_blend, float stft_blend_weight,
                                  int stft_blend_n_fft, float** out, size_t* out_length,
                                  int* out_n_frames) {
  SONARE_C_API_ENTRY;
  return sonare_nnls_chroma_ex2(samples, length, sample_rate, enable_stft_blend, stft_blend_weight,
                                stft_blend_n_fft, constants::kDefaultHopLength, out, out_length,
                                out_n_frames);
}

SonareError sonare_nnls_chroma_ex2(const float* samples, size_t length, int sample_rate,
                                   int enable_stft_blend, float stft_blend_weight,
                                   int stft_blend_n_fft, int hop_length, float** out,
                                   size_t* out_length, int* out_n_frames) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_n_frames);
  NnlsChromaConfig config;
  if (const SonareError err = run_validation([&] {
        config = NnlsChromaConfig::fast(hop_length);
        config.enable_stft_blend = enable_stft_blend != 0;
        config.stft_blend_weight = stft_blend_weight;
        config.stft_blend_n_fft = stft_blend_n_fft;
        validate_nnls_chroma_config(config);
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_REQUIRE_NONNULL(out_n_frames);

  return run_offline("nnls_chroma_ex2", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       Chroma chroma = nnls_chroma(audio, config);

                       *out_n_frames = chroma.n_frames();
                       size_t total = static_cast<size_t>(chroma.n_chroma()) * chroma.n_frames();
                       *out_length = total;
                       if (total == 0) return SONARE_OK;
                       std::unique_ptr<float[]> features(new float[total]);
                       std::memcpy(features.get(), chroma.data(), total * sizeof(float));
                       *out = release_array(features);
                       return SONARE_OK;
                     });
}

SonareError sonare_nnls_chroma(const float* samples, size_t length, int sample_rate, float** out,
                               size_t* out_length, int* out_n_frames) {
  SONARE_C_API_ENTRY;
  return sonare_nnls_chroma_ex(samples, length, sample_rate, 1, 0.55f, 4096, out, out_length,
                               out_n_frames);
}
