#pragma once

/// @file image_source.h
/// @brief Image-source method for early reflections: Allen–Berkley analytic
///        mirror images for shoebox rooms and Borish generalized images (with
///        visibility rejection) for polyhedral rooms, plus sub-sample early-
///        reflection IR synthesis.

#include <vector>

#include "acoustic/geometry.h"
#include "acoustic/room_model.h"
#include "core/audio.h"

namespace sonare::acoustic {

/// @brief Speed of sound in air at ~20 °C (m/s).
inline constexpr float kSoundSpeed = 343.0f;

/// @brief Hard upper bound on the image-source reflection order.
///
/// Shoebox image-source cost grows ~ order^3 and polyhedral ~ faces^order, so an
/// unbounded user-supplied order is a memory/CPU exhaustion (DoS) vector — order
/// 200 already wants gigabytes. Perceptually useful early-reflection orders are
/// ~6–10; the late tail carries the dense diffuse energy beyond that. The image
/// generators clamp to this ceiling so no public surface (including WASM, which
/// bypasses the C-ABI validation) can drive an explosion.
inline constexpr int kMaxImageSourceOrder = 12;

/// @brief A mirrored (image) source contributing one early reflection.
///
/// `reflection` holds the per-octave-band pressure reflection product
/// (∏ β = ∏ sqrt(1-α) over the walls/faces the path bounces off); distance
/// attenuation is applied later by the synthesizer. `t` semantics: `distance`
/// is to the listener and is always > 0 for a usable image.
struct ImageSource {
  Vec3 position;                  ///< mirrored source position (room coords)
  int order = 0;                  ///< total reflection order (0 = direct)
  float distance = 0.0f;          ///< |position - listener| (m)
  std::vector<float> reflection;  ///< per-band ∏β, before distance attenuation
};

/// @brief Allen–Berkley analytic shoebox images with total order <= @p max_order.
///
/// Order 0 is the direct path. Per-band reflection is the product of wall
/// β = sqrt(1-α). Walls are read via `ShoeboxWall` indexing; if wall materials
/// are empty the walls are treated as rigid (β = 1, single band).
std::vector<ImageSource> shoebox_image_sources(const ShoeboxRoom& room,
                                               const SourceListener& placement, int max_order = 3);

/// @brief Borish generalized images for a polyhedral room with visibility
///        rejection.
///
/// Each candidate image is accepted only if every reflection point lies inside
/// its face and every path segment is unoccluded by other faces (checked with
/// the mesh's `VoxelGrid`). Order 0 (direct) is included when source and
/// listener are mutually visible. Cost grows ~ faces^order, so @p max_order
/// defaults low; higher orders on complex meshes are intentionally bounded,
/// with dense late energy delegated to the statistical (FDN) late tail.
std::vector<ImageSource> polyhedral_image_sources(const PolyhedralRoom& room,
                                                  const SourceListener& placement,
                                                  int max_order = 2);

/// @brief Configuration for early-reflection IR synthesis.
struct EarlyIrConfig {
  float sound_speed = kSoundSpeed;
  int fdl = 21;         ///< fractional-delay windowed-sinc length (odd; 1 = nearest sample)
  int band = -1;        ///< band to render; -1 = broadband = RMS of per-band β (the
                        ///< energy-correct collapse; exact for single-band/uniform materials,
                        ///< an approximation for frequency-dependent walls — use band>=0 for
                        ///< per-band fidelity)
  int max_samples = 0;  ///< 0 = size automatically from the farthest image
};

/// @brief Render the mono early-reflection impulse response.
///
/// Each image contributes a windowed-sinc kernel centred at
/// `distance / sound_speed` seconds (sub-sample accurate) scaled by
/// `reflection / (4π·distance)`. Polarity is preserved (absorptive walls give
/// positive reflection coefficients). The arrival time of each reflection lands
/// within ±1 sample of `round(distance/c·sr)`.
Audio synthesize_early_ir(const std::vector<ImageSource>& images, int sample_rate,
                          const EarlyIrConfig& config = {});

}  // namespace sonare::acoustic
