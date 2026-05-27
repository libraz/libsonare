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

/// @brief Sha'ath/KeyFinder-style major key profile.
/// @details Clean-room approximation from Sha'ath 2011 thesis figure 2.8. The thesis describes
/// the profile as a slight adjustment of Krumhansl with stronger tonic and final diatonic degree.
constexpr std::array<float, 12> SHAATH_MAJOR_PROFILE = {6.70f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
                                                        2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 3.45f};

/// @brief Sha'ath/KeyFinder-style minor key profile.
constexpr std::array<float, 12> SHAATH_MINOR_PROFILE = {6.75f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
                                                        2.54f, 4.75f, 3.98f, 2.69f, 4.00f, 3.17f};

/// @brief Bellman-Budge major key profile.
/// @details Values are published by the Humdrum keycor documentation and mirrored by music21.
constexpr std::array<float, 12> BELLMAN_BUDGE_MAJOR_PROFILE = {
    16.80f, 0.86f, 12.95f, 1.41f, 13.49f, 11.93f, 1.25f, 20.28f, 1.80f, 8.04f, 0.62f, 10.57f};

/// @brief Bellman-Budge minor key profile.
constexpr std::array<float, 12> BELLMAN_BUDGE_MINOR_PROFILE = {
    18.16f, 0.69f, 12.99f, 13.34f, 1.07f, 11.15f, 1.38f, 21.07f, 7.49f, 1.53f, 0.92f, 10.21f};

/// @brief Faraldo EDMT-style major profile.
/// @details Faraldo EDMT is described as using Temperley-style profiles in an EDM-oriented
/// pipeline; this keeps the profile selector explicit without copying AGPL implementation data.
constexpr std::array<float, 12> FARALDO_EDMT_MAJOR_PROFILE = TEMPERLEY_MAJOR_PROFILE;

/// @brief Faraldo EDMT-style minor profile.
constexpr std::array<float, 12> FARALDO_EDMT_MINOR_PROFILE = TEMPERLEY_MINOR_PROFILE;

/// @brief Faraldo EDMA-style major profile.
/// @details Clean-room EDM-oriented profile: tonic/fifth/major-third weighted, non-diatonic
/// tones suppressed. Intended as opt-in until real dataset validation is available.
constexpr std::array<float, 12> FARALDO_EDMA_MAJOR_PROFILE = {
    6.00f, 0.65f, 3.00f, 0.75f, 4.90f, 2.80f, 0.65f, 5.60f, 0.90f, 2.70f, 0.75f, 2.10f};

/// @brief Faraldo EDMA-style minor profile.
/// @details Keeps tonic/fifth dominant and allows EDM minor tracks with prominent natural
/// third energy by weighting both minor and major thirds.
constexpr std::array<float, 12> FARALDO_EDMA_MINOR_PROFILE = {
    6.20f, 0.65f, 2.40f, 4.90f, 4.10f, 2.70f, 0.65f, 5.80f, 2.90f, 0.85f, 3.70f, 1.30f};

/// @brief Faraldo EDMM-style major profile.
/// @details Minor-biased EDM variant. Opt-in only; it is not used by default genre auto mode.
constexpr std::array<float, 12> FARALDO_EDMM_MAJOR_PROFILE = {
    5.20f, 0.60f, 2.20f, 3.30f, 3.40f, 2.50f, 0.60f, 5.30f, 2.60f, 0.80f, 3.20f, 1.20f};

/// @brief Faraldo EDMM-style minor profile.
constexpr std::array<float, 12> FARALDO_EDMM_MINOR_PROFILE = {
    6.40f, 0.55f, 2.10f, 4.80f, 4.40f, 2.40f, 0.55f, 5.90f, 2.90f, 0.75f, 3.80f, 1.10f};

/// @brief Constants for key profile enhancement.
/// @details Multiplicative boost factors matching bpm-detector Python implementation.
namespace key_constants {
/// @brief Major profile: tonic boost factor.
constexpr float kMajorTonicBoost = 1.5f;
/// @brief Major profile: major third boost factor.
constexpr float kMajorThirdBoost = 1.3f;
/// @brief Major profile: perfect fifth boost factor.
constexpr float kMajorFifthBoost = 1.2f;

/// @brief Minor profile: tonic boost factor.
constexpr float kMinorTonicBoost = 1.7f;
/// @brief Minor profile: minor third boost factor.
constexpr float kMinorThirdBoost = 1.3f;
/// @brief Minor profile: perfect fifth boost factor.
constexpr float kMinorFifthBoost = 1.2f;
/// @brief Minor profile: minor seventh boost factor.
constexpr float kMinorSeventhBoost = 1.2f;
}  // namespace key_constants

/// @brief Multiplicative boosts for customizing key profiles.
/// @details Factors are multiplied with profile values (1.0 = no change).
struct KeyProfileBoosts {
  float tonic = 1.0f;    ///< Boost for tonic (root note)
  float third = 1.0f;    ///< Boost for third (major or minor)
  float fifth = 1.0f;    ///< Boost for fifth
  float seventh = 1.0f;  ///< Boost for seventh
};

/// @brief Key profile type selection.
enum class KeyProfileType {
  KrumhanslSchmuckler,  ///< Krumhansl-Schmuckler (default)
  Temperley,            ///< Temperley
  Shaath,               ///< Sha'ath/KeyFinder-style profile
  FaraldoEDMT,          ///< Faraldo EDMT-style profile
  FaraldoEDMA,          ///< Faraldo EDMA-style profile
  FaraldoEDMM,          ///< Faraldo EDMM-style profile
  BellmanBudge          ///< Bellman-Budge profile
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

/// @brief Gets a modal key profile for a given root.
/// @param root Root pitch class
/// @param mode Musical mode
/// @param profile_type Base major/minor profile family for Major/Minor modes
/// @return Rotated mode profile
std::array<float, 12> get_mode_profile(
    PitchClass root, Mode mode, KeyProfileType profile_type = KeyProfileType::KrumhanslSchmuckler);

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

/// @brief Gets a boosted modal key profile.
/// @param root Root pitch class
/// @param mode Musical mode
/// @param boosts Profile boosts
/// @param profile_type Base profile type
/// @return Boosted and rotated profile
std::array<float, 12> get_boosted_mode_profile(
    PitchClass root, Mode mode, const KeyProfileBoosts& boosts = KeyProfileBoosts(),
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

/// @brief Best major/minor key match for a chroma vector.
struct MajorMinorKeyMatch {
  int root = 0;           ///< Pitch class (0=C, ..., 11=B)
  bool minor = false;     ///< True for minor, false for major
  float correlation = 0;  ///< Raw profile correlation [-1, 1]
};

/// @brief Finds the best major/minor Krumhansl-Schmuckler-style key match.
/// @param chroma Normalized chroma vector [12].
/// @param profile_type Profile family to use.
/// @return Best root/mode and raw correlation.
MajorMinorKeyMatch find_best_major_minor_key(
    const std::array<float, 12>& chroma,
    KeyProfileType profile_type = KeyProfileType::KrumhanslSchmuckler);

}  // namespace sonare
