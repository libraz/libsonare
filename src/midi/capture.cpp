#include "midi/capture.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "util/insertion_sort.h"

namespace sonare::midi {
namespace {

struct CapturedEvent {
  MidiEvent event;
  double rel_ppq = 0.0;
  size_t order = 0;
};

constexpr double kMaxSafeGridLine = 4.0e18;

bool same_double(double a, double b) noexcept { return a == b || (std::isnan(a) && std::isnan(b)); }

bool same_capture_config(const CaptureConfig& a, const CaptureConfig& b) noexcept {
  if (!same_double(a.clip_start_ppq, b.clip_start_ppq)) return false;
  if (a.quantize.enabled != b.quantize.enabled ||
      !same_double(a.quantize.grid_ppq, b.quantize.grid_ppq) ||
      !same_double(a.quantize.strength, b.quantize.strength) ||
      !same_double(a.quantize.swing, b.quantize.swing) ||
      a.quantize.groove_steps != b.quantize.groove_steps) {
    return false;
  }
  for (size_t i = 0; i < CaptureQuantize::kMaxGrooveSteps; ++i) {
    if (!same_double(a.quantize.groove_offsets[i], b.quantize.groove_offsets[i])) {
      return false;
    }
  }
  return true;
}

bool safe_grid_line(double line, int64_t* out_line) noexcept {
  if (out_line == nullptr || !std::isfinite(line) || line < -kMaxSafeGridLine ||
      line > kMaxSafeGridLine) {
    return false;
  }
  *out_line = static_cast<int64_t>(line);
  return true;
}

double clamp01(double v) noexcept {
  if (!std::isfinite(v)) return 0.0;
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

double clamp_groove_offset(double v) noexcept {
  if (!std::isfinite(v)) return 0.0;
  if (v < -1.0) return -1.0;
  if (v > 1.0) return 1.0;
  return v;
}

size_t bounded_groove_steps(const CaptureQuantize& quantize) noexcept {
  return quantize.groove_steps > CaptureQuantize::kMaxGrooveSteps ? CaptureQuantize::kMaxGrooveSteps
                                                                  : quantize.groove_steps;
}

size_t groove_index(int64_t line, size_t steps) noexcept {
  if (steps == 0) return 0;
  const int64_t s = static_cast<int64_t>(steps);
  int64_t idx = line % s;
  if (idx < 0) idx += s;
  return static_cast<size_t>(idx);
}

double swung_grid_ppq(double line, double grid_ppq, const CaptureQuantize& quantize) noexcept {
  int64_t line_i = 0;
  if (!safe_grid_line(line, &line_i)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double ppq = line * grid_ppq;
  const double swing = clamp01(quantize.swing);
  if (line_i % 2 != 0) {
    ppq += grid_ppq * 0.5 * swing;
  }
  const size_t steps = bounded_groove_steps(quantize);
  if (steps > 0) {
    ppq += grid_ppq * clamp_groove_offset(quantize.groove_offsets[groove_index(line_i, steps)]);
  }
  return ppq;
}

double nearest_quantized_ppq(double ppq, const CaptureQuantize& quantize) noexcept {
  const double grid_ppq = quantize.grid_ppq;
  if (!std::isfinite(ppq) || !std::isfinite(grid_ppq) || grid_ppq <= 0.0) return ppq;
  const double base = std::floor(ppq / grid_ppq);
  if (!std::isfinite(base) || base < -kMaxSafeGridLine + 2.0 || base > kMaxSafeGridLine - 3.0) {
    return ppq;
  }
  double best = ppq;
  double best_dist = std::numeric_limits<double>::infinity();
  bool found = false;
  for (int offset = -2; offset <= 3; ++offset) {
    const double line = base + static_cast<double>(offset);
    const double candidate = swung_grid_ppq(line, grid_ppq, quantize);
    if (!std::isfinite(candidate)) continue;
    const double dist = std::abs(ppq - candidate);
    if (!std::isfinite(dist)) continue;
    if (!found || dist < best_dist || (dist == best_dist && candidate < best)) {
      best = candidate;
      best_dist = dist;
      found = true;
    }
  }
  return found ? best : ppq;
}

}  // namespace

double quantize_ppq(double ppq, double grid_ppq, double strength, double swing) noexcept {
  CaptureQuantize quantize;
  quantize.grid_ppq = grid_ppq;
  quantize.strength = strength;
  quantize.swing = swing;
  return quantize_ppq(ppq, quantize);
}

double quantize_ppq(double ppq, const CaptureQuantize& quantize) noexcept {
  if (!std::isfinite(ppq) || !std::isfinite(quantize.grid_ppq) || quantize.grid_ppq <= 0.0 ||
      !std::isfinite(quantize.strength) || !std::isfinite(quantize.swing)) {
    return ppq;
  }
  const double strength = clamp01(quantize.strength);
  const double snapped = nearest_quantized_ppq(ppq, quantize);
  const double result = ppq + (snapped - ppq) * strength;
  return std::isfinite(result) ? result : ppq;
}

void MidiCapture::reset_note_shift_context() noexcept {
  active_note_shifts_.clear();
  note_shift_context_valid_ = false;
  note_shift_clip_ = nullptr;
  note_shift_config_ = CaptureConfig{};
}

void MidiCapture::prepare(const transport::TempoMap* tempo_map, size_t capacity_pow2) {
  reset_note_shift_context();
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
  if (!note_shift_context_valid_ || note_shift_clip_ != clip ||
      !same_capture_config(note_shift_config_, config)) {
    active_note_shifts_.clear();
    note_shift_context_valid_ = true;
    note_shift_clip_ = clip;
    note_shift_config_ = config;
  }

  auto find_active_note_shift = [&](const MidiEvent& event) -> ActiveNoteShift* {
    for (ActiveNoteShift& note : active_note_shifts_) {
      if (note.group == event.ump.group && note.channel == event.ump.channel() &&
          note.note == event.ump.note_number() && note.source_track_id == event.source_track_id) {
        return &note;
      }
    }
    return nullptr;
  };
  auto push_note_shift = [&](const MidiEvent& event, double shift) {
    ActiveNoteShift* existing = find_active_note_shift(event);
    if (existing != nullptr) {
      existing->shifts.push_back(shift);
      return;
    }
    ActiveNoteShift note;
    note.group = event.ump.group;
    note.channel = event.ump.channel();
    note.note = event.ump.note_number();
    note.source_track_id = event.source_track_id;
    note.shifts.push_back(shift);
    active_note_shifts_.push_back(std::move(note));
  };
  auto pop_note_shift = [&](const MidiEvent& event, double* shift) {
    ActiveNoteShift* existing = find_active_note_shift(event);
    if (existing == nullptr || existing->shifts.empty()) {
      return false;
    }
    *shift = existing->shifts.front();
    existing->shifts.erase(existing->shifts.begin());
    if (existing->shifts.empty()) {
      for (size_t i = 0; i < active_note_shifts_.size(); ++i) {
        if (&active_note_shifts_[i] == existing) {
          active_note_shifts_.erase(active_note_shifts_.begin() + i);
          break;
        }
      }
    }
    return true;
  };

  std::vector<CapturedEvent> events;
  size_t order = 0;
  MidiEvent event;
  while (queue_.pop(event)) {
    // Convert absolute render frame back to musical time, then make it relative
    // to the clip's musical start so the clip is position-independent.
    const double abs_ppq = tempo_map_->sample_to_ppq(event.render_frame);
    double rel_ppq = abs_ppq - config.clip_start_ppq;
    if (rel_ppq < 0.0) {
      rel_ppq = 0.0;
    }
    events.push_back({event, rel_ppq, order++});
  }

  insertion_sort(events.begin(), events.end(), [](const CapturedEvent& a, const CapturedEvent& b) {
    if (a.event.render_frame != b.event.render_frame) {
      return a.event.render_frame < b.event.render_frame;
    }
    return a.order < b.order;
  });

  for (const CapturedEvent& captured : events) {
    double rel_ppq = captured.rel_ppq;
    if (config.quantize.enabled) {
      if (captured.event.ump.is_note_on()) {
        const double quantized = quantize_ppq(rel_ppq, config.quantize);
        push_note_shift(captured.event, quantized - rel_ppq);
        rel_ppq = quantized;
      } else if (captured.event.ump.is_note_off()) {
        double shift = 0.0;
        if (pop_note_shift(captured.event, &shift)) {
          rel_ppq += shift;
        } else {
          rel_ppq = quantize_ppq(rel_ppq, config.quantize);
        }
      } else {
        rel_ppq = quantize_ppq(rel_ppq, config.quantize);
      }
      if (rel_ppq < 0.0) {
        rel_ppq = 0.0;
      }
    }
    MidiClipEvent clip_event;
    clip_event.ppq = rel_ppq;
    clip_event.ump = captured.event.ump;
    clip_event.sysex_payload = captured.event.sysex_payload;
    clip_event.sysex_payload_size = captured.event.sysex_payload_size;
    clip->add_event(clip_event);
  }
  // Stable sort so note-off precedes note-on at identical timestamps (matches
  // MidiClip's deterministic ordering contract).
  clip->sort_stable();
  return events.size();
}

}  // namespace sonare::midi
