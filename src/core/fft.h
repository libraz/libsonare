#pragma once

/// @file fft.h
/// @brief FFT wrapper over the PFFFT and KissFFT backends.

#include <complex>
#include <memory>
#include <vector>

namespace sonare {

/// @brief Real-valued FFT processor.
/// @details Provides forward and inverse real FFT operations.
///
/// Backend: the SIMD PFFFT kernels are used for every transform length they can
/// factor, and the scalar KissFFT serves the rest, so an unusual @c n_fft still
/// transforms rather than failing. Which backend runs is an implementation
/// detail with no effect beyond floating-point rounding; it is chosen per
/// transform kind on that transform's first call, and an instance allocates
/// nothing for a kind it is never asked to perform. An @c ErrorCode::OutOfMemory
/// therefore surfaces from the transform that first needs the state, not from
/// the constructor. A realtime owner calls prepare() for the kinds it will use,
/// from wherever it sizes its other buffers, so that no first transform
/// allocates on the audio thread. In a @c noexcept caller that is a correctness
/// requirement rather than a latency one: the first transform of a kind can
/// throw, and an exception leaving a @c noexcept function terminates the
/// process. Builds configured with
/// `-DSONARE_USE_PFFFT=OFF`, which is the WebAssembly default, use KissFFT
/// throughout.
///
/// Thread Safety:
/// - Different instances can be used concurrently from different threads.
/// - A single instance must NOT be shared between threads without external
///   synchronization, as the backend's scratch buffers are written during
///   computation.
/// - For multi-threaded processing, create one FFT instance per thread.
class FFT {
 public:
  /// @brief Constructs FFT processor.
  /// @param n_fft FFT size (should be power of 2 for efficiency)
  /// @throws SonareException with ErrorCode::InvalidParameter if n_fft is not an
  ///         even integer greater than or equal to 2
  explicit FFT(int n_fft);

  ~FFT();

  // Non-copyable, movable
  FFT(const FFT&) = delete;
  FFT& operator=(const FFT&) = delete;
  FFT(FFT&&) noexcept;
  FFT& operator=(FFT&&) noexcept;

  /// @brief Builds the backend state for the given transform kinds up front.
  /// @details Idempotent, and a kind already built is left untouched. All three
  ///          kinds are named so that preparing one is a decision about the
  ///          other two rather than a silent omission.
  /// @param real_forward Prepare what forward() needs
  /// @param real_inverse Prepare what inverse() needs
  /// @param complex_forward Prepare what forward_complex() needs
  /// @throws SonareException with ErrorCode::OutOfMemory if allocation fails
  void prepare(bool real_forward, bool real_inverse, bool complex_forward);

  /// @brief Performs forward FFT (real to complex).
  /// @param input Input signal (size must equal n_fft)
  /// @param output Complex spectrum (size must equal n_bins)
  /// @throws SonareException with ErrorCode::InvalidParameter if input or output is null
  void forward(const float* input, std::complex<float>* output);

  /// @brief Performs forward complex-to-complex FFT.
  /// @param input Complex input signal (size must equal n_fft)
  /// @param output Complex spectrum (size must equal n_fft, all bins)
  void forward_complex(const std::complex<float>* input, std::complex<float>* output);

  /// @brief Performs inverse FFT (complex to real).
  /// @param input Complex spectrum (size must equal n_bins)
  /// @param output Output signal (size must equal n_fft)
  /// @throws SonareException with ErrorCode::InvalidParameter if input or output is null
  void inverse(const std::complex<float>* input, float* output);

  /// @brief Returns FFT size.
  int n_fft() const { return n_fft_; }

  /// @brief Returns number of frequency bins (n_fft/2 + 1).
  int n_bins() const { return n_fft_ / 2 + 1; }

 private:
  int n_fft_;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sonare
