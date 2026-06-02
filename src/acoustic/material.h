#pragma once

/// @file material.h
/// @brief Frequency-dependent surface materials (octave-band absorption and
///        scattering) and building-material presets for room acoustics.

#include <vector>

#include "acoustic/room_types.h"

namespace sonare::acoustic {

/// @brief Per-octave-band surface material.
///
/// `absorption` and `scattering` hold one coefficient in [0,1] per octave band
/// (nominal 125 / 250 / 500 / 1k / 2k / 4k Hz for the default 6 bands), matching
/// `kDefaultOctaveBands` and the analyzer's band split. Blind room estimation
/// maps estimated band absorption onto these vectors.
struct Material {
  std::vector<float> absorption;  ///< alpha(f), [0,1]
  std::vector<float> scattering;  ///< s(f), [0,1]
};

/// @brief Catalogue of building-material presets (octave-band coefficient tables).
enum class MaterialPreset {
  Concrete,  ///< sealed/painted concrete — low absorption, smooth
  Wood,      ///< wood panelling / parquet
  Curtain,   ///< heavy draped fabric — high HF absorption
  Carpet,    ///< heavy carpet on a hard floor
  Glass,     ///< large glass pane — slight LF absorption, smooth
};

/// @brief Build a material from a named preset (always `kDefaultOctaveBands` bands).
Material make_material(MaterialPreset preset);

/// @brief Uniform material with constant absorption/scattering across all bands.
Material uniform_material(float absorption, float scattering, int n_bands = kDefaultOctaveBands);

/// @brief Per-band linear blend of two materials.
///
/// @p t is clamped to [0,1]; t=0 returns @p a, t=1 returns @p b. Continuous in
/// @p t. The shortest absorption/scattering vector across both inputs governs
/// the result length, so the returned material always satisfies
/// absorption.size() == scattering.size().
Material mix_materials(const Material& a, const Material& b, float t);

}  // namespace sonare::acoustic
