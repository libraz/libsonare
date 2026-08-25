/// @file fft.cpp
/// @brief Implementation of FFT wrapper.

#include "core/fft.h"

#include <cstring>

#include "util/exception.h"

extern "C" {
#include "kiss_fft.h"
#include "kiss_fftr.h"
}

#if SONARE_HAVE_PFFFT
extern "C" {
#include "pffft.h"
}
#endif

namespace sonare {

namespace {

#if SONARE_HAVE_PFFFT

/// PFFFT only factors lengths of the form 2^a * 3^b * 5^c, and its SIMD kernels
/// additionally require the length to be a multiple of `SIMD_SZ * SIMD_SZ` for a
/// complex transform and twice that for a real one. `pffft_new_setup` asserts on
/// the multiple (rather than returning null), so the caller has to screen it
/// first; the factorization is left to the setup, which does return null.
/// `pffft_simd_size()` reports SIMD_SZ, 4 with SIMD and 1 without, so the same
/// test holds for a scalar build.
bool pffft_length_allowed(int n, bool real) {
  const int simd = pffft_simd_size();
  const int granularity = simd * simd * (real ? 2 : 1);
  return n > 0 && (n % granularity) == 0;
}

/// Owns one `pffft_aligned_malloc` block. PFFFT reads and writes its buffers
/// with aligned SIMD loads, so every pointer handed to it has to come from here
/// rather than from a `std::vector`, whose data is only guaranteed to carry the
/// default new alignment (and which a caller may have offset into anyway).
class AlignedFloats {
 public:
  AlignedFloats() = default;

  ~AlignedFloats() { reset(); }

  AlignedFloats(const AlignedFloats&) = delete;
  AlignedFloats& operator=(const AlignedFloats&) = delete;

  void allocate(size_t count, const char* what) {
    reset();
    data_ = static_cast<float*>(pffft_aligned_malloc(count * sizeof(float)));
    if (data_ == nullptr) {
      throw SonareException(ErrorCode::OutOfMemory, what);
    }
  }

  void reset() {
    if (data_ != nullptr) {
      pffft_aligned_free(data_);
      data_ = nullptr;
    }
  }

  float* get() const { return data_; }
  explicit operator bool() const { return data_ != nullptr; }

 private:
  float* data_ = nullptr;
};

/// Owns one `PFFFT_Setup`. A member rather than a raw handle so that a buffer
/// allocation failing after the setup succeeded still releases it: a
/// constructor that throws runs no destructor of its own, only those of the
/// members it had already built.
class PffftSetup {
 public:
  PffftSetup() = default;

  ~PffftSetup() {
    if (setup_ != nullptr) pffft_destroy_setup(setup_);
  }

  PffftSetup(const PffftSetup&) = delete;
  PffftSetup& operator=(const PffftSetup&) = delete;

  /// Creates the setup for @p n, leaving it null when PFFFT cannot serve that
  /// length -- the caller falls back to KissFFT rather than failing.
  void create(int n, pffft_transform_t transform) {
    if (!pffft_length_allowed(n, transform == PFFFT_REAL)) return;
    setup_ = pffft_new_setup(n, transform);
  }

  PFFFT_Setup* get() const { return setup_; }
  explicit operator bool() const { return setup_ != nullptr; }

 private:
  PFFFT_Setup* setup_ = nullptr;
};

#endif  // SONARE_HAVE_PFFFT

}  // namespace

/// @details Both backends are wired per transform kind rather than per instance:
/// a length PFFFT can factor for a complex transform is not necessarily one it
/// can factor for a real transform, so each of the three entry points picks its
/// own backend and only the configs that backend needs are allocated.
struct FFT::Impl {
#if SONARE_HAVE_PFFFT
  PffftSetup real_setup;
  PffftSetup complex_setup;
  AlignedFloats real_in;
  AlignedFloats real_out;
  AlignedFloats real_work;
  AlignedFloats complex_in;
  AlignedFloats complex_out;
  AlignedFloats complex_work;
#endif

  // KissFFT configs, allocated only for the transforms PFFFT does not serve.
  kiss_fftr_cfg forward_cfg = nullptr;
  kiss_fftr_cfg inverse_cfg = nullptr;
  kiss_fft_cfg forward_complex_cfg = nullptr;

  explicit Impl(int n) {
#if SONARE_HAVE_PFFFT
    real_setup.create(n, PFFFT_REAL);
    if (real_setup) {
      // `work` is passed explicitly rather than left null: PFFFT falls back to a
      // stack allocation of the same size, which is fine for an STFT frame but
      // not for the long transforms the convolution and match-EQ paths use.
      const size_t count = static_cast<size_t>(n);
      real_in.allocate(count, "Failed to allocate PFFFT real input buffer");
      real_out.allocate(count, "Failed to allocate PFFFT real output buffer");
      real_work.allocate(count, "Failed to allocate PFFFT real work buffer");
    }
    complex_setup.create(n, PFFFT_COMPLEX);
    if (complex_setup) {
      const size_t count = 2 * static_cast<size_t>(n);
      complex_in.allocate(count, "Failed to allocate PFFFT complex input buffer");
      complex_out.allocate(count, "Failed to allocate PFFFT complex output buffer");
      complex_work.allocate(count, "Failed to allocate PFFFT complex work buffer");
    }
    const bool need_kiss_real = !real_setup;
    const bool need_kiss_complex = !complex_setup;
#else
    const bool need_kiss_real = true;
    const bool need_kiss_complex = true;
#endif

    // The KissFFT handles are raw, and a throwing constructor runs no destructor
    // of its own, so a failure here releases whatever it already took before it
    // leaves.
    const char* failure = nullptr;
    if (need_kiss_real) {
      forward_cfg = kiss_fftr_alloc(n, 0, nullptr, nullptr);
      inverse_cfg = kiss_fftr_alloc(n, 1, nullptr, nullptr);
      if (!forward_cfg || !inverse_cfg) {
        failure = "Failed to allocate KissFFT real config";
      }
    }
    if (need_kiss_complex && failure == nullptr) {
      forward_complex_cfg = kiss_fft_alloc(n, 0, nullptr, nullptr);
      if (!forward_complex_cfg) {
        failure = "Failed to allocate KissFFT complex config";
      }
    }
    if (failure != nullptr) {
      release_kiss();
      throw SonareException(ErrorCode::OutOfMemory, failure);
    }
  }

