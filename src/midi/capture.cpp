#include "midi/capture.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace sonare::midi {
namespace {

struct CapturedEvent {
  MidiEvent event;
  double rel_ppq = 0.0;
  size_t order = 0;
};

struct ActiveNoteShift {
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;
  std::vector<double> shifts;
};

ActiveNoteShift* find_active_note_shift(std::vector<ActiveNoteShift>* active,
                                        const Ump& ump) noexcept {
  if (active == nullptr) return nullptr;
  for (ActiveNoteShift& note : *active) {
    if (note.group == ump.group && note.channel == ump.channel() &&
        note.note == ump.note_number()) {
      return &note;
    }
  }
  return nullptr;
}

void push_note_shift(std::vector<ActiveNoteShift>* active, const Ump& ump, double shift) {
  ActiveNoteShift* existing = find_active_note_shift(active, ump);
  if (existing != nullptr) {
    existing->shifts.push_back(shift);
    return;
  }
  ActiveNoteShift note;
  note.group = ump.group;
  note.channel = ump.channel();
  note.note = ump.note_number();
  note.shifts.push_back(shift);
  active->push_back(std::move(note));
}

bool pop_note_shift(std::vector<ActiveNoteShift>* active, const Ump& ump, double* shift) {
  ActiveNoteShift* existing = find_active_note_shift(active, ump);
  if (existing == nullptr || existing->shifts.empty()) {
    return false;
  }
  *shift = existing->shifts.front();
  existing->shifts.erase(existing->shifts.begin());
  return true;
}

double clamp01(double v) noexcept {
  if (v < 0.0) return 0.0;
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
  double ppq = line * grid_ppq;
  const double swing = clamp01(quantize.swing);
  if (static_cast<int64_t>(line) % 2 != 0) {
    ppq += grid_ppq * 0.5 * swing;
  }
  const size_t steps = bounded_groove_steps(quantize);
  if (steps > 0) {
    const int64_t line_i = static_cast<int64_t>(line);
    ppq += grid_ppq * quantize.groove_offsets[groove_index(line_i, steps)];
  }
  return ppq;
}

double nearest_quantized_ppq(double ppq, const CaptureQuantize& quantize) noexcept {
  const double grid_ppq = quantize.grid_ppq;
  if (grid_ppq <= 0.0) return ppq;
  const double base = std::floor(ppq / grid_ppq);
  double best = swung_grid_ppq(base - 2.0, grid_ppq, quantize);
  double best_dist = std::abs(ppq - best);
  for (int offset = -1; offset <= 3; ++offset) {
    const double line = base + static_cast<double>(offset);
    const double candidate = swung_grid_ppq(line, grid_ppq, quantize);
    const double dist = std::abs(ppq - candidate);
    if (dist < best_dist || (dist == best_dist && candidate < best)) {
      best = candidate;
      best_dist = dist;
    }
  }
  return best;
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
  if (quantize.grid_ppq <= 0.0) {
    return ppq;
  }
  const double strength = clamp01(quantize.strength);
  const double snapped = nearest_quantized_ppq(ppq, quantize);
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

  std::stable_sort(events.begin(), events.end(),
                   [](const CapturedEvent& a, const CapturedEvent& b) {
                     if (a.event.render_frame != b.event.render_frame) {
                       return a.event.render_frame < b.event.render_frame;
                     }
                     return a.order < b.order;
                   });

  std::vector<ActiveNoteShift> active_note_shifts;
  for (const CapturedEvent& captured : events) {
    double rel_ppq = captured.rel_ppq;
    if (config.quantize.enabled) {
      if (captured.event.ump.is_note_on()) {
        const double quantized = quantize_ppq(rel_ppq, config.quantize);
        push_note_shift(&active_note_shifts, captured.event.ump, quantized - rel_ppq);
        rel_ppq = quantized;
      } else if (captured.event.ump.is_note_off()) {
        double shift = 0.0;
        if (pop_note_shift(&active_note_shifts, captured.event.ump, &shift)) {
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
    clip->add_event(clip_event);
  }
  // Stable sort so note-off precedes note-on at identical timestamps (matches
  // MidiClip's deterministic ordering contract).
  clip->sort_stable();
  return events.size();
}

}  // namespace sonare::midi
