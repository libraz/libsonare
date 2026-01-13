#include "analysis/key_profiles.h"

#include <cmath>
#include <numeric>

namespace sonare {

namespace {

/// @brief Rotates a profile to start at a given root.
std::array<float, 12> rotate_profile(const std::array<float, 12>& profile, int semitones) {
  std::array<float, 12> rotated;
  for (int i = 0; i < 12; ++i) {
    int src = (i - semitones + 12) % 12;
    rotated[i] = profile[src];
  }
  return rotated;
}

}  // namespace

std::array<float, 12> get_major_profile(PitchClass root, KeyProfileType profile_type) {
  const auto& base_profile =
      (profile_type == KeyProfileType::Temperley) ? TEMPERLEY_MAJOR_PROFILE : KS_MAJOR_PROFILE;
  return rotate_profile(base_profile, static_cast<int>(root));
}

std::array<float, 12> get_minor_profile(PitchClass root, KeyProfileType profile_type) {
  const auto& base_profile =
      (profile_type == KeyProfileType::Temperley) ? TEMPERLEY_MINOR_PROFILE : KS_MINOR_PROFILE;
  return rotate_profile(base_profile, static_cast<int>(root));
}

std::array<float, 12> get_boosted_major_profile(PitchClass root, const KeyProfileBoosts& boosts,
                                                KeyProfileType profile_type) {
  auto profile = get_major_profile(root, profile_type);

  int root_idx = static_cast<int>(root);

  // Apply boosts at the appropriate intervals
  profile[root_idx] += boosts.tonic;                // Tonic
  profile[(root_idx + 4) % 12] += boosts.third;     // Major third
  profile[(root_idx + 7) % 12] += boosts.fifth;     // Perfect fifth
  profile[(root_idx + 11) % 12] += boosts.seventh;  // Major seventh

  return profile;
}

std::array<float, 12> get_boosted_minor_profile(PitchClass root, const KeyProfileBoosts& boosts,
                                                KeyProfileType profile_type) {
  auto profile = get_minor_profile(root, profile_type);

  int root_idx = static_cast<int>(root);

  // Apply boosts at the appropriate intervals
  profile[root_idx] += boosts.tonic;                // Tonic
  profile[(root_idx + 3) % 12] += boosts.third;     // Minor third
  profile[(root_idx + 7) % 12] += boosts.fifth;     // Perfect fifth
  profile[(root_idx + 10) % 12] += boosts.seventh;  // Minor seventh

  return profile;
}

std::array<float, 12> normalize_profile(const std::array<float, 12>& profile) {
  float sum = std::accumulate(profile.begin(), profile.end(), 0.0f);

  std::array<float, 12> normalized;
  if (sum > 1e-10f) {
    for (int i = 0; i < 12; ++i) {
      normalized[i] = profile[i] / sum;
    }
  } else {
    normalized.fill(1.0f / 12.0f);
  }

  return normalized;
}

float profile_correlation(const std::array<float, 12>& chroma,
                          const std::array<float, 12>& profile) {
  return profile_correlation(chroma.data(), profile);
}

float profile_correlation(const float* chroma, const std::array<float, 12>& profile) {
  // Compute means
  float chroma_mean = 0.0f;
  float profile_mean = 0.0f;
  for (int i = 0; i < 12; ++i) {
    chroma_mean += chroma[i];
    profile_mean += profile[i];
  }
  chroma_mean /= 12.0f;
  profile_mean /= 12.0f;

  // Compute Pearson correlation
  float numerator = 0.0f;
  float chroma_var = 0.0f;
  float profile_var = 0.0f;

  for (int i = 0; i < 12; ++i) {
    float chroma_diff = chroma[i] - chroma_mean;
    float profile_diff = profile[i] - profile_mean;

    numerator += chroma_diff * profile_diff;
    chroma_var += chroma_diff * chroma_diff;
    profile_var += profile_diff * profile_diff;
  }

  float denominator = std::sqrt(chroma_var * profile_var);

  if (denominator < 1e-10f) {
    return 0.0f;
  }

  return numerator / denominator;
}

}  // namespace sonare
