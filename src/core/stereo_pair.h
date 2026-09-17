#pragma once

/// @file stereo_pair.h
/// @brief The precondition every two-channel entry point shares.
///
/// An Audio models one channel, so a stereo operation takes two of them and has
/// to agree they describe the same recording before it may treat them as a pair.
/// The check lived as an identical private copy in each repair translation unit;
/// it sits here so a new stereo entry inherits the same refusals and the same
/// wording rather than restating them.

#include "core/audio.h"

namespace sonare {

/// @brief Refuses a pair of channels that cannot be processed together.
/// @details Rejects an empty channel, a length mismatch, and a sample-rate
///          mismatch. Sample values are not inspected: a non-finite sample is
///          each operation's own business, and several deliberately pass one
///          through.
/// @throws SonareException with ErrorCode::InvalidParameter.
void require_stereo_pair(const Audio& left, const Audio& right);

}  // namespace sonare
