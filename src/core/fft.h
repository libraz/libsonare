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
/// transform kind at construction. Builds configured with
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
  /// @throws SonareException with ErrorCode::OutOfMemory if allocation fails
  explicit FFT(int n_fft);

  ~FFT();

  // Non-copyable, movable
  FFT(const FFT&) = delete;
  FFT& operator=(const FFT&) = delete;
  FFT(FFT&&) noexcept;
  FFT& operator=(FFT&&) noexcept;

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
