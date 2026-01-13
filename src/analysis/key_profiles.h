#pragma once

/// @file key_profiles.h
/// @brief Key profiles for key detection (Krumhansl-Schmuckler algorithm).

#include <array>

#include "util/types.h"

namespace sonare {

/// @brief Krumhansl-Schmuckler major key profile.
/// @details Correlation values for each pitch class in a major key.
/// Index 0 = tonic, 1 = minor second, ..., 11 = major seventh
constexpr std::array<float, 12> KS_MAJOR_PROFILE = {
    6.35f,  // C (tonic)
    2.23f,  // C#
    3.48f,  // D
    2.33f,  // D#
    4.38f,  // E (major third)
    4.09f,  // F
    2.52f,  // F#
    5.19f,  // G (fifth)
    2.39f,  // G#
    3.66f,  // A
    2.29f,  // A#
    2.88f   // B (major seventh)
};

/// @brief Krumhansl-Schmuckler minor key profile.
/// @details Correlation values for each pitch class in a minor key.
constexpr std::array<float, 12> KS_MINOR_PROFILE = {
    6.33f,  // C (tonic)
    2.68f,  // C#
    3.52f,  // D
    5.38f,  // D# (minor third)
    2.60f,  // E
    3.53f,  // F
    2.54f,  // F#
    4.75f,  // G (fifth)
    3.98f,  // G#
    2.69f,  // A
    3.34f,  // A#
    3.17f   // B
};

/// @brief Alternative Temperley major key profile.
constexpr std::array<float, 12> TEMPERLEY_MAJOR_PROFILE = {5.0f, 2.0f, 3.5f, 2.0f, 4.5f, 4.0f,
                                                           2.0f, 4.5f, 2.0f, 3.5f, 1.5f, 4.0f};

/// @brief Alternative Temperley minor key profile.
constexpr std::array<float, 12> TEMPERLEY_MINOR_PROFILE = {5.0f, 2.0f, 3.5f, 4.5f, 2.0f, 4.0f,
                                                           2.0f, 4.5f, 3.5f, 2.0f, 1.5f, 4.0f};

/// @brief Boosts for customizing key profiles.
struct KeyProfileBoosts {
  float tonic = 0.0f;    ///< Boost for tonic (root note)
  float third = 0.0f;    ///< Boost for third (major or minor)
  float fifth = 0.0f;    ///< Boost for fifth
  float seventh = 0.0f;  ///< Boost for seventh
};

/// @brief Key profile type selection.
enum class KeyProfileType {
  KrumhanslSchmuckler,  ///< Krumhansl-Schmuckler (default)
  Temperley             ///< Temperley
};

/// @brief Gets the major key profile for a given root.
/// @param root Root pitch class (C=0, C#=1, ..., B=11)
/// @param profile_type Profile type to use
/// @return Rotated profile starting at root
std::array<float, 12> get_major_profile(
    PitchClass root, KeyProfileType profile_type = KeyProfileType::KrumhanslSchmuckler);

/// @brief Gets the minor key profile for a given root.
/// @param root Root pitch class
/// @param profile_type Profile type to use
/// @return Rotated profile starting at root
std::array<float, 12> get_minor_profile(
    PitchClass root, KeyProfileType profile_type = KeyProfileType::KrumhanslSchmuckler);

/// @brief Gets a boosted major key profile.
/// @param root Root pitch class
/// @param boosts Profile boosts
/// @param profile_type Base profile type
/// @return Boosted and rotated profile
std::array<float, 12> get_boosted_major_profile(
    PitchClass root, const KeyProfileBoosts& boosts = KeyProfileBoosts(),
    KeyProfileType profile_type = KeyProfileType::KrumhanslSchmuckler);

/// @brief Gets a boosted minor key profile.
/// @param root Root pitch class
/// @param boosts Profile boosts
/// @param profile_type Base profile type
/// @return Boosted and rotated profile
std::array<float, 12> get_boosted_minor_profile(
    PitchClass root, const KeyProfileBoosts& boosts = KeyProfileBoosts(),
    KeyProfileType profile_type = KeyProfileType::KrumhanslSchmuckler);

/// @brief Normalizes a key profile to unit sum.
/// @param profile Profile to normalize
/// @return Normalized profile
std::array<float, 12> normalize_profile(const std::array<float, 12>& profile);

/// @brief Computes correlation between chroma vector and key profile.
/// @param chroma Chroma vector [12]
/// @param profile Key profile [12]
/// @return Pearson correlation coefficient [-1, 1]
float profile_correlation(const std::array<float, 12>& chroma,
                          const std::array<float, 12>& profile);

/// @brief Computes correlation between chroma vector and key profile.
/// @param chroma Chroma vector pointer [12]
/// @param profile Key profile [12]
/// @return Pearson correlation coefficient [-1, 1]
float profile_correlation(const float* chroma, const std::array<float, 12>& profile);

}  // namespace sonare
