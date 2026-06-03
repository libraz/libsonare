#include "midi/capture.h"

#include <cmath>

namespace sonare::midi {

double quantize_ppq(double ppq, double grid_ppq, double strength) noexcept {
  if (grid_ppq <= 0.0) {
    return ppq;
  }
  if (strength < 0.0) strength = 0.0;
  if (strength > 1.0) strength = 1.0;
  const double snapped = std::round(ppq / grid_ppq) * grid_ppq;
  return ppq + (snapped - ppq) * strength;
}

void MidiCapture::prepare(const transport::TempoMap* tempo_map, size_t capacity_pow2) {
  tempo_map_ = tempo_map;
  queue_.reserve(capacity_pow2);
  dropped_count_.store(0, std::memory_order_relaxed);
}

bool MidiCapture::push(const MidiEvent& event) noexcept {
  if (queue_.push(event)) {
    return true;
  }
  dropped_count_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

size_t MidiCapture::drain(const CaptureConfig& config, MidiClip* clip) {
  if (clip == nullptr || tempo_map_ == nullptr) {
    return 0;
  }
  size_t drained = 0;
  MidiEvent event;
  while (queue_.pop(event)) {
    // Convert absolute render frame back to musical time, then make it relative
    // to the clip's musical start so the clip is position-independent.
    const double abs_ppq = tempo_map_->sample_to_ppq(event.render_frame);
    double rel_ppq = abs_ppq - config.clip_start_ppq;
    if (rel_ppq < 0.0) {
      rel_ppq = 0.0;
    }
    if (config.quantize.enabled) {
      rel_ppq = quantize_ppq(rel_ppq, config.quantize.grid_ppq, config.quantize.strength);
      if (rel_ppq < 0.0) {
        rel_ppq = 0.0;
      }
    }
    MidiClipEvent clip_event;
    clip_event.ppq = rel_ppq;
    clip_event.ump = event.ump;
    clip->add_event(clip_event);
    ++drained;
  }
  // Stable sort so note-off precedes note-on at identical timestamps (matches
  // MidiClip's deterministic ordering contract).
  clip->sort_stable();
  return drained;
}

}  // namespace sonare::midi
