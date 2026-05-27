#pragma once

/// @file scoped_no_denormals.h
/// @brief Scoped flush-to-zero/denormals-are-zero guard for realtime DSP blocks.

#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#elif defined(__aarch64__)
#include <cstdint>
#endif

namespace sonare::rt {

class ScopedNoDenormals {
 public:
  ScopedNoDenormals() noexcept {
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86)
    previous_mxcsr_ = _mm_getcsr();
    _mm_setcsr(previous_mxcsr_ | kFlushToZero | kDenormalsAreZero);
#elif defined(__aarch64__)
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(previous_fpcr_));
    const std::uint64_t fpcr = previous_fpcr_ | kFlushToZero;
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
  }

  ~ScopedNoDenormals() noexcept {
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86)
    _mm_setcsr(previous_mxcsr_);
#elif defined(__aarch64__)
    __asm__ __volatile__("msr fpcr, %0" : : "r"(previous_fpcr_));
#endif
  }

  ScopedNoDenormals(const ScopedNoDenormals&) = delete;
  ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

 private:
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86)
  static constexpr unsigned int kFlushToZero = 0x8000;
  static constexpr unsigned int kDenormalsAreZero = 0x0040;
  unsigned int previous_mxcsr_ = 0;
#elif defined(__aarch64__)
  // FPCR bit 24 (FZ): flush-to-zero mode for ARMv8 single/double precision.
  static constexpr std::uint64_t kFlushToZero = 1u << 24;
  std::uint64_t previous_fpcr_ = 0;
#endif
};

}  // namespace sonare::rt
