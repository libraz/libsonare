#pragma once

/// @file c_ordinal.h
/// @brief The one range check that turns a caller-supplied C-ABI ordinal into an enum.
/// @details An ordinal outside its documented set is refused with
///          SONARE_ERROR_INVALID_PARAMETER, never mapped to a default, dropped or
///          stored: a substituted default is indistinguishable from a caller who
///          asked for it. Every caller ordinal in src/c_api reaches its enum
///          through this header, so it holds the only cast from a caller value to
///          an enum. Header-only and dependency-free, because the WASM bindings
///          include synth_patch_common.h directly.

namespace sonare_c_detail {

/// Whether @p value lies in the closed ordinal range [@p first, @p last].
template <typename Int>
constexpr bool valid_c_enum(Int value, long long first, long long last) noexcept {
  return static_cast<long long>(value) >= first && static_cast<long long>(value) <= last;
}

/// Whether @p value names one of the @p count ordinals 0 .. count - 1.
template <typename Int>
constexpr bool valid_c_enum(Int value, long long count) noexcept {
  return valid_c_enum(value, 0, count - 1);
}

/// Converts @p value to @p E when it lies in [@p first, @p last]; the enumerator
/// is the ordinal minus @p bias (1 for a C field whose 0 means "keep").
/// Returns false and leaves @p out untouched otherwise.
template <typename E, typename Int>
[[nodiscard]] constexpr bool ordinal_to_enum(Int value, long long first, long long last, E* out,
                                             long long bias = 0) noexcept {
  if (!valid_c_enum(value, first, last)) return false;
  *out = static_cast<E>(static_cast<long long>(value) - bias);
  return true;
}

/// Maps an ordinal that a preceding valid_c_enum refusal already admitted onto
/// @p E, minus @p bias. For a site whose refusal must stay a separate statement
/// (it carries its own diagnostic); anywhere else use ordinal_to_enum.
template <typename E, typename Int>
constexpr E admitted_ordinal_to_enum(Int value, long long bias = 0) noexcept {
  return static_cast<E>(static_cast<long long>(value) - bias);
}

}  // namespace sonare_c_detail
