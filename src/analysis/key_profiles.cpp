#include "analysis/key_profiles.h"

#include <cmath>
#include <initializer_list>
#include <numeric>

#include "util/constants.h"

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

const std::array<float, 12>& major_base_profile(KeyProfileType profile_type) {
  switch (profile_type) {
    case KeyProfileType::Temperley:
      return TEMPERLEY_MAJOR_PROFILE;
    case KeyProfileType::Shaath:
      return SHAATH_MAJOR_PROFILE;
    case KeyProfileType::FaraldoEDMT:
      return FARALDO_EDMT_MAJOR_PROFILE;
    case KeyProfileType::FaraldoEDMA:
      return FARALDO_EDMA_MAJOR_PROFILE;
    case KeyProfileType::FaraldoEDMM:
      return FARALDO_EDMM_MAJOR_PROFILE;
    case KeyProfileType::BellmanBudge:
      return BELLMAN_BUDGE_MAJOR_PROFILE;
    case KeyProfileType::KrumhanslSchmuckler:
    default:
      return KS_MAJOR_PROFILE;
  }
}

const std::array<float, 12>& minor_base_profile(KeyProfileType profile_type) {
  switch (profile_type) {
    case KeyProfileType::Temperley:
      return TEMPERLEY_MINOR_PROFILE;
    case KeyProfileType::Shaath:
      return SHAATH_MINOR_PROFILE;
    case KeyProfileType::FaraldoEDMT:
      return FARALDO_EDMT_MINOR_PROFILE;
    case KeyProfileType::FaraldoEDMA:
      return FARALDO_EDMA_MINOR_PROFILE;
    case KeyProfileType::FaraldoEDMM:
      return FARALDO_EDMM_MINOR_PROFILE;
    case KeyProfileType::BellmanBudge:
      return BELLMAN_BUDGE_MINOR_PROFILE;
    case KeyProfileType::KrumhanslSchmuckler:
    default:
      return KS_MINOR_PROFILE;
  }
}

std::array<float, 12> modal_base_profile(Mode mode) {
  std::array<float, 12> profile = {0.55f, 0.35f, 0.35f, 0.35f, 0.35f, 0.35f,
                                   0.35f, 0.35f, 0.35f, 0.35f, 0.35f, 0.35f};

  auto set_scale = [&profile](std::initializer_list<int> intervals) {
    for (int interval : intervals) {
      profile[interval] = 2.80f;
    }
  };

  switch (mode) {
    case Mode::Dorian:
      set_scale({0, 2, 3, 5, 7, 9, 10});
      profile[3] = 4.70f;
      profile[9] = 3.70f;
      break;
    case Mode::Phrygian:
      set_scale({0, 1, 3, 5, 7, 8, 10});
      profile[1] = 3.90f;
      profile[3] = 4.70f;
      break;
    case Mode::Lydian:
      set_scale({0, 2, 4, 6, 7, 9, 11});
      profile[4] = 4.70f;
      profile[6] = 3.90f;
      break;
    case Mode::Mixolydian:
      set_scale({0, 2, 4, 5, 7, 9, 10});
      profile[4] = 4.70f;
      profile[10] = 3.90f;
      break;
    case Mode::Locrian:
      set_scale({0, 1, 3, 5, 6, 8, 10});
      profile[3] = 4.40f;
      profile[6] = 4.90f;
      break;
    case Mode::Major:
    case Mode::Minor:
    default:
      return profile;
  }

  profile[0] = 6.30f;
  if (mode == Mode::Locrian) {
    profile[7] = 1.10f;
  } else {
    profile[7] = 5.20f;
  }

  return profile;
}

}  // namespace

std::array<float, 12> get_major_profile(PitchClass root, KeyProfileType profile_type) {
  return rotate_profile(major_base_profile(profile_type), static_cast<int>(root));
}

std::array<float, 12> get_minor_profile(PitchClass root, KeyProfileType profile_type) {
  return rotate_profile(minor_base_profile(profile_type), static_cast<int>(root));
}

std::array<float, 12> get_mode_profile(PitchClass root, Mode mode, KeyProfileType profile_type) {
  switch (mode) {
    case Mode::Major:
      return get_major_profile(root, profile_type);
    case Mode::Minor:
      return get_minor_profile(root, profile_type);
    case Mode::Dorian:
    case Mode::Phrygian:
    case Mode::Lydian:
    case Mode::Mixolydian:
    case Mode::Locrian:
      return rotate_profile(modal_base_profile(mode), static_cast<int>(root));
    default:
      return get_major_profile(root, profile_type);
  }
}

