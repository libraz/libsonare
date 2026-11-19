#pragma once

/// @file sonare_c_last_refusal.h
/// @brief Internal: the structured parameter refusal behind the calling thread's last C-ABI error.
/// @details The flat message (sonare_last_error_message) is what C and Python read, in the core's
///          own subject spelling. A binding that spells its request keys differently re-renders
///          the refusal from its subjects instead of parsing the message. Not exported.

#include "util/exception.h"

namespace sonare_c_detail {

/// @brief The refusal the last failing C-ABI call on this thread recorded, or nullptr when that
///        failure was not a SonareParameterRefusal. Cleared together with the last error.
const sonare::SonareParameterRefusal* last_parameter_refusal() noexcept;

}  // namespace sonare_c_detail
