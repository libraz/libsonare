#pragma once

/// @file tail_utils.h
/// @brief Shared serial/parallel processor-tail aggregation rules.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "rt/processor_base.h"
#include "util/numeric_validation.h"

namespace sonare::mixing {

enum class TailTopology : uint8_t {
  kSerial,
  kParallel,
};

/// Combines two non-negative tail lengths. Serial processors extend one
/// another, while parallel branches/merges need only the longest branch.
inline int combine_tail_samples(int first, int second, TailTopology topology) noexcept {
  first = std::max(0, first);
  second = std::max(0, second);
  if (topology == TailTopology::kParallel) return std::max(first, second);
  return numeric::saturating_add(first, second);
}

inline int processor_chain_tail_samples(
    const std::vector<std::unique_ptr<rt::ProcessorBase>>& processors) noexcept {
  int total = 0;
  for (const auto& processor : processors) {
    total = combine_tail_samples(total, processor->tail_samples(), TailTopology::kSerial);
  }
  return total;
}

}  // namespace sonare::mixing
