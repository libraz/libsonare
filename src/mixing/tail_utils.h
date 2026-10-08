#pragma once

/// @file tail_utils.h
/// @brief Shared serial/parallel processor-tail aggregation rules.

#include <cstdint>
#include <memory>
#include <vector>

#include "rt/processor_base.h"
#include "rt/tail_budget.h"

namespace sonare::mixing {

enum class TailTopology : uint8_t {
  kSerial,
  kParallel,
};

/// Combines two non-negative tail lengths. Serial processors extend one
/// another, while parallel branches/merges need only the longest branch.
inline int combine_tail_samples(int first, int second, TailTopology topology) noexcept {
  rt::TailBudget tail = rt::TailBudget::reported(first);
  const rt::TailBudget other = rt::TailBudget::reported(second);
  return (topology == TailTopology::kParallel ? tail.alongside(other) : tail.then(other)).samples();
}

/// A serial chain's tail: each processor rings on after the one before it.
inline rt::TailBudget processor_chain_tail(
    const std::vector<std::unique_ptr<rt::ProcessorBase>>& processors) noexcept {
  rt::TailBudget total;
  for (const auto& processor : processors) {
    total.then(rt::TailBudget::reported(processor->tail_samples()));
  }
  return total;
}

}  // namespace sonare::mixing
