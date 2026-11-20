#include <sonare/sonare_c.h>

#include <cstring>
#include <memory>

#include "mastering/api/chain.h"
#include "sonare_c_internal.h"
#include "sonare_c_mastering_helpers.h"

using namespace sonare;
using namespace sonare_c_detail;
using namespace sonare_c_mastering_detail;

void sonare_free_mastering_chain_result(SonareMasteringChainResult* result) {
  if (!result) return;
  delete[] result->samples;
  if (result->stages) {
    for (size_t i = 0; i < result->stages_count; ++i) {
      delete[] result->stages[i];
    }
    delete[] result->stages;
  }
  free_chain_metrics(result);
  *result = {};
}

void sonare_free_mastering_chain_stereo_result(SonareMasteringChainStereoResult* result) {
  if (!result) return;
  delete[] result->left;
  delete[] result->right;
  if (result->stages) {
    for (size_t i = 0; i < result->stages_count; ++i) {
      delete[] result->stages[i];
    }
    delete[] result->stages;
  }
  free_chain_metrics(result);
  *result = {};
}
