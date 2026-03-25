/// @file fft.cpp
/// @brief Implementation of FFT wrapper.

#include "core/fft.h"

#include "util/exception.h"

extern "C" {
#include "kiss_fft.h"
#include "kiss_fftr.h"
}

namespace sonare {

struct FFT::Impl {
  kiss_fftr_cfg forward_cfg;
  kiss_fftr_cfg inverse_cfg;
  kiss_fft_cfg forward_complex_cfg;

  explicit Impl(int n_fft) {
    forward_cfg = kiss_fftr_alloc(n_fft, 0, nullptr, nullptr);
    inverse_cfg = kiss_fftr_alloc(n_fft, 1, nullptr, nullptr);
    forward_complex_cfg = kiss_fft_alloc(n_fft, 0, nullptr, nullptr);
    SONARE_CHECK_MSG(forward_cfg && inverse_cfg && forward_complex_cfg, ErrorCode::OutOfMemory,
                     "Failed to allocate KissFFT config");
  }

  ~Impl() {
    if (forward_cfg) kiss_fft_free(forward_cfg);
    if (inverse_cfg) kiss_fft_free(inverse_cfg);
    if (forward_complex_cfg) kiss_fft_free(forward_complex_cfg);
  }
};

FFT::FFT(int n_fft) : n_fft_(n_fft) {
  SONARE_CHECK_MSG(n_fft > 0, ErrorCode::InvalidParameter, "FFT size must be positive");
  impl_ = std::make_unique<Impl>(n_fft);
}

FFT::~FFT() = default;

FFT::FFT(FFT&&) noexcept = default;
FFT& FFT::operator=(FFT&&) noexcept = default;

void FFT::forward(const float* input, std::complex<float>* output) {
  SONARE_CHECK_MSG(input != nullptr && output != nullptr, ErrorCode::InvalidParameter,
                   "Null pointer passed to FFT::forward");
  kiss_fftr(impl_->forward_cfg, input, reinterpret_cast<kiss_fft_cpx*>(output));
}

void FFT::forward_complex(const std::complex<float>* input, std::complex<float>* output) {
  kiss_fft(impl_->forward_complex_cfg, reinterpret_cast<const kiss_fft_cpx*>(input),
           reinterpret_cast<kiss_fft_cpx*>(output));
}

void FFT::inverse(const std::complex<float>* input, float* output) {
  SONARE_CHECK_MSG(input != nullptr && output != nullptr, ErrorCode::InvalidParameter,
                   "Null pointer passed to FFT::inverse");
  kiss_fftri(impl_->inverse_cfg, reinterpret_cast<const kiss_fft_cpx*>(input), output);

  // KissFFT doesn't scale, so normalize manually
  float scale = 1.0f / n_fft_;
  for (int i = 0; i < n_fft_; ++i) {
    output[i] *= scale;
  }
}

}  // namespace sonare
