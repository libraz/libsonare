/// @file fft.cpp
/// @brief Implementation of FFT wrapper.

#include "core/fft.h"

#include <stdexcept>

extern "C" {
#include "kiss_fft.h"
#include "kiss_fftr.h"
}

namespace sonare {

struct FFT::Impl {
  kiss_fftr_cfg forward_cfg;
  kiss_fftr_cfg inverse_cfg;

  explicit Impl(int n_fft) {
    forward_cfg = kiss_fftr_alloc(n_fft, 0, nullptr, nullptr);
    inverse_cfg = kiss_fftr_alloc(n_fft, 1, nullptr, nullptr);
    if (!forward_cfg || !inverse_cfg) {
      throw std::runtime_error("Failed to allocate KissFFT config");
    }
  }

  ~Impl() {
    if (forward_cfg) kiss_fft_free(forward_cfg);
    if (inverse_cfg) kiss_fft_free(inverse_cfg);
  }
};

FFT::FFT(int n_fft) : n_fft_(n_fft), impl_(std::make_unique<Impl>(n_fft)) {}

FFT::~FFT() = default;

FFT::FFT(FFT&&) noexcept = default;
FFT& FFT::operator=(FFT&&) noexcept = default;

void FFT::forward(const float* input, std::complex<float>* output) {
  kiss_fftr(impl_->forward_cfg, input, reinterpret_cast<kiss_fft_cpx*>(output));
}

void FFT::inverse(const std::complex<float>* input, float* output) {
  kiss_fftri(impl_->inverse_cfg, reinterpret_cast<const kiss_fft_cpx*>(input), output);

  // KissFFT doesn't scale, so normalize manually
  float scale = 1.0f / n_fft_;
  for (int i = 0; i < n_fft_; ++i) {
    output[i] *= scale;
  }
}

}  // namespace sonare
