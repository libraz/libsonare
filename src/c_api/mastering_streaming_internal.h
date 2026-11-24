#pragma once

#include <memory>

#include "mastering/api/chain.h"

struct SonareStreamingMasteringChain {
  std::unique_ptr<sonare::mastering::api::StreamingMasteringChain> chain;
  // Mirrors the last successful prepare(). The process entries scan the caller's
  // buffer for non-finite samples before handing it to the core, so the block
  // bound has to be known HERE: without it the scan is the first thing to
  // dereference the buffer and would read past its end for any oversized
  // num_samples the core would have rejected. Zero until prepare() succeeds.
  int max_block_size = 0;
};
