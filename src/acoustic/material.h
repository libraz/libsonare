#pragma once

/// @file material.h
/// @brief Frequency-dependent surface materials (octave-band absorption and
///        scattering) and building-material presets for room acoustics.

#include <cstddef>
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
///
/// Host-facing convenience: nothing inside the library calls this (room
/// morphing interpolates acoustic metrics, not materials). It exists so hosts
/// can author in-between wall treatments from two presets; the MIN-truncation
/// rule above is pinned by tests and is part of the public contract.
Material mix_materials(const Material& a, const Material& b, float t);

/// @brief Shared octave-band reconciliation rule for a set of wall/face materials.
///
/// Both the image-source method (specular reflection product) and the late-tail
/// RT60 model must agree on how many octave bands a multi-material room has and
/// how to read a coefficient past a given material's length. To keep the two
/// paths consistent this is the single source of truth:
///   * reconciliation rule: the MAXIMUM non-empty band count across the
///     materials (so no material's frequency detail is dropped);
///   * padding policy: a coefficient requested past a material's length reuses
///     that material's LAST coefficient (an empty material reads as @p empty_value).
/// Returns at least 1 (a rigid/all-empty room collapses to a single band).
template <typename MaterialRange>
std::size_t reconcile_band_count(const MaterialRange& materials) {
  std::size_t bands = 0;
  for (const Material& m : materials) {
    if (m.absorption.size() > bands) bands = m.absorption.size();
  }
  return bands == 0 ? 1u : bands;
}

/// @brief Absorption coefficient of @p material at octave band @p band under the
///        shared repeat-last padding policy (see `reconcile_band_count`).
///
/// An empty material returns @p empty_value (0 = rigid by default); otherwise
/// bands at or past the material's length reuse its last coefficient.
float material_alpha_at(const Material& material, std::size_t band, float empty_value = 0.0f);

}  // namespace sonare::acoustic
