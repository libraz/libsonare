#pragma once

/// @file phase_vocoder.h
/// @brief Phase vocoder for time-scale modification.

#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

#include "core/audio.h"
#include "core/fft.h"
#include "core/spectrum.h"

namespace sonare {

/// @brief Configuration for phase vocoder.
struct PhaseVocoderConfig {
  int hop_length = 512;  ///< Hop length for analysis/synthesis
};

struct StreamingPhaseVocoderConfig {
  int sample_rate = 48000;
  int n_fft = 2048;
  int hop_length = 512;
  int win_length = 0;
  bool phase_lock = true;
};

/// @brief Performs phase vocoder time-stretching on a spectrogram.
/// @details Resamples the spectrogram in time while maintaining phase coherence.
///          Uses linear magnitude interpolation and phase accumulation with
///          instantaneous frequency estimation.
///
///          Boundary handling:
///          - At the start: uses first frame without interpolation
///          - At the end: clamps to last two frames, using frame[-2] and frame[-1]
///          - For very short spectrograms (< 2 frames), behavior may be undefined
///
/// @param spec Input spectrogram (must have at least 2 frames)
/// @param rate Time stretch rate (< 1.0 = slower, > 1.0 = faster)
/// @param config Phase vocoder configuration
/// @return Time-stretched spectrogram
/// @throws SonareException if spec is empty or rate <= 0
Spectrogram phase_vocoder(const Spectrogram& spec, float rate,
                          const PhaseVocoderConfig& config = PhaseVocoderConfig());

/// @brief Phase vocoder time-stretching with identity phase locking.
/// @details Same magnitude/instantaneous-frequency path as phase_vocoder(), but
///          synthesis phases are accumulated only at spectral peaks. Non-peak bins
///          are rigidly locked to the peak of their region of influence, preserving
///          the intra-frame phase relationship from the analysis frame. This reduces
///          inter-bin phase incoherence ("phasiness"). See Laroche & Dolson (1999).
/// @param spec Input spectrogram (must have at least 2 frames)
/// @param rate Time stretch rate (< 1.0 = slower, > 1.0 = faster)
/// @param config Phase vocoder configuration
/// @return Time-stretched spectrogram
/// @throws SonareException if spec is empty or rate <= 0
Spectrogram phase_vocoder_phaselocked(const Spectrogram& spec, float rate,
                                      const PhaseVocoderConfig& config = PhaseVocoderConfig());

/// @brief Computes instantaneous frequency from phase difference.
/// @param phase Current phase values [n_bins]
/// @param prev_phase Previous phase values [n_bins]
/// @param n_bins Number of frequency bins
/// @param hop_length Hop length in samples
/// @param sample_rate Sample rate in Hz
/// @return Instantaneous frequency in Hz [n_bins]
std::vector<float> compute_instantaneous_frequency(const float* phase, const float* prev_phase,
                                                   int n_bins, int hop_length, int sample_rate);

/// @brief Chunked phase-vocoder prototype for Step 5 tempo-sync work.
/// @details Mono/fixed-rate streaming-shaped prototype. It owns analysis STFT
///          frames, phase accumulator state and a synthesis OLA buffer so
///          process() can return stable prefix audio before finalize().
///          reserve() plus process_into()/finalize_into() provides a caller-owned
///          output path that is allocation-free after reservation; process() and
///          finalize() still return owning Audio chunks for offline callers.
class StreamingPhaseVocoder {
 public:
  explicit StreamingPhaseVocoder(StreamingPhaseVocoderConfig config = {});

  void reset();
  void push(const float* samples, size_t count);
  void push(const Audio& audio);
  Audio process(const float* samples, size_t count, float rate);
  Audio process(const Audio& audio, float rate);
  size_t process_into(const float* samples, size_t count, float rate, float* out,
                      size_t out_capacity);
  Audio finalize(float rate);
  size_t finalize_into(float rate, float* out, size_t out_capacity);
  Audio finish(float rate);
  void reserve(size_t max_input_samples, size_t max_output_samples);

  size_t pending_input_samples() const noexcept { return input_.size(); }
  int latency_samples() const noexcept;
  const StreamingPhaseVocoderConfig& config() const noexcept { return config_; }

 private:
  void bind_rate(float rate);
  void ensure_stream_state();
  void analyze_available_frames(bool final);
  void synthesize_available_frames(bool final);
  void synthesize_output_frame(int t_out, float rate);
  Audio drain_available(bool final);
  size_t drain_into(bool final, float* out, size_t out_capacity);
  void compact_buffers();
  float normalized_output_sample(size_t user_sample) const noexcept;
  const std::complex<float>& analysis_frame_at(int frame, int bin) const noexcept;

  StreamingPhaseVocoderConfig config_;
  std::vector<float> input_;
  size_t input_base_sample_ = 0;
  size_t ola_base_sample_ = 0;
  size_t emitted_output_samples_ = 0;
  float active_rate_ = 0.0f;
  bool finalized_ = false;

  std::unique_ptr<FFT> fft_;
  std::vector<float> analysis_window_;
  std::vector<float> synthesis_window_;
  std::vector<float> window_product_;
  std::vector<float> frame_;
  std::vector<std::complex<float>> frame_spectrum_;
  std::vector<std::complex<float>> analysis_frames_;
  std::vector<double> phase_acc_;
  std::vector<float> mag_;
  std::vector<float> ana_phase_;
  std::vector<float> inst_freq_;
  std::vector<int> peaks_;
  std::vector<int> nearest_peak_;
  std::vector<float> ola_output_;
  std::vector<float> ola_window_sum_;
  int analysis_frame_base_ = 0;
  int next_analysis_frame_ = 0;
  int next_output_frame_ = 0;
};

}  // namespace sonare
