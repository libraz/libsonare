#pragma once

#include <cstddef>

#include "rt/processor_base.h"

namespace sonare::rt {

struct ProcessorDiscardTestAccess {
  static void add(ProcessorBase& processor, std::size_t count) {
    processor.non_finite_discards_.add(count);
  }
};

}  // namespace sonare::rt
