#pragma once

#include <string>

#include "mastering/eq/parametric.h"
#include "mixing/channel_strip_eq.h"
#include "util/exception.h"

namespace sonare::c_api {

/// Validate one engine strip EQ band for its sample rate and keep DSP refusals
/// in the field paths of the band JSON argument.
inline void validate_engine_eq_band_argument(const mastering::eq::EqBand& band,
                                             double sample_rate) {
  try {
    mixing::validate_eq(mixing::api::StripEq{true, {band}});
    (void)mastering::eq::design_eq_biquad(band, sample_rate);
  } catch (const SonareParameterRefusal& refusal) {
    throw refusal.respelled(
        refusal.code(),
        [](const std::string& subject) {
          if (subject == "frequency_hz") return std::string("band.frequencyHz");
          if (subject == "q") return std::string("band.q");
          return subject;
        },
        true);
  }
}

}  // namespace sonare::c_api
