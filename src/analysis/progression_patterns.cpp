#include "analysis/progression_patterns.h"

#include <algorithm>

namespace sonare {

const std::vector<ProgressionPattern>& known_progression_patterns() {
  static const std::vector<ProgressionPattern> patterns = {
      {"royalRoad", {{0, 0}, {7, 0}, {9, 1}, {5, 0}}},
      {"komuro", {{9, 1}, {5, 0}, {7, 0}, {0, 0}}},
      {"canon", {{0, 0}, {7, 0}, {9, 1}, {4, 1}, {5, 0}, {0, 0}, {5, 0}, {7, 0}}},
      {"justTheTwoOfUs", {{5, 0}, {4, 1}, {9, 1}}},
      {"basic145", {{0, 0}, {5, 0}, {7, 0}, {0, 0}}},
      {"blues12",
       {{0, 0},
        {0, 0},
        {0, 0},
        {0, 0},
        {5, 0},
        {5, 0},
        {0, 0},
        {0, 0},
        {7, 0},
        {5, 0},
        {0, 0},
        {7, 0}}},
      {"axis", {{9, 1}, {5, 0}, {0, 0}, {7, 0}}},
      {"fifties", {{0, 0}, {9, 1}, {5, 0}, {7, 0}}},
      {"sensitive", {{0, 0}, {7, 0}, {9, 1}, {4, 1}}},
  };
  return patterns;
}

std::array<std::pair<int, int>, 7> diatonic_triads(bool minor_key) {
  if (minor_key) {
    return {{
        {0, 1},   // i
        {2, 2},   // ii°
        {3, 0},   // III
        {5, 1},   // iv
        {7, 0},   // V
        {8, 0},   // VI
        {10, 0},  // VII
    }};
  }
  return {{
      {0, 0},   // I
      {2, 1},   // ii
      {4, 1},   // iii
      {5, 0},   // IV
      {7, 0},   // V
      {9, 1},   // vi
      {11, 2},  // vii°
  }};
}

float chord_similarity_score(int root, int quality, int expected_root, int expected_quality) {
  if (root == expected_root && quality == expected_quality) {
    return 1.0f;
  }
  if (root == expected_root) {
    return 0.6f;
  }

  int root_diff = std::abs(root - expected_root);
  if (root_diff > 6) {
    root_diff = 12 - root_diff;
  }

  float similarity = 0.0f;
  if (root_diff == 7 || root_diff == 5) {
    similarity = 0.3f;
  } else if (root_diff == 3 || root_diff == 4) {
    similarity = 0.25f;
  } else if (root_diff == 2) {
    similarity = 0.15f;
  } else if (root_diff == 1) {
    similarity = 0.2f;
  }

  if (quality == expected_quality) {
    similarity += 0.1f;
  }
  return similarity;
}

}  // namespace sonare
