#pragma once

/// @file channel_strip_eq.h
/// @brief Diff-apply of a scene StripEq spec onto a live ParametricEq stage.

#include <atomic>

#include "mastering/eq/parametric.h"
#include "mixing/api/scene.h"

namespace sonare::mixing {

class ChannelStrip;

/// @brief Applies @p next onto @p eq, touching only the slots that changed.
/// @details With @p previous null, every slot in [0, ParametricEq::kMaxBands)
///          is driven straight from @p next: set_band where populated,
///          clear_band otherwise. With @p previous non-null, a slot whose
///          effective value (EqBand::operator==, missing treated as
///          default-constructed) agrees between @p next and @p previous is
///          left untouched, so an unrelated resend does not disturb that
///          slot's filter state; a slot dropped from @p next is cleared.
///          @p enabled always takes @p next.enabled, independent of the band
///          diff. Callers validate @p next first (see validate_eq); an
///          unvalidated band (e.g. TiltShelf/FlatTilt) throws from the
///          underlying ParametricEq::set_band.
void apply_eq(mastering::eq::ParametricEq& eq, std::atomic<bool>& enabled, const api::StripEq& next,
              const api::StripEq* previous);

/// @brief apply_eq() onto @p strip's own EQ stage and bypass flag.
void apply_strip_eq(ChannelStrip& strip, const api::StripEq& next, const api::StripEq* previous);

/// @brief Rejects a StripEq the parametric stage cannot host.
/// @throws SonareException(InvalidFormat) for more than
///         ParametricEq::kMaxBands bands, SonareException(InvalidParameter)
///         for a TiltShelf/FlatTilt band (composite types with no
///         single-section design; see design_eq_biquad).
void validate_eq(const api::StripEq& eq);

}  // namespace sonare::mixing
