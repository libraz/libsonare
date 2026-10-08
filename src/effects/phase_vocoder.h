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
#include "util/time_map.h"

namespace sonare {

/// @brief Configuration for phase vocoder.
struct PhaseVocoderConfig {
  /// Analysis/synthesis hop. Zero inherits the input Spectrogram hop.
  int hop_length = 0;
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

/// @brief Centered Hann STFT of @p audio holding the frame pair phase_vocoder() needs.
/// @details Non-empty input shorter than one hop is zero-extended to a second
///          analysis frame; resynthesize with to_audio(length) to trim it again.
/// @throws SonareException if @p audio is empty or the geometry is invalid.
Spectrogram phase_vocoder_analysis(const Audio& audio, int n_fft, int hop_length);

/// @brief Computes instantaneous frequency from phase difference.
/// @param phase Current phase values [n_bins]
/// @param prev_phase Previous phase values [n_bins]
/// @param n_bins Number of frequency bins
/// @param hop_length Hop length in samples
/// @param sample_rate Sample rate in Hz
/// @return Instantaneous frequency in Hz [n_bins]
std::vector<float> compute_instantaneous_frequency(const float* phase, const float* prev_phase,
                                                   int n_bins, int hop_length, int sample_rate);

/// @brief Frame-by-frame phase-vocoder synthesis shared by the offline, streaming
///        and multichannel tempo-sync paths.
/// @details The constructor validates the resynthesis geometry, window pair
///          included. Each next_frame() emits one output frame's synthesis phases
///          and only then applies the analysis transition leading out of it, so at
///          rate 1 output frame t carries the analysis phase of input frame t.
class PhaseVocoderSynthesizer {
 public:
  PhaseVocoderSynthesizer(int n_fft, int hop_length, int sample_rate, WindowType window,
                          int win_length, bool phase_lock);

  /// @brief Makes the next frame the first of a new stream.
  void reset() noexcept { first_frame_ = true; }

  /// @brief Synthesizes one output frame interpolated at @p frac between two
  ///        consecutive analysis frames of n_fft/2 + 1 bins each.
  void next_frame(const std::complex<float>* frame0, const std::complex<float>* frame1, float frac);

  /// @brief Clamped analysis frame pair (t_in, t_in + 1) and interpolation weight
  ///        for output frame @p t_out over @p n_frames_in >= 2 analysis frames.
  static void locate_input_frame(const TimeStretchMap& map, int t_out, int n_frames_in, int* t_in,
                                 float* frac);

  int n_bins() const noexcept { return n_bins_; }
  const float* magnitude() const noexcept { return magnitude_.data(); }
  const float* analysis_phase() const noexcept { return analysis_phase_.data(); }
  const double* synthesis_phase() const noexcept { return synthesis_phase_.data(); }

 private:
  int n_fft_;
  int n_bins_;
  int sample_rate_;
  bool phase_lock_;
  bool first_frame_ = true;
  double time_step_;
  std::vector<float> magnitude_;
  std::vector<float> analysis_phase_;
  std::vector<float> inst_freq_;
  std::vector<double> accumulator_;
  std::vector<double> synthesis_phase_;
  std::vector<int> peaks_;
  std::vector<int> nearest_peak_;
};

/// @brief Chunked phase-vocoder prototype for Step 5 tempo-sync work.
/// @details Mono/fixed-rate streaming-shaped prototype. It owns analysis STFT
///          frames, phase accumulator state and a synthesis OLA buffer so
///          process() can return stable prefix audio before finalize().
///          reserve() plus process_into()/finalize_into() provides a caller-owned
///          output path that is allocation-free after reservation; process() and
///          finalize() still return owning Audio chunks for offline callers.
///          Binding a rate rewrites the held map in place, so the scalar path
///          allocates nothing after reserve(); binding a profile with more
///          segments than the map's reserved capacity grows it once.
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
  /// @brief Ends the stream into @p out and returns how many samples it wrote.
  /// @details A tail longer than @p out_capacity is kept: call again, pushing
  ///   nothing in between, until it returns fewer samples than the capacity. The
  ///   stream resets once the whole tail has been delivered.
  size_t finalize_into(float rate, float* out, size_t out_capacity);
  Audio finish(float rate);
  void reserve(size_t max_input_samples, size_t max_output_samples);

  size_t pending_input_samples() const noexcept { return input_.size(); }
  int latency_samples() const noexcept;
  const StreamingPhaseVocoderConfig& config() const noexcept { return config_; }

 private:
  void bind_rate(float rate);
  void bind_map(const TimeStretchMap& map);
  void ensure_stream_state();
  void analyze_available_frames(bool final);
  void synthesize_available_frames(bool final);
  void synthesize_output_frame(int t_out);
  Audio drain_available(bool final);
  size_t drain_into(bool final, float* out, size_t out_capacity);
  size_t undelivered_samples(bool final) const;
  void compact_buffers();
  float normalized_output_sample(size_t user_sample) const noexcept;
  const std::complex<float>& analysis_frame_at(int frame, int bin) const noexcept;

  StreamingPhaseVocoderConfig config_;
  PhaseVocoderSynthesizer synth_;
  std::vector<float> input_;
  size_t input_base_sample_ = 0;
  size_t ola_base_sample_ = 0;
  size_t emitted_output_samples_ = 0;
  /// Bound on the first call and re-bindable only where it agrees with what has
  /// already been synthesized, because compact_buffers erases behind it. Held by
  /// value and sized at construction so binding assigns into storage that already
  /// exists: a profile with more segments than this one holds allocates once.
  TimeStretchMap active_map_{1.0f};
  bool map_bound_ = false;
  bool finalized_ = false;

  std::unique_ptr<FFT> fft_;
  std::vector<float> analysis_window_;
  std::vector<float> synthesis_window_;
  std::vector<float> window_product_;
  std::vector<float> frame_;
  std::vector<std::complex<float>> frame_spectrum_;
  std::vector<std::complex<float>> analysis_frames_;
  std::vector<float> ola_output_;
  std::vector<float> ola_window_sum_;
  int analysis_frame_base_ = 0;
  int next_analysis_frame_ = 0;
  int next_output_frame_ = 0;
};

}  // namespace sonare
