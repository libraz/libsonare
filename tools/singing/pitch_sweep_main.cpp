/// @file pitch_sweep_main.cpp
/// @brief Fits the singing compensation table over the grid given on the
///        command line: `--force f...` and `--pitch hz...`, one row of tensions
///        per force on stdout. Run by tools/singing/fit_pitch_compensation.py.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "singing/pitch_fit.h"

int main(int argc, char** argv) {
  std::vector<float> forces;
  std::vector<float> pitches;
  std::vector<float>* into = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--force") == 0) {
      into = &forces;
    } else if (std::strcmp(argv[i], "--pitch") == 0) {
      into = &pitches;
    } else if (into != nullptr) {
      into->push_back(std::strtof(argv[i], nullptr));
    } else {
      std::fprintf(stderr, "usage: %s --force f... --pitch hz...\n", argv[0]);
      return 2;
    }
  }
  if (forces.empty() || pitches.empty()) {
    std::fprintf(stderr, "usage: %s --force f... --pitch hz...\n", argv[0]);
    return 2;
  }
  int failures = 0;
  // Each cell warm-starts from the force below it, the first row from its left neighbour.
  std::vector<float> above(pitches.size(), -1.0f);
  for (const float force : forces) {
    float left = 0.55f;
    for (size_t p = 0; p < pitches.size(); ++p) {
      const float guess = above[p] > 0.0f ? above[p] : left;
      const float t = sonare::tools::singing::fit_tension(pitches[p], force, guess);
      if (t < 0.0f) {
        std::fprintf(stderr, "no phonating tension for %g Hz at force %g\n", pitches[p], force);
        ++failures;
      } else {
        left = t;
        above[p] = t;
      }
      std::printf("%s%.9g", p == 0 ? "" : " ", t);
    }
    std::printf("\n");
  }
  return failures == 0 ? 0 : 1;
}
