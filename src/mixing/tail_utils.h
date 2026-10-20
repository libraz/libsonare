#pragma once

/// @file tail_utils.h
/// @brief Shared serial/parallel processor-tail aggregation rules.

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
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

/// A chain's summed latency in Q8 samples.
inline int processor_chain_latency_q8(
    const std::vector<std::unique_ptr<rt::ProcessorBase>>& processors) noexcept {
  int total = 0;
  for (const auto& processor : processors) {
    total += processor->latency_samples_q8();
  }
  return total;
}

/// Adds the Q8 latency of the first @p count processors of @p processors to @p prefix.
/// Returns false when the running sum leaves the range of int.
inline bool accumulate_chain_latency_q8(
    const std::vector<std::unique_ptr<rt::ProcessorBase>>& processors, size_t count,
    int64_t& prefix) noexcept {
  for (size_t i = 0; i < count; ++i) {
    if (processors[i] != nullptr) {
      prefix += static_cast<int64_t>(processors[i]->latency_samples_q8());
      if (prefix < std::numeric_limits<int>::min() || prefix > std::numeric_limits<int>::max()) {
        return false;
      }
    }
  }
  return true;
}

/// Resolves a JSON-key parameter name to its id on @p processor, or -1 when the
/// processor is null, the key is unknown or the parameter is not realtime-safe.
inline int processor_parameter_id_for_key(const rt::ProcessorBase* processor,
                                          const std::string& key) noexcept {
  if (processor == nullptr) {
    return -1;
  }
  for (const auto& desc : processor->parameter_descriptors()) {
    if (desc.key == key) {
      return processor->parameter_is_realtime_safe(desc.id) ? static_cast<int>(desc.id) : -1;
    }
  }
  return -1;
}

}  // namespace sonare::mixing
