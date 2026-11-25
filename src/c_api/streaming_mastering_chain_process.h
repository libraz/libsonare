#pragma once

#include <array>

#include "mastering/api/chain.h"
#include "sonare_c_internal.h"

namespace sonare_c_detail {

inline void streaming_mastering_chain_process_mono(
    sonare::mastering::api::StreamingMasteringChain& chain, float* samples, int num_samples) {
  float* channels[] = {samples};
  const std::array<CName, 1> names{{{"num_channels", "samples"}}};
  with_c_names(names, [&] { chain.process_block(channels, 1, num_samples); });
}

inline void streaming_mastering_chain_process_stereo(
    sonare::mastering::api::StreamingMasteringChain& chain, float* left, float* right,
    int num_samples) {
  float* channels[] = {left, right};
  const std::array<CPairName, 1> names{{{"num_channels", "left", "right"}}};
  with_c_pair_names(names, [&] { chain.process_block(channels, 2, num_samples); });
}

inline int streaming_mastering_chain_flush_mono(
    sonare::mastering::api::StreamingMasteringChain& chain, float* samples, int capacity) {
  float* channels[] = {samples};
  const std::array<CName, 1> names{{{"num_channels", "samples"}}};
  return with_c_names(names, [&] { return chain.flush(channels, 1, capacity); });
}

inline int streaming_mastering_chain_flush_stereo(
    sonare::mastering::api::StreamingMasteringChain& chain, float* left, float* right,
    int capacity) {
  float* channels[] = {left, right};
  const std::array<CPairName, 1> names{{{"num_channels", "left", "right"}}};
  return with_c_pair_names(names, [&] { return chain.flush(channels, 2, capacity); });
}

}  // namespace sonare_c_detail