  ~Impl() { release_kiss(); }

  void release_kiss() {
    if (forward_cfg) kiss_fft_free(forward_cfg);
    if (inverse_cfg) kiss_fft_free(inverse_cfg);
    if (forward_complex_cfg) kiss_fft_free(forward_complex_cfg);
    forward_cfg = nullptr;
    inverse_cfg = nullptr;
    forward_complex_cfg = nullptr;
  }
};

FFT::FFT(int n_fft) : n_fft_(n_fft) {
  SONARE_CHECK_MSG(n_fft >= 2 && (n_fft % 2) == 0, ErrorCode::InvalidParameter,
                   "FFT size must be an even integer greater than or equal to 2");
  impl_ = std::make_unique<Impl>(n_fft);
}

FFT::~FFT() = default;

FFT::FFT(FFT&&) noexcept = default;
FFT& FFT::operator=(FFT&&) noexcept = default;

void FFT::forward(const float* input, std::complex<float>* output) {
  SONARE_CHECK_MSG(input != nullptr && output != nullptr, ErrorCode::InvalidParameter,
                   "Null pointer passed to FFT::forward");
#if SONARE_HAVE_PFFFT
  if (impl_->real_setup) {
    const int n = n_fft_;
    const int half = n / 2;
    std::memcpy(impl_->real_in.get(), input, static_cast<size_t>(n) * sizeof(float));
    pffft_transform_ordered(impl_->real_setup.get(), impl_->real_in.get(), impl_->real_out.get(),
                            impl_->real_work.get(), PFFFT_FORWARD);
    // PFFFT's ordered real spectrum is n floats holding n/2 complex bins, with
    // the two purely-real terms packed into the first slot as DC + i*Nyquist.
    // Unpack to the n/2 + 1 canonical bins this class returns.
    const float* packed = impl_->real_out.get();
    output[0] = std::complex<float>(packed[0], 0.0f);
    output[half] = std::complex<float>(packed[1], 0.0f);
    for (int k = 1; k < half; ++k) {
      output[k] = std::complex<float>(packed[2 * k], packed[2 * k + 1]);
    }
    return;
  }
#endif
  kiss_fftr(impl_->forward_cfg, input, reinterpret_cast<kiss_fft_cpx*>(output));
}

void FFT::forward_complex(const std::complex<float>* input, std::complex<float>* output) {
  SONARE_CHECK_MSG(input != nullptr && output != nullptr, ErrorCode::InvalidParameter,
                   "Null pointer passed to FFT::forward_complex");
#if SONARE_HAVE_PFFFT
  if (impl_->complex_setup) {
    const size_t floats = 2 * static_cast<size_t>(n_fft_);
    // std::complex<float> is specified to have the layout of float[2] and is
    // trivially copyable, but it is not a trivial class, so the void* casts keep
    // GCC's -Wclass-memaccess from flagging the interleaved copies.
    std::memcpy(impl_->complex_in.get(), static_cast<const void*>(input), floats * sizeof(float));
    pffft_transform_ordered(impl_->complex_setup.get(), impl_->complex_in.get(),
                            impl_->complex_out.get(), impl_->complex_work.get(), PFFFT_FORWARD);
    std::memcpy(static_cast<void*>(output), impl_->complex_out.get(), floats * sizeof(float));
    return;
  }
#endif
  kiss_fft(impl_->forward_complex_cfg, reinterpret_cast<const kiss_fft_cpx*>(input),
           reinterpret_cast<kiss_fft_cpx*>(output));
}

void FFT::inverse(const std::complex<float>* input, float* output) {
  SONARE_CHECK_MSG(input != nullptr && output != nullptr, ErrorCode::InvalidParameter,
                   "Null pointer passed to FFT::inverse");
#if SONARE_HAVE_PFFFT
  if (impl_->real_setup) {
    const int n = n_fft_;
    const int half = n / 2;
    float* packed = impl_->real_in.get();
    // Mirror of the forward unpacking. The imaginary parts of bin 0 and the
    // Nyquist bin are dropped, matching kiss_fftri, which ignores them too.
    packed[0] = input[0].real();
    packed[1] = input[half].real();
    for (int k = 1; k < half; ++k) {
      packed[2 * k] = input[k].real();
      packed[2 * k + 1] = input[k].imag();
    }
    pffft_transform_ordered(impl_->real_setup.get(), packed, impl_->real_out.get(),
                            impl_->real_work.get(), PFFFT_BACKWARD);
    const float* result = impl_->real_out.get();
    const float scale = 1.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      output[i] = result[i] * scale;
    }
    return;
  }
#endif
  kiss_fftri(impl_->inverse_cfg, reinterpret_cast<const kiss_fft_cpx*>(input), output);

  // KissFFT doesn't scale, so normalize manually
  float scale = 1.0f / n_fft_;
  for (int i = 0; i < n_fft_; ++i) {
    output[i] *= scale;
  }
}

}  // namespace sonare
