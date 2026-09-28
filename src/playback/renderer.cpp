#include "playback/renderer.h"

#include <algorithm>

namespace sonare::playback {

struct PlaybackRenderer::Impl {
  RendererConfig config;
  int sample_rate = 0;
  int max_block_size = 0;
};

std::string diagnostics_to_json(const RendererDiagnostics& diagnostics) {
  (void)diagnostics;
  return "{}";
}

PlaybackRenderer::PlaybackRenderer(const RendererConfig& config, const HrtfSet* hrtf,
                                   int sample_rate, int max_block_size)
    : impl_(std::make_unique<Impl>()) {
  (void)hrtf;
  impl_->config = config;
  impl_->sample_rate = sample_rate;
  impl_->max_block_size = max_block_size;
}

PlaybackRenderer::~PlaybackRenderer() = default;

void PlaybackRenderer::set_config(const RendererConfig& config) { impl_->config = config; }

RendererConfig PlaybackRenderer::config() const { return impl_->config; }

void PlaybackRenderer::set_head_orientation(float yaw_deg, float pitch_deg,
                                            float roll_deg) noexcept {
  (void)yaw_deg;
  (void)pitch_deg;
  (void)roll_deg;
}

bool PlaybackRenderer::process_planar(const float* const* in, int in_channels, float* const* out,
                                      int out_channels, int frames) noexcept {
  (void)in;
  (void)in_channels;
  if (out == nullptr || frames <= 0) return true;
  for (int ch = 0; ch < out_channels; ++ch) {
    if (out[ch] != nullptr) std::fill(out[ch], out[ch] + frames, 0.0f);
  }
  return true;
}

bool PlaybackRenderer::process_interleaved(const float* in, int in_channels, float* out,
                                           int out_channels, int frames) noexcept {
  (void)in;
  (void)in_channels;
  if (out == nullptr || frames <= 0 || out_channels <= 0) return true;
  std::fill(out, out + static_cast<size_t>(frames) * static_cast<size_t>(out_channels), 0.0f);
  return true;
}

void PlaybackRenderer::reset() noexcept {}

int PlaybackRenderer::latency_samples() const noexcept { return 0; }

int PlaybackRenderer::input_channel_count() const noexcept { return 0; }

int PlaybackRenderer::output_channel_count() const noexcept { return 0; }

int PlaybackRenderer::sample_rate() const noexcept { return impl_->sample_rate; }

int PlaybackRenderer::max_block_size() const noexcept { return impl_->max_block_size; }

RendererDiagnostics PlaybackRenderer::diagnostics() const { return {}; }

uint32_t PlaybackRenderer::non_finite_discard_count() const noexcept { return 0; }

std::vector<float> render_interleaved(const float* in, size_t frames, int in_channels,
                                      int sample_rate, const RendererConfig& config,
                                      const HrtfSet* hrtf, int* out_channels) {
  (void)in;
  (void)frames;
  (void)in_channels;
  (void)sample_rate;
  (void)config;
  (void)hrtf;
  if (out_channels != nullptr) *out_channels = 0;
  return {};
}

}  // namespace sonare::playback
