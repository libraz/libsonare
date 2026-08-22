#include <sonare/sonare_c.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "editing/voice_changer/streaming_retune.h"
#include "sonare_c_internal.h"

using sonare::editing::voice_changer::StreamingRetune;
using sonare::editing::voice_changer::StreamingRetuneConfig;

struct SonareStreamingRetune {
  std::unique_ptr<StreamingRetune> retune;
  // Mirrors the last successful prepare(). process_mono scans the caller's
  // buffer for non-finite samples before handing it to the core, so the block
  // bound has to be known HERE: without it the scan is the first thing to
  // dereference the buffer and would read past its end for any oversized
  // num_samples the core would have refused. Zero until prepare() succeeds.
  // Same reason as SonareStreamingMasteringChain.
  int max_block_size = 0;
  // process_block reads the input and writes the output through separate
  // pointers, so an in-place C call needs somewhere to read from. Sized once by
  // prepare() rather than allocated per block: the core promises "Allocation
  // happens only in prepare()", and a per-block vector broke that promise 375
  // times a second at 128 samples / 48 kHz. Same persistent-scratch shape as
  // sonare_c_voice_changer.cpp.
  std::vector<float> scratch;
};

namespace {

using sonare_c_detail::set_last_error;

bool all_finite(const float* samples, size_t num_samples) noexcept {
  if (!samples) return num_samples == 0;
  for (size_t i = 0; i < num_samples; ++i) {
    if (!std::isfinite(samples[i])) return false;
  }
  return true;
}

// The two live controls are clamped by the core to its documented ranges, but a
// NON-FINITE value is a caller error rather than a request to clamp: the core
// substitutes a default for it, which would let a NaN arrive here and leave as
// a silent 0 semitones. Reject it at this boundary instead, the way every other
// scalar C entry point does.
bool finite_controls(float semitones, float mix) noexcept {
  return std::isfinite(semitones) && std::isfinite(mix);
}

// Reproduces the two rejections StreamingRetune::process_block makes on state
// and block size. The core's is noexcept and answers a violated precondition
// with a silent no-op to stay audio-thread callable, so a C caller would get
// SONARE_OK and an unchanged buffer with nothing to distinguish it from a
// successful render. Report them here instead.
SonareError check_block_bounds(const SonareStreamingRetune* handle, size_t num_samples) noexcept {
  if (handle->max_block_size <= 0) return SONARE_ERROR_INVALID_STATE;
  if (num_samples > static_cast<size_t>(handle->max_block_size)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
}

}  // namespace

extern "C" {

SonareStreamingRetune* sonare_streaming_retune_create(float semitones, float mix, int grain_size) {
  SONARE_C_API_ENTRY;
  if (!finite_controls(semitones, mix)) {
    set_last_error("streaming retune: semitones and mix must be finite");
    return nullptr;
  }
  SONARE_C_TRY
  StreamingRetuneConfig config;
  config.semitones = semitones;
  config.mix = mix;
  config.grain_size = grain_size;
  auto* handle = new SonareStreamingRetune;
  handle->retune = std::make_unique<StreamingRetune>(config);
  return handle;
  SONARE_C_CATCH_RETURN(nullptr)
}

void sonare_streaming_retune_destroy(SonareStreamingRetune* retune) { delete retune; }

SonareError sonare_streaming_retune_prepare(SonareStreamingRetune* retune, double sample_rate,
                                            int max_block_size) {
  SONARE_C_API_ENTRY;
  if (!retune || !retune->retune) return SONARE_ERROR_INVALID_PARAMETER;
  if (!(sample_rate > 0.0) || max_block_size < 0) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  retune->retune->prepare(sample_rate, max_block_size);
  // Size the block scratch here, where allocation is allowed, so process_mono
  // never has to. assign() also zeroes it, which keeps a stale tail from a
  // larger previous prepare out of the buffer a debugger would show. It runs
  // BEFORE max_block_size is published: that field is what admits a block into
  // process_mono, so a throwing reallocation must not leave it authorizing a
  // block the scratch cannot hold.
  retune->scratch.assign(static_cast<size_t>(max_block_size), 0.0f);
  retune->max_block_size = max_block_size;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_streaming_retune_reset(SonareStreamingRetune* retune) {
  SONARE_C_API_ENTRY;
  if (!retune || !retune->retune) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  retune->retune->reset();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_streaming_retune_set_config(SonareStreamingRetune* retune, float semitones,
                                               float mix, int grain_size) {
  SONARE_C_API_ENTRY;
  if (!retune || !retune->retune) return SONARE_ERROR_INVALID_PARAMETER;
  if (!finite_controls(semitones, mix)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  StreamingRetuneConfig config;
  config.semitones = semitones;
  config.mix = mix;
  config.grain_size = grain_size;
  retune->retune->set_config(config);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_streaming_retune_config(SonareStreamingRetune* retune, float* out_semitones,
                                           float* out_mix, int* out_grain_size) {
  SONARE_C_API_ENTRY;
  if (!retune || !retune->retune) return SONARE_ERROR_INVALID_PARAMETER;
  const StreamingRetuneConfig& config = retune->retune->config();
  if (out_semitones) *out_semitones = config.semitones;
  if (out_mix) *out_mix = config.mix;
  if (out_grain_size) *out_grain_size = config.grain_size;
  return SONARE_OK;
}

SonareError sonare_streaming_retune_process_mono(SonareStreamingRetune* retune, float* samples,
                                                 size_t num_samples) {
  SONARE_C_API_ENTRY;
  if (!retune || !retune->retune || (!samples && num_samples > 0)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (num_samples == 0) return SONARE_OK;
  if (const SonareError bounds = check_block_bounds(retune, num_samples); bounds != SONARE_OK) {
    return bounds;
  }
  if (!all_finite(samples, num_samples)) return SONARE_ERROR_INVALID_PARAMETER;
  // The bounds check above already refused anything longer than the prepared
  // maximum, which is exactly what the scratch was sized for, so this copy
  // cannot grow it. process_block is noexcept and the copy allocates nothing,
  // so there is no exception path left to guard here.
  std::copy_n(samples, num_samples, retune->scratch.begin());
  retune->retune->process_block(retune->scratch.data(), samples, static_cast<int>(num_samples));
  return SONARE_OK;
}

SonareError sonare_streaming_retune_grain_size(SonareStreamingRetune* retune, int* out_grain_size) {
  SONARE_C_API_ENTRY;
  if (!retune || !retune->retune || !out_grain_size) return SONARE_ERROR_INVALID_PARAMETER;
  *out_grain_size = retune->retune->grain_size();
  return SONARE_OK;
}

SonareError sonare_streaming_retune_latency_samples(SonareStreamingRetune* retune,
                                                    int* out_latency_samples) {
  SONARE_C_API_ENTRY;
  if (!retune || !retune->retune || !out_latency_samples) return SONARE_ERROR_INVALID_PARAMETER;
  *out_latency_samples = retune->retune->latency_samples();
  return SONARE_OK;
}

}  // extern "C"
