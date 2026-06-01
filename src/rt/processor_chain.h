#pragma once

/// @file processor_chain.h
/// @brief Helpers for processor-chain level accounting.

#include <initializer_list>
#include <vector>

#include "rt/processor_base.h"
#include "util/exception.h"

namespace sonare::rt {

inline int total_latency_samples(std::initializer_list<const ProcessorBase*> processors) {
  int total = 0;
  for (const ProcessorBase* processor : processors) {
    if (processor == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "processor must not be null");
    }
    total += processor->latency_samples();
  }
  return total;
}

inline int total_latency_samples_q8(std::initializer_list<const ProcessorBase*> processors) {
  int total = 0;
  for (const ProcessorBase* processor : processors) {
    if (processor == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "processor must not be null");
    }
    total += processor->latency_samples_q8();
  }
  return total;
}

inline int total_latency_samples(const std::vector<const ProcessorBase*>& processors) {
  int total = 0;
  for (const ProcessorBase* processor : processors) {
    if (processor == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "processor must not be null");
    }
    total += processor->latency_samples();
  }
  return total;
}

inline int total_latency_samples_q8(const std::vector<const ProcessorBase*>& processors) {
  int total = 0;
  for (const ProcessorBase* processor : processors) {
    if (processor == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "processor must not be null");
    }
    total += processor->latency_samples_q8();
  }
  return total;
}

}  // namespace sonare::rt
