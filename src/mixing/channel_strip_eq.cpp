#include "mixing/channel_strip_eq.h"

#include "mixing/channel_strip.h"
#include "util/exception.h"

namespace sonare::mixing {

namespace {

using mastering::eq::EqBand;
using mastering::eq::EqBandType;
using mastering::eq::ParametricEq;

// Missing slots read as a default-constructed (disabled, identity) band, which
// is what clear_band leaves behind -- so a slot absent from both specs compares
// equal and is left alone.
EqBand slot_value(const api::StripEq& spec, size_t index) {
  return index < spec.bands.size() ? spec.bands[index] : EqBand{};
}

}  // namespace

void apply_eq(ParametricEq& eq, std::atomic<bool>& enabled, const api::StripEq& next,
             const api::StripEq* previous) {
  for (size_t index = 0; index < ParametricEq::kMaxBands; ++index) {
    if (previous != nullptr && slot_value(next, index) == slot_value(*previous, index)) {
      continue;
    }
    if (index < next.bands.size()) {
      eq.set_band(index, next.bands[index]);
    } else {
      eq.clear_band(index);
    }
  }
  enabled.store(next.enabled, std::memory_order_relaxed);
}

void apply_strip_eq(ChannelStrip& strip, const api::StripEq& next, const api::StripEq* previous) {
  apply_eq(strip.eq(), strip.eq_enabled_, next, previous);
}

void validate_eq(const api::StripEq& eq) {
  if (eq.bands.size() > ParametricEq::kMaxBands) {
    throw SonareException(ErrorCode::InvalidFormat,
                          "strip EQ has more bands than the parametric stage supports");
  }
  for (const EqBand& band : eq.bands) {
    if (band.type == EqBandType::TiltShelf || band.type == EqBandType::FlatTilt) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "strip EQ band type is not supported by the parametric stage");
    }
  }
}

}  // namespace sonare::mixing
