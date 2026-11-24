#pragma once

#include "core/audio.h"
#include "core/spectrum.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "feature/rhythm.h"
#include "filters/mel.h"
#include "util/parameter_checks.h"

namespace sonare_c_detail {

// Admit the input buffer before scalar arguments, without constructing an Audio.
template <typename ScanSamples>
void admit_forward_mel(std::size_t length, int sample_rate, const sonare::MelConfig& config,
                       ScanSamples&& scan_samples, const int* n_mfcc = nullptr) {
  namespace check = sonare::util::check;
  sonare::validate_offline_audio_extent("samples", length, sample_rate);
  scan_samples();
  // Refuse the edges before admitting the STFT geometry.
  sonare::validate_mel_bounds(config.fmin, config.fmax, sample_rate);
  sonare::validate_config(config.to_stft_config());
  check::integer_within(config.n_mels, sonare::kMelBandsDomain, "n_mels");
  if (n_mfcc) {
    check::integer_within(*n_mfcc, sonare::kMfccCountDomain, "n_mfcc");
    if (*n_mfcc > config.n_mels) {
      check::refuse_relation({"n_mfcc", "n_mels"}, "{0} must be at most {1}");
    }
  }
}

// Refuses an inverse-transform input buffer that does not hold exactly rows * n_frames floats,
// guarding the product against size_t overflow (32-bit WASM); rows and n_frames are >= 1.
inline void check_inverse_input_length(size_t input_length, int rows, int n_frames,
                                       const char* rows_subject) {
  namespace check = sonare::util::check;
  const size_t r = static_cast<size_t>(rows);
  const size_t f = static_cast<size_t>(n_frames);
  if (r > sonare::kMaxAudioBufferSize / f) {
    check::refuse_relation(
        {rows_subject, "n_frames"},
        "{0} * {1} must be at most " + std::to_string(sonare::kMaxAudioBufferSize));
  }
  if (input_length != r * f) {
    check::refuse_relation({"input_length", rows_subject, "n_frames"}, "{0} must equal {1} * {2}");
  }
}

enum class MelInverseKind { kStft, kMelAudio, kMfccAudio };

template <typename ScanCoefficients>
void admit_inverse_mel(MelInverseKind kind, size_t length, int rows, int frames, int sample_rate,
                       const sonare::MelConfig& config, int n_iter, float lifter,
                       ScanCoefficients&& scan_coefficients) {
  namespace check = sonare::util::check;
  const bool mfcc = kind == MelInverseKind::kMfccAudio;
  const char* rows_subject = mfcc ? "n_mfcc" : "n_mels";
  check::integer_within(
      rows, mfcc ? sonare::kInverseMfccCountDomain : sonare::kInverseMelBandsDomain, rows_subject);
  check::integer_within(frames, sonare::kMelToStftFramesDomain, "n_frames");
  check_inverse_input_length(length, rows, frames, rows_subject);
  const auto check_hop = [&] {
    if (config.hop_length != 0) {
      check::integer_within(config.hop_length, sonare::kStftHopDomain, "hop_length");
    }
  };
  if (kind == MelInverseKind::kStft) {
    check::integer_within(config.n_fft, sonare::kMelToStftFftSizeDomain, "n_fft");
    check::integer_within(sample_rate, sonare::kInverseSampleRateDomain, "sample_rate");
  } else {
    check::even_within(config.n_fft, sonare::kMelToAudioFftSizeDomain, "n_fft");
    if (kind == MelInverseKind::kMfccAudio) check_hop();
    check::integer_within(sample_rate, sonare::kInverseAudioSampleRateDomain, "sample_rate");
    if (kind == MelInverseKind::kMelAudio) check_hop();
  }
  sonare::validate_mel_bounds(config.fmin, config.fmax, sample_rate);
  if (kind == MelInverseKind::kMfccAudio) {
    check::integer_within(config.n_mels, sonare::kInverseMelBandsDomain, "n_mels");
    check::at_least(lifter, 0.0, "lifter");
    scan_coefficients();
  }
  if (kind != MelInverseKind::kStft) {
    check::integer_within(n_iter, sonare::kInverseIterationsDomain, "n_iter");
  }
}

inline void admit_inverse_mel(MelInverseKind kind, size_t length, int rows, int frames,
                              int sample_rate, const sonare::MelConfig& config, int n_iter = 0) {
  admit_inverse_mel(kind, length, rows, frames, sample_rate, config, n_iter, 0.0f, [] {});
}

// The C buffer contract admits an empty envelope and scans populated envelopes first.
template <typename ScanEnvelope>
void admit_cyclic_tempogram(int sample_rate, int hop_length, int win_length, float bpm_min,
                            int n_bins, ScanEnvelope&& scan_envelope) {
  namespace check = sonare::util::check;
  scan_envelope();
  check::integer_within(sample_rate, sonare::kRhythmCountDomain, "sample_rate");
  check::integer_within(win_length, sonare::kTempogramWinLengthDomain, "win_length");
  check::integer_within(hop_length, sonare::kRhythmCountDomain, "hop_length");
  check::greater_than(bpm_min, 0.0, "bpm_min");
  check::integer_within(n_bins, sonare::kRhythmCountDomain, "n_bins");
}

}  // namespace sonare_c_detail
