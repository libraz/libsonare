#include "analysis/key_profiles.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

#include "util/constants.h"
#include "util/math_utils.h"

namespace sonare {

using sonare::constants::kEpsilon;

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

/// @brief The four weights a modal profile is built from, in the order a listener
///        uses them to place a tonic.
/// @details These are a stated construction, not a measured hierarchy. Nothing
/// like the Krumhansl-Kessler probe-tone experiment exists here for the five
/// modes: the values were chosen so that the tonic dominates, its fifth and
/// third confirm it, the degree that separates the mode from its major or minor
/// neighbour stands above the remaining scale tones, and everything outside the
/// scale is suppressed. The *ordering* is what carries the meaning; the exact
/// numbers place each weight between its neighbours and are otherwise arbitrary.
/// See the note on @ref get_mode_profile for what that does and does not
/// license a caller to conclude.
namespace modal_weights {
/// The tonic. Highest, because a mode is a scale heard from a particular note,
/// and the tonic is the only thing separating a mode from its relative major.
constexpr float kTonic = 6.30f;
/// The fifth above the tonic, which is what confirms it as a tonic.
constexpr float kFifth = 5.20f;
/// The third, which fixes the mode's major or minor colour.
constexpr float kThird = 4.70f;
/// The characteristic degree: the one note that separates this mode from the
/// major or minor scale it otherwise shares. Weighted above the ordinary scale
/// tones because it is the whole evidence for the mode.
constexpr float kCharacteristic = 3.90f;
/// The remaining scale degrees.
constexpr float kScaleTone = 2.80f;
/// Anything outside the scale.
constexpr float kNonScaleTone = 0.35f;
}  // namespace modal_weights

/// @brief The scale and the three weighted degrees of one mode.
/// @details The scale is a fixed-size array rather than an initializer_list so
/// the spec owns its degrees; an initializer_list returned by value would leave
/// the caller reading a destroyed backing array.
struct ModalSpec {
  std::array<int, 7> scale{};
  bool defined = false;    ///< False for Major and Minor, which use published profiles
  int third = 0;           ///< Semitones to the third
  int fifth = 0;           ///< Semitones to the fifth; 6 for Locrian, which has none perfect
  int characteristic = 0;  ///< Semitones to the degree that names the mode
};

ModalSpec modal_spec(Mode mode) {
  switch (mode) {
    case Mode::Dorian:
      // Minor scale with a natural sixth; the sixth is what makes it not Aeolian.
      return {{0, 2, 3, 5, 7, 9, 10}, true, 3, 7, 9};
    case Mode::Phrygian:
      // Minor scale with a flat second.
      return {{0, 1, 3, 5, 7, 8, 10}, true, 3, 7, 1};
    case Mode::Lydian:
      // Major scale with a raised fourth.
      return {{0, 2, 4, 6, 7, 9, 11}, true, 4, 7, 6};
    case Mode::Mixolydian:
      // Major scale with a flat seventh.
      return {{0, 2, 4, 5, 7, 9, 10}, true, 4, 7, 10};
    case Mode::Locrian:
      // The only mode without a perfect fifth, which is also what names it, so
      // the diminished fifth takes both roles and gets the higher of the two.
      return {{0, 1, 3, 5, 6, 8, 10}, true, 3, 6, 6};
    case Mode::Major:
    case Mode::Minor:
    default:
      return {};
  }
}

std::array<float, 12> modal_base_profile(Mode mode) {
  std::array<float, 12> profile;
  profile.fill(modal_weights::kNonScaleTone);

  const ModalSpec spec = modal_spec(mode);
  if (!spec.defined) {
    // Major and minor are covered by the published profiles and never reach here.
    return profile;
  }

  for (int interval : spec.scale) {
    profile[interval] = modal_weights::kScaleTone;
  }
  profile[spec.characteristic] = modal_weights::kCharacteristic;
  profile[spec.third] = modal_weights::kThird;
  // The fifth is written after the third and the characteristic degree so that
  // Locrian, where all three can name the same degree, ends on the fifth's
  // weight rather than on whichever assignment happened to run last.
  profile[spec.fifth] = std::max(profile[spec.fifth], modal_weights::kFifth);
  profile[0] = modal_weights::kTonic;

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
  return pearson_correlation(chroma, profile.data(), profile.size());
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
