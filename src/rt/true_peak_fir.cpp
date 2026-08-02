#include "rt/true_peak_fir.h"

#include "util/exception.h"

namespace sonare::rt {
namespace {

constexpr int kTapsPerPhase = 12;
constexpr double kStandardKaiserBeta = 9.5;
constexpr double kTwoTimesKaiserBeta = 7.85726;

const PolyphaseFir& unity_fir() {
  static const PolyphaseFir fir;
  return fir;
}

const PolyphaseFir& two_times_fir() {
  static const auto fir = design_polyphase_lowpass(2, 2 * kTapsPerPhase, kTwoTimesKaiserBeta, true);
  return fir;
}

const PolyphaseFir& four_times_fir() {
  static const auto fir = design_polyphase_lowpass(4, 4 * kTapsPerPhase, kStandardKaiserBeta, true);
  return fir;
}

const PolyphaseFir& eight_times_fir() {
  static const auto fir = design_polyphase_lowpass(8, 8 * kTapsPerPhase, kStandardKaiserBeta, true);
  return fir;
}

const PolyphaseFir& sixteen_times_fir() {
  static const auto fir =
      design_polyphase_lowpass(16, 16 * kTapsPerPhase, kStandardKaiserBeta, true);
  return fir;
}

}  // namespace

bool is_supported_polyphase_oversample_factor(int factor) noexcept {
  return factor == 1 || factor == 2 || factor == 4 || factor == 8 || factor == 16;
}

const PolyphaseFir& true_peak_fir_for(int factor) {
  switch (factor) {
    case 1:
      return unity_fir();
    case 2:
      return two_times_fir();
    case 4:
      return four_times_fir();
    case 8:
      return eight_times_fir();
    case 16:
      return sixteen_times_fir();
    default:
      throw SonareException(ErrorCode::InvalidParameter,
                            "true-peak oversample factor must be one of 1, 2, 4, 8, or 16");
  }
}

}  // namespace sonare::rt
