#pragma once

/// @file inverse.h
/// @brief Inverse feature reconstruction: Mel/MFCC -> spectrogram -> audio.

#include <vector>

#include "core/audio.h"
#include "feature/mel_spectrogram.h"
#include "util/constants.h"

namespace sonare {

/// @brief Approximate inverse of a Mel filterbank.
/// @details Uses the pseudo-inverse of the Mel filterbank matrix (the same
/// strategy librosa's NNLS path falls back to when NNLS is unavailable). Output
/// is non-negative-clipped to remove sign artefacts.
/// @param M Mel power spectrogram [n_mels x n_frames] row-major.
/// @param n_mels Number of Mel bands.
/// @param n_frames Number of time frames.
/// @param mel_config Mel configuration that produced @p M (used to rebuild the
///        filterbank consistently).
/// @param sr Sample rate of the audio that produced @p M (Hz). The Mel
///        filterbank frequency mapping depends on it; passing the wrong value
///        skews the reconstructed spectrum.
/// @return STFT power spectrogram [(n_fft/2 + 1) x n_frames] row-major.
std::vector<float> mel_to_stft(const float* M, int n_mels, int n_frames,
                               const MelConfig& mel_config, int sr = constants::kDefaultSampleRate);

/// @brief Reconstructs audio from a Mel spectrogram via Griffin-Lim.
/// @param M Mel power spectrogram [n_mels x n_frames] row-major.
/// @param n_mels Number of Mel bands.
/// @param n_frames Number of time frames.
/// @param mel_config Mel configuration that produced @p M.
/// @param n_iter Griffin-Lim iterations (default 32).
/// @param sr Sample rate of the original audio (Hz). Used for both the Mel
///        filterbank inversion and the Griffin-Lim STFT timing.
/// @return Reconstructed audio.
Audio mel_to_audio(const float* M, int n_mels, int n_frames, const MelConfig& mel_config,
                   int n_iter = 32, int sr = constants::kDefaultSampleRate);

/// @brief Inverts MFCC coefficients back to a Mel power spectrogram.
/// @param mfcc MFCC matrix [n_mfcc x n_frames] row-major.
/// @param n_mfcc Number of MFCCs.
/// @param n_frames Number of time frames.
/// @param n_mels Number of Mel bins to reconstruct (default 128).
/// @param lifter Cepstral lifter factor that was applied when computing the MFCC
///        (default 0 = none). When non-zero the lift window is divided back out
///        before the inverse DCT, matching
///        `librosa.feature.inverse.mfcc_to_mel(..., lifter=...)`. Must match the
///        value passed to @ref MelSpectrogram::mfcc.
/// @return Mel power spectrogram [n_mels x n_frames] row-major.
std::vector<float> mfcc_to_mel(const float* mfcc, int n_mfcc, int n_frames, int n_mels = 128,
                               float lifter = 0.0f);

/// @brief Reconstructs audio directly from MFCC.
/// @details Calls @ref mfcc_to_mel, then @ref mel_to_audio with the provided
/// MelConfig (which must match the configuration that produced the MFCC).
/// @param sr Sample rate of the original audio (Hz), threaded through to
///        @ref mel_to_audio.
/// @param lifter Cepstral lifter factor applied to the MFCC (default 0 = none),
///        forwarded to @ref mfcc_to_mel so liftered MFCCs round-trip correctly.
Audio mfcc_to_audio(const float* mfcc, int n_mfcc, int n_frames, const MelConfig& mel_config,
                    int n_iter = 32, int sr = constants::kDefaultSampleRate, float lifter = 0.0f);

}  // namespace sonare
