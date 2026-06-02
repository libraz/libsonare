#pragma once

/// @file room_types.h
/// @brief Shared geometric room types and constants for the room-acoustics
///        simulation module (5th reverb engine).
///
/// This header holds only zero-dependency, WASM-safe POD and small inline
/// helpers shared across the acoustic subsystem (room model, image source,
/// RIR synthesis) and the room estimator. Larger types (ShoeboxRoom,
/// PolyhedralRoom, Material, RirSynthConfig) live in room_model.h.

namespace sonare {

/// @brief Axis-aligned room dimensions in metres.
///
/// Shared by acoustic synthesis (sonare::acoustic) and blind room estimation
/// (sonare::RoomEstimate), so geometry round-trips through one definition.
struct RoomDimensions {
  float length = 0.0f;  ///< x extent (m)
  float width = 0.0f;   ///< y extent (m)
  float height = 0.0f;  ///< z extent (m)
};

namespace acoustic {

/// @brief Octave-band count for material absorption/scattering and synthesized
///        decay tails.
///
/// Kept in lockstep with `AcousticConfig::n_octave_bands` (default 6) so the
/// estimator's per-band output feeds synthesis material bands without
/// resampling. A static check in the unit tests guards this against drift.
inline constexpr int kDefaultOctaveBands = 6;

/// @brief Interior volume of an axis-aligned shoebox room (m^3).
float room_volume(const RoomDimensions& dims) noexcept;

/// @brief Total surface area of an axis-aligned shoebox room (m^2).
float room_surface_area(const RoomDimensions& dims) noexcept;

}  // namespace acoustic
}  // namespace sonare
