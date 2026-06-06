#include "engine/tempo_sync.h"

#include <algorithm>
#include <cmath>

#include "effects/phase_vocoder.h"
#include "transport/musical_time.h"
#include "util/exception.h"

namespace sonare::engine {
namespace {

std::vector<float> stretch_segment(const float* source, size_t source_samples,
                                   size_t target_samples,
                                   const TempoSyncWarpBakeConfig& bake_config) {
  StreamingPhaseVocoderConfig config;
  config.sample_rate = bake_config.sample_rate;
  config.n_fft = bake_config.n_fft;
  config.hop_length = bake_config.hop_length;
  config.phase_lock = bake_config.phase_lock;

  const float rate =
      static_cast<float>(static_cast<double>(source_samples) / static_cast<double>(target_samples));
  StreamingPhaseVocoder stretcher(config);
  stretcher.reserve(source_samples, target_samples + static_cast<size_t>(config.n_fft));

  std::vector<float> out(target_samples + static_cast<size_t>(config.n_fft), 0.0f);
  size_t written = stretcher.process_into(source, source_samples, rate, out.data(), out.size());
  written += stretcher.finalize_into(rate, out.data() + written, out.size() - written);
  out.resize(std::min(written, target_samples));
  if (out.size() < target_samples) out.resize(target_samples, 0.0f);
  return out;
}

void smooth_segment_joins(std::vector<float>* audio, const std::vector<size_t>& boundaries,
                          size_t requested_fade) {
  if (audio == nullptr || requested_fade == 0 || audio->empty()) return;
  std::vector<float>& out = *audio;
  for (const size_t boundary : boundaries) {
    if (boundary == 0 || boundary >= out.size()) continue;
    const size_t fade = std::min({requested_fade, boundary, out.size() - boundary});
    if (fade == 0) continue;
    const float next_anchor = out[boundary];
    for (size_t i = 0; i < fade; ++i) {
      const float a = static_cast<float>(i + 1) / static_cast<float>(fade + 1);
      const size_t left_index = boundary - fade + i;
      const size_t right_index = boundary + i;
      out[left_index] = out[left_index] * (1.0f - a) + next_anchor * a;
      out[right_index] = next_anchor * (1.0f - a) + out[right_index] * a;
    }
  }
}

}  // namespace

double tempo_sync_ppq(TempoSyncValue value) noexcept {
  return transport::note_length_ppq(value.denominator, value.modifier);
}

int64_t tempo_sync_samples(TempoSyncValue value, const transport::TransportState& state) noexcept {
  return transport::ppq_duration_to_samples(tempo_sync_ppq(value), state.bpm, state.sample_rate);
}

std::vector<float> bake_tempo_sync_warp_channel(const float* source, size_t source_samples,
                                                const std::vector<TempoSyncWarpSegment>& segments,
                                                const TempoSyncWarpBakeConfig& config) {
  SONARE_CHECK(source != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(source_samples > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.sample_rate > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_fft > 0 && config.hop_length > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(!segments.empty(), ErrorCode::InvalidParameter);

  size_t total_target_samples = 0;
  for (const TempoSyncWarpSegment& segment : segments) {
    SONARE_CHECK(segment.source_samples > 0 && segment.target_samples > 0,
                 ErrorCode::InvalidParameter);
    SONARE_CHECK(segment.source_offset <= source_samples &&
                     segment.source_samples <= source_samples - segment.source_offset,
                 ErrorCode::InvalidParameter);
    total_target_samples += segment.target_samples;
  }

  std::vector<float> output;
  output.reserve(total_target_samples);
  std::vector<size_t> boundaries;
  boundaries.reserve(segments.size() > 0 ? segments.size() - 1 : 0);

  for (const TempoSyncWarpSegment& segment : segments) {
    if (!output.empty()) boundaries.push_back(output.size());
    std::vector<float> stretched = stretch_segment(
        source + segment.source_offset, segment.source_samples, segment.target_samples, config);
    output.insert(output.end(), stretched.begin(), stretched.end());
  }

  if (output.size() > total_target_samples) {
    output.resize(total_target_samples);
  } else if (output.size() < total_target_samples) {
    output.resize(total_target_samples, 0.0f);
  }
  smooth_segment_joins(&output, boundaries, config.join_crossfade_samples);
  return output;
}

}  // namespace sonare::engine
