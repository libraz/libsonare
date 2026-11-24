#pragma once

#include "mastering/eq/equalizer.h"

struct SonareEq {
  sonare::mastering::eq::EqualizerProcessor processor;
  double sample_rate = 48000.0;
  int max_block_size = 0;
};
