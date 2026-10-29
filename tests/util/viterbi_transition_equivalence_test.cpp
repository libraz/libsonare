#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "util/sequence.h"

namespace {

std::vector<int> reference_viterbi(const std::vector<float>& emissions, int states, int steps,
                                   const std::vector<float>& transition,
                                   const std::vector<float>& initial) {
  const float minus_inf = -std::numeric_limits<float>::infinity();
  std::vector<float> scores(static_cast<size_t>(states) * steps, minus_inf);
  std::vector<int> back(static_cast<size_t>(states) * steps, 0);
  for (int state = 0; state < states; ++state) {
    const float prior = initial.empty() ? -std::log(static_cast<float>(states))
                                        : std::log(std::max(initial[state], 1e-30f));
    scores[static_cast<size_t>(state) * steps] =
        prior + emissions[static_cast<size_t>(state) * steps];
  }
  for (int step = 1; step < steps; ++step) {
    for (int state = 0; state < states; ++state) {
      float best = minus_inf;
      int best_prev = 0;
      for (int previous = 0; previous < states; ++previous) {
        const float probability =
            std::max(transition[static_cast<size_t>(previous) * states + state], 1e-30f);
        const float score =
            scores[static_cast<size_t>(previous) * steps + step - 1] + std::log(probability);
        if (score > best) {
          best = score;
          best_prev = previous;
        }
      }
      scores[static_cast<size_t>(state) * steps + step] =
          best + emissions[static_cast<size_t>(state) * steps + step];
      back[static_cast<size_t>(state) * steps + step] = best_prev;
    }
  }
  std::vector<int> path(static_cast<size_t>(steps));
  int last = 0;
  for (int state = 1; state < states; ++state) {
    if (scores[static_cast<size_t>(state) * steps + steps - 1] >
        scores[static_cast<size_t>(last) * steps + steps - 1]) {
      last = state;
    }
  }
  path.back() = last;
  for (int step = steps - 1; step > 0; --step) {
    path[static_cast<size_t>(step - 1)] =
        back[static_cast<size_t>(path[static_cast<size_t>(step)]) * steps + step];
  }
  return path;
}

void compare_with_reference(const std::vector<float>& emissions, int states, int steps,
                            const std::vector<float>& transition,
                            const std::vector<float>& initial) {
  const auto* initial_ptr = initial.empty() ? nullptr : initial.data();
  const auto actual =
      sonare::viterbi(emissions.data(), states, steps, transition.data(), initial_ptr);
  REQUIRE(actual == reference_viterbi(emissions, states, steps, transition, initial));
}

}  // namespace

TEST_CASE("Viterbi preserves paths for floored transition values and nonuniform priors",
          "[util][viterbi]") {
  constexpr int kStates = 3;
  constexpr int kSteps = 6;
  const std::vector<float> emissions{
      -0.1f, -0.8f, -0.2f, -0.4f, -0.1f, -0.5f, -0.3f, -0.1f, -0.9f,
      -0.2f, -0.6f, -0.3f, -0.8f, -0.2f, -0.1f, -0.7f, -0.2f, -0.1f,
  };
  const std::vector<float> transition{0.7f, 0.0f, -1.0f, 0.1f, 0.6f, 0.3f, 0.2f, 0.4f, 0.8f};
  const std::vector<float> initial{0.05f, 0.8f, 0.15f};
  compare_with_reference(emissions, kStates, kSteps, transition, initial);
}

TEST_CASE("Viterbi retains first-predecessor tie ordering and ignores transitions for one step",
          "[util][viterbi]") {
  const std::vector<float> tied_emissions(12, 0.0f);
  const std::vector<float> tied_transition(9, 1.0f);
  const auto tied_path = sonare::viterbi(tied_emissions.data(), 3, 4, tied_transition.data());
  REQUIRE(tied_path == std::vector<int>{0, 0, 0, 0});
  REQUIRE(tied_path == reference_viterbi(tied_emissions, 3, 4, tied_transition, {}));

  const std::vector<float> one_step_emissions{-1.0f, -0.2f, -0.5f};
  const std::vector<float> unused_transition(9, std::numeric_limits<float>::quiet_NaN());
  REQUIRE(sonare::viterbi(one_step_emissions.data(), 3, 1, unused_transition.data()) ==
          std::vector<int>{1});
}

TEST_CASE("Viterbi transition precomputation preserves seeded reference paths", "[util][viterbi]") {
  uint32_t state = 0x3f4a2c17u;
  auto next = [&state] {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>((state >> 8) % 1000u) / 1000.0f;
  };
  for (const auto& [states, steps] : {std::pair{2, 2}, std::pair{4, 13}, std::pair{9, 31}}) {
    CAPTURE(states, steps);
    std::vector<float> emissions(static_cast<size_t>(states) * steps);
    std::vector<float> transition(static_cast<size_t>(states) * states);
    std::vector<float> initial(static_cast<size_t>(states));
    for (float& value : emissions) value = -next();
    for (float& value : transition) value = next();
    for (float& value : initial) value = next();
    compare_with_reference(emissions, states, steps, transition, initial);
  }
}
