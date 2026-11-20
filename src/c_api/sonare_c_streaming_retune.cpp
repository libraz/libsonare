#include <sonare/sonare_c.h>

#if defined(SONARE_WITH_VOICE_CHANGER)
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "editing/voice_changer/streaming_retune.h"
#endif
#include "sonare_c_internal.h"
#if defined(SONARE_WITH_VOICE_CHANGER)
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

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
#endif

namespace {

#if defined(SONARE_WITH_VOICE_CHANGER)
using sonare_c_detail::set_last_error;
namespace check = sonare::util::check;

// Refuses a config outside the retune domain with the entry's own argument names; 0 grain is the
// C spelling of the automatic grain.
bool config_accepted(const char* fn, const StreamingRetuneConfig& config) {
  const std::string problem = sonare::editing::voice_changer::streaming_retune_config_problem(
      config, fn, {"pitch_semitones", "mix", "grain_size_samples"});
  if (problem.empty()) return true;
  set_last_error(SONARE_ERROR_INVALID_PARAMETER, problem.c_str());
  return false;
}

// A block longer than the prepared maximum, or one before any prepare().
SonareError refuse_block_length(size_t num_samples, int max_block_size) {
  if (max_block_size <= 0) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "the retune stage has not been prepared");
    return SONARE_ERROR_INVALID_STATE;
  }
  if (num_samples <= static_cast<size_t>(max_block_size)) return SONARE_OK;
  return sonare_c_detail::run_validation([&] {
    check::refuse("num_samples", sonare::util::refusal::integer_range_predicate(0, max_block_size));
  });
}

#endif
}  // namespace

extern "C" {

SonareError sonare_streaming_retune_create(float pitch_semitones, float mix, int grain_size_samples,
                                           SonareStreamingRetune** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_WITH_VOICE_CHANGER)
  StreamingRetuneConfig config;
  config.semitones = pitch_semitones;
  config.mix = mix;
  config.grain_size = grain_size_samples;
  if (!config_accepted("streaming_retune_create", config)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  auto handle = std::make_unique<SonareStreamingRetune>();
  handle->retune = std::make_unique<StreamingRetune>(config);
  *out = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_REQUIRE_NONNULL(out);
  (void)pitch_semitones;
  (void)mix;
  (void)grain_size_samples;
  sonare_c_detail::set_last_error(SONARE_ERROR_NOT_SUPPORTED,
                                  "streaming retune: voice changer support is not built");
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

void sonare_streaming_retune_destroy(SonareStreamingRetune* retune) {
#if defined(SONARE_WITH_VOICE_CHANGER)
  delete retune;
#else
  (void)retune;
#endif
}

SonareError sonare_streaming_retune_prepare(SonareStreamingRetune* retune, double sample_rate,
                                            int max_block_size) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_VOICE_CHANGER)
  SONARE_C_REQUIRE_NONNULL(retune);
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
#else
  SONARE_C_STUB_NOT_SUPPORTED(retune, sample_rate, max_block_size);
#endif
}

SonareError sonare_streaming_retune_reset(SonareStreamingRetune* retune) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_VOICE_CHANGER)
  SONARE_C_REQUIRE_NONNULL(retune);
  SONARE_C_TRY
  retune->retune->reset();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(retune);
#endif
}

SonareError sonare_streaming_retune_set_config(SonareStreamingRetune* retune, float pitch_semitones,
                                               float mix, int grain_size_samples) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_VOICE_CHANGER)
  SONARE_C_REQUIRE_NONNULL(retune);
  StreamingRetuneConfig config;
  config.semitones = pitch_semitones;
  config.mix = mix;
  config.grain_size = grain_size_samples;
  if (!config_accepted("streaming_retune_set_config", config)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  retune->retune->set_config(config);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(retune, pitch_semitones, mix, grain_size_samples);
#endif
}

SonareError sonare_streaming_retune_config(SonareStreamingRetune* retune,
                                           float* out_pitch_semitones, float* out_mix,
                                           int* out_grain_size_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_pitch_semitones, out_mix, out_grain_size_samples);
  SONARE_C_TRY
#if defined(SONARE_WITH_VOICE_CHANGER)
  SONARE_C_REQUIRE_NONNULL(retune);
  const StreamingRetuneConfig& config = retune->retune->config();
  if (out_pitch_semitones) *out_pitch_semitones = config.semitones;
  if (out_mix) *out_mix = config.mix;
  if (out_grain_size_samples) *out_grain_size_samples = config.grain_size;
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(retune, out_pitch_semitones, out_mix, out_grain_size_samples);
#endif
  SONARE_C_CATCH
}

SonareError sonare_streaming_retune_process_mono(SonareStreamingRetune* retune, float* samples,
                                                 size_t num_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
#if defined(SONARE_WITH_VOICE_CHANGER)
  SONARE_C_REQUIRE_NONNULL(retune);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(samples, num_samples);
  if (num_samples == 0) return SONARE_OK;
  if (const SonareError bounds = refuse_block_length(num_samples, retune->max_block_size);
      bounds != SONARE_OK) {
    return bounds;
  }
  if (!sonare::numeric::all_finite(samples, num_samples)) {
    return sonare_c_detail::run_validation(
        [] { check::refuse("samples", "must contain only finite samples"); });
  }
  // The bounds check above already refused anything longer than the prepared
  // maximum, which is exactly what the scratch was sized for, so this copy
  // cannot grow it. process_block is noexcept and the copy allocates nothing,
  // so there is no exception path left to guard here.
  std::copy_n(samples, num_samples, retune->scratch.begin());
  retune->retune->process_block(retune->scratch.data(), samples, static_cast<int>(num_samples));
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(retune, samples, num_samples);
#endif
  SONARE_C_CATCH
}

SonareError sonare_streaming_retune_grain_size_samples(SonareStreamingRetune* retune,
                                                       int* out_grain_size_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_grain_size_samples);
#if defined(SONARE_WITH_VOICE_CHANGER)
  SONARE_C_REQUIRE_NONNULL(retune);
  SONARE_C_REQUIRE_NONNULL(out_grain_size_samples);
  *out_grain_size_samples = retune->retune->grain_size();
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(retune, out_grain_size_samples);
#endif
}

SonareError sonare_streaming_retune_latency_samples(SonareStreamingRetune* retune,
                                                    int* out_latency_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_latency_samples);
  SONARE_C_TRY
#if defined(SONARE_WITH_VOICE_CHANGER)
  SONARE_C_REQUIRE_NONNULL(retune);
  SONARE_C_REQUIRE_NONNULL(out_latency_samples);
  *out_latency_samples = retune->retune->latency_samples();
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(retune, out_latency_samples);
#endif
  SONARE_C_CATCH
}

}  // extern "C"