std::array<float, 12> get_boosted_major_profile(PitchClass root, const KeyProfileBoosts& boosts,
                                                KeyProfileType profile_type) {
  auto profile = get_major_profile(root, profile_type);

  int root_idx = static_cast<int>(root);

  // Apply multiplicative boosts at the appropriate intervals
  profile[root_idx] *= boosts.tonic;                // Tonic
  profile[(root_idx + 4) % 12] *= boosts.third;     // Major third
  profile[(root_idx + 7) % 12] *= boosts.fifth;     // Perfect fifth
  profile[(root_idx + 11) % 12] *= boosts.seventh;  // Major seventh

  return profile;
}

std::array<float, 12> get_boosted_minor_profile(PitchClass root, const KeyProfileBoosts& boosts,
                                                KeyProfileType profile_type) {
  auto profile = get_minor_profile(root, profile_type);

  int root_idx = static_cast<int>(root);

  // Apply multiplicative boosts at the appropriate intervals
  profile[root_idx] *= boosts.tonic;                // Tonic
  profile[(root_idx + 3) % 12] *= boosts.third;     // Minor third
  profile[(root_idx + 7) % 12] *= boosts.fifth;     // Perfect fifth
  profile[(root_idx + 10) % 12] *= boosts.seventh;  // Minor seventh

  return profile;
}

std::array<float, 12> get_boosted_mode_profile(PitchClass root, Mode mode,
                                               const KeyProfileBoosts& boosts,
                                               KeyProfileType profile_type) {
  if (mode == Mode::Major) {
    return get_boosted_major_profile(root, boosts, profile_type);
  }
  if (mode == Mode::Minor) {
    return get_boosted_minor_profile(root, boosts, profile_type);
  }

  auto profile = get_mode_profile(root, mode, profile_type);
  const int root_idx = static_cast<int>(root);

  profile[root_idx] *= boosts.tonic;
  switch (mode) {
    case Mode::Dorian:
    case Mode::Phrygian:
    case Mode::Locrian:
      profile[(root_idx + 3) % 12] *= boosts.third;
      break;
    case Mode::Lydian:
    case Mode::Mixolydian:
      profile[(root_idx + 4) % 12] *= boosts.third;
      break;
    default:
      break;
  }
  profile[(root_idx + (mode == Mode::Locrian ? 6 : 7)) % 12] *= boosts.fifth;
  profile[(root_idx + ((mode == Mode::Lydian || mode == Mode::Major) ? 11 : 10)) % 12] *=
      boosts.seventh;

  return profile;
}

std::array<float, 12> normalize_profile(const std::array<float, 12>& profile) {
  float sum = std::accumulate(profile.begin(), profile.end(), 0.0f);

  std::array<float, 12> normalized;
  if (sum > constants::kEpsilon) {
    for (int i = 0; i < 12; ++i) {
      normalized[i] = profile[i] / sum;
    }
  } else {
    normalized.fill(1.0f / constants::kSemitonesPerOctave);
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
  chroma_mean /= constants::kSemitonesPerOctave;
  profile_mean /= constants::kSemitonesPerOctave;

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

  if (denominator < constants::kEpsilon) {
    return 0.0f;
  }

  return numerator / denominator;
}

MajorMinorKeyMatch find_best_major_minor_key(const std::array<float, 12>& chroma,
                                             KeyProfileType profile_type) {
  MajorMinorKeyMatch best;
  best.correlation = -2.0f;

  for (int root = 0; root < 12; ++root) {
    const PitchClass pc = static_cast<PitchClass>(root);

    auto major_profile = normalize_profile(get_major_profile(pc, profile_type));
    const float major_corr = profile_correlation(chroma, major_profile);
    if (major_corr > best.correlation) {
      best.root = root;
      best.minor = false;
      best.correlation = major_corr;
    }

    auto minor_profile = normalize_profile(get_minor_profile(pc, profile_type));
    const float minor_corr = profile_correlation(chroma, minor_profile);
    if (minor_corr > best.correlation) {
      best.root = root;
      best.minor = true;
      best.correlation = minor_corr;
    }
  }

  return best;
}

}  // namespace sonare
