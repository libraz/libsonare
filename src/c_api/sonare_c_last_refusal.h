#pragma once

/// @file sonare_c_last_refusal.h
/// @brief Internal: the structured parameter refusal behind the calling thread's last C-ABI error.
/// @details The flat message (sonare_last_error_message) is what C and Python read, in the core's
///          own subject spelling. A binding that spells its request keys differently re-renders
///          the refusal from its subjects instead of parsing the message. Not exported.

#include <sonare/sonare_c.h>

#include <cstddef>

#include "util/exception.h"

namespace sonare_c_detail {

/// @brief The refusal the last failing C-ABI call on this thread recorded, or nullptr when that
///        failure was not a SonareParameterRefusal. Cleared together with the last error.
const sonare::SonareParameterRefusal* last_parameter_refusal() noexcept;

/// @brief Backs sonare_last_refusal_subject_count: the refusal's subjects plus a named successor.
size_t last_refusal_subject_count() noexcept;

/// @brief Backs sonare_last_refusal_subject; NULL past the count.
const char* last_refusal_subject(size_t index) noexcept;

/// @brief @p refusal with its subjects replaced by @p subjects, in order, and a predicate's domain
///        divided by @p scales[0] (C units per caller unit; NULL means 1).
/// @throws SonareParameterRefusal(InvalidParameter) on a count other than the refusal's subjects
///         plus a named successor, a NULL subject, or a scale that is not finite and positive.
sonare::SonareParameterRefusal respelled_refusal(const sonare::SonareParameterRefusal& refusal,
                                                 const char* const* subjects, const double* scales,
                                                 size_t subject_count);

/// @brief Backs sonare_last_refusal_render. Records no error.
SonareError render_last_refusal(const char* const* subjects, const double* scales,
                                size_t subject_count, char** out_text) noexcept;

}  // namespace sonare_c_detail
