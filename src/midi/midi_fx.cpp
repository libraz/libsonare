#include "midi/midi_fx.h"

#include <cmath>
#include <cstdint>
#include <limits>

#include "midi/control_value.h"
#include "midi/midi_clip.h"
#include "midi/note_fx_shared.h"
#include "util/numeric_validation.h"
#include "util/splitmix64.h"

namespace sonare::midi {
namespace {

using fx_shared::bounded_groove_steps;
using fx_shared::clamp01;
using fx_shared::clamp_groove_offset;
using fx_shared::groove_index;
using fx_shared::retune_note_event;
using fx_shared::with_note_number;

class HumanizeRandom {
 public:
  explicit HumanizeRandom(uint64_t seed) noexcept : stream_(seed) {}

  // Uniform integer in [-amount, +amount]. amount must be >= 0.
  int64_t symmetric(int64_t amount) noexcept {
    if (amount <= 0) return 0;
    const uint64_t span = static_cast<uint64_t>(amount) * 2ull + 1ull;
    const uint64_t draw = stream_.next() % span;
    const uint64_t magnitude = static_cast<uint64_t>(amount);
    if (draw >= magnitude) return static_cast<int64_t>(draw - magnitude);
    return -static_cast<int64_t>(magnitude - draw);
  }

 private:
  ::sonare::SplitMix64 stream_;
};

// Mix the chain seed with an event ordinal so each event in the block gets an
// independent (but deterministic) jitter draw.
uint64_t seed_for(uint32_t base_seed, size_t ordinal) noexcept {
  return (static_cast<uint64_t>(base_seed) << 32) ^ (0xD1B54A32D192ED03ull * (ordinal + 1));
}

int clamp_note(int64_t note) noexcept {
  if (note < 0) return 0;
  if (note > 127) return 127;
  return note;
}

int clamp_velocity_on(int64_t velocity) noexcept {
  // A note-on keeps at least velocity 1 so it never degrades into a note-off.
  if (velocity < 1) return 1;
  if (velocity > 127) return 127;
  return velocity;
}

int clamp_velocity16_on(int64_t velocity) noexcept {
  // MIDI 2.0 equivalent of clamp_velocity_on: keep at least 1 (never a
  // note-off) and cap at the full 16-bit range.
  if (velocity < 1) return 1;
  if (velocity > 65535) return 65535;
  return velocity;
}

int64_t saturated_product_positive(int64_t lhs, int64_t rhs) noexcept {
  if (rhs <= 0) return 0;
  if (lhs > 0 && lhs > std::numeric_limits<int64_t>::max() / rhs) {
    return std::numeric_limits<int64_t>::max();
  }
  if (lhs < 0 && lhs < std::numeric_limits<int64_t>::min() / rhs) {
    return std::numeric_limits<int64_t>::min();
  }
  return lhs * rhs;
}

int64_t rounded_frame(double value) noexcept {
  constexpr double kMax = static_cast<double>(std::numeric_limits<int64_t>::max());
  constexpr double kMin = static_cast<double>(std::numeric_limits<int64_t>::min());
  if (value >= kMax) return std::numeric_limits<int64_t>::max();
  if (value <= kMin) return std::numeric_limits<int64_t>::min();
  if (!std::isfinite(value)) return value > 0.0 ? std::numeric_limits<int64_t>::max() : 0;
  return static_cast<int64_t>(std::llround(value));
}

uint64_t distance(int64_t a, int64_t b) noexcept {
  return a >= b ? static_cast<uint64_t>(a) - static_cast<uint64_t>(b)
                : static_cast<uint64_t>(b) - static_cast<uint64_t>(a);
}

int64_t swung_grid_frame(int64_t line, int64_t grid, const QuantizeConfig& config) noexcept {
  int64_t frame = saturated_product_positive(line, grid);
  const float swing = clamp01(config.swing);
  if ((line & 1ll) != 0) {
    const int64_t swing_offset = rounded_frame(static_cast<double>(grid) * 0.5 * swing);
    frame = numeric::saturating_add(frame, swing_offset);
  }
  const size_t steps = bounded_groove_steps(config);
  if (steps > 0) {
    const float offset = clamp_groove_offset(config.groove_offsets[groove_index(line, steps)]);
    frame = numeric::saturating_add(
        frame, rounded_frame(static_cast<double>(grid) * static_cast<double>(offset)));
  }
  return frame;
}

int64_t nearest_quantized_frame(int64_t frame, int64_t grid,
                                const QuantizeConfig& config) noexcept {
  if (grid <= 0) return frame;
  int64_t base = frame / grid;
  if (frame % grid < 0) --base;
  bool found = false;
  int64_t best = frame;
  uint64_t best_dist = 0;
  for (int offset = -2; offset <= 3; ++offset) {
    int64_t line = 0;
    if (!numeric::checked_add(base, static_cast<int64_t>(offset), &line)) continue;
    const int64_t candidate = swung_grid_frame(line, grid, config);
    const uint64_t dist = distance(frame, candidate);
    if (!found || dist < best_dist || (dist == best_dist && candidate < best)) {
      best = candidate;
      best_dist = dist;
      found = true;
    }
  }
  return found ? best : frame;
}

bool is_midi2_note_addressed_form(const Ump& ump) noexcept {
  if (ump.message_type() != UmpMessageType::kMidi2ChannelVoice) return false;
  const uint8_t status = ump.status_nibble();
  return status == static_cast<uint8_t>(UmpStatus::kRegisteredPerNoteController) ||
         status == static_cast<uint8_t>(UmpStatus::kAssignablePerNoteController) ||
         status == static_cast<uint8_t>(UmpStatus::kPerNotePitchBend) ||
         status == static_cast<uint8_t>(UmpStatus::kPerNoteManagement);
}

bool is_poly_pressure_form(const Ump& ump) noexcept {
  return ump.status_nibble() == static_cast<uint8_t>(UmpStatus::kPolyPressure);
}

uint8_t note_velocity7(const Ump& ump) noexcept {
  if (ump.message_type() == UmpMessageType::kMidi2ChannelVoice) {
    return scale_velocity_16_to_7(static_cast<uint16_t>(ump.words[1] >> 16u));
  }
  return midi1_velocity7(ump);
}

uint16_t note_velocity16(const Ump& ump) noexcept {
  if (ump.message_type() == UmpMessageType::kMidi2ChannelVoice) {
    return static_cast<uint16_t>(ump.words[1] >> 16u);
  }
  return scale_velocity_7_to_16(midi1_velocity7(ump));
}

/// Rewrites a MIDI 2.0 note-on velocity in the full 16-bit domain, preserving
/// the note number and note-on attribute bytes.
Ump make_midi2_note_on_velocity16(const Ump& src, uint8_t note, uint16_t velocity16) noexcept {
  const uint8_t attr_type = static_cast<uint8_t>(src.words[0] & 0xFFu);
  const uint16_t attr_data = static_cast<uint16_t>(src.words[1] & 0xFFFFu);
  return make_midi2_note_on(src.group, src.channel(), note, velocity16, attr_type, attr_data);
}

Ump make_note(const Ump& src, bool note_on, uint8_t note, Velocity16 velocity) noexcept {
  if (src.message_type() == UmpMessageType::kMidi2ChannelVoice) {
    const uint16_t velocity16 = velocity.raw;
    if (note_on) {
      const uint8_t attr_type = static_cast<uint8_t>(src.words[0] & 0xFFu);
      const uint16_t attr_data = static_cast<uint16_t>(src.words[1] & 0xFFFFu);
      return make_midi2_note_on(src.group, src.channel(), note, velocity16, attr_type, attr_data);
    }
    return make_midi2_note_off(src.group, src.channel(), note, velocity16);
  }
  // u7() floors at 1, but a MIDI 1.0 note-off may carry velocity 0.
  const uint8_t velocity7 = velocity.raw == 0 ? uint8_t{0} : velocity.u7();
  if (note_on) {
    return make_midi1_note_on(src.group, src.channel(), note, velocity7);
  }
  return make_midi1_note_off(src.group, src.channel(), note, velocity7);
}

}  // namespace

void MidiFxChain::push_or_overflow(const MidiEvent& ev, MidiFxBuffer* out) noexcept {
  if (!out->push(ev)) {
    overflow_count_.bump();
  }
}

void MidiFxChain::clear_note_timings() noexcept {
  active_note_timing_count_ = 0;
  next_note_timing_order_ = 0;
}

void MidiFxChain::clear_note_tracking(uint8_t group, uint8_t channel) noexcept {
  size_t write = 0;
  for (size_t read = 0; read < active_note_timing_count_; ++read) {
    const ActiveNoteTiming& timing = active_note_timings_[read];
    if (timing.group == group && timing.channel == channel) continue;
    if (write != read) active_note_timings_[write] = timing;
    ++write;
  }
  active_note_timing_count_ = write;
}

void MidiFxChain::clear_clip_note_tracking() noexcept {
  size_t write = 0;
  for (size_t read = 0; read < active_note_timing_count_; ++read) {
    const ActiveNoteTiming& timing = active_note_timings_[read];
    if (timing.from_clip) continue;
    if (write != read) active_note_timings_[write] = timing;
    ++write;
  }
  active_note_timing_count_ = write;
}

void MidiFxChain::clear_note_tracking(uint8_t group, uint8_t channel, uint8_t note,
                                      uint32_t source_track_id, bool from_clip,
                                      uint32_t clip_id) noexcept {
  size_t write = 0;
  for (size_t read = 0; read < active_note_timing_count_; ++read) {
    const ActiveNoteTiming& timing = active_note_timings_[read];
    if (timing.group == group && timing.channel == channel && timing.note == note &&
        timing.source_track_id == source_track_id && timing.from_clip == from_clip &&
        timing.clip_id == clip_id) {
      continue;
    }
    if (write != read) active_note_timings_[write] = timing;
    ++write;
  }
  active_note_timing_count_ = write;
}

size_t MidiFxChain::find_earliest_note_timing(const MidiEvent& event, bool from_clip,
                                              uint32_t clip_id,
                                              const int64_t* shaped_on_frame) const noexcept {
  size_t match = active_note_timing_count_;
  uint64_t earliest = UINT64_MAX;
  const uint8_t group = ump_group_from_word0(event.ump.words[0]);
  for (size_t i = 0; i < active_note_timing_count_; ++i) {
    const ActiveNoteTiming& timing = active_note_timings_[i];
    if (timing.group == group && timing.channel == event.ump.channel() &&
        timing.note == event.ump.note_number() && timing.source_track_id == event.source_track_id &&
        timing.from_clip == from_clip && timing.clip_id == clip_id &&
        (shaped_on_frame == nullptr || timing.shaped_on_frame == *shaped_on_frame) &&
        timing.order < earliest) {
      match = i;
      earliest = timing.order;
    }
  }
  return match;
}

void MidiFxChain::clear_pending_note_tracking(const MidiEvent& event, bool from_clip,
                                              uint32_t clip_id) noexcept {
  if (!is_channel_voice(event.ump) || !event.ump.is_note_on()) return;

  const int64_t shaped_on_frame = event.render_frame;
  const size_t match = find_earliest_note_timing(event, from_clip, clip_id, &shaped_on_frame);
  if (match == active_note_timing_count_) return;
  active_note_timings_[match] = active_note_timings_[active_note_timing_count_ - 1];
  --active_note_timing_count_;
}

void MidiFxChain::push_note_timing(const MidiEvent& event, int64_t frame_shift,
                                   int64_t shaped_on_frame, bool from_clip,
                                   uint32_t clip_id) noexcept {
  if (active_note_timing_count_ >= active_note_timings_.size()) {
    overflow_count_.bump();
    return;
  }
  ActiveNoteTiming& timing = active_note_timings_[active_note_timing_count_++];
  timing.group = ump_group_from_word0(event.ump.words[0]);
  timing.channel = event.ump.channel();
  timing.note = event.ump.note_number();
  timing.source_track_id = event.source_track_id;
  timing.from_clip = from_clip;
  timing.clip_id = clip_id;
  timing.frame_shift = frame_shift;
  timing.shaped_on_frame = shaped_on_frame;
  timing.order = next_note_timing_order_++;
}

bool MidiFxChain::pop_note_timing(const MidiEvent& event, bool from_clip, uint32_t clip_id,
                                  ActiveNoteTiming* out) noexcept {
  if (out == nullptr) return false;
  const size_t match = find_earliest_note_timing(event, from_clip, clip_id, nullptr);
  if (match == active_note_timing_count_) return false;
  *out = active_note_timings_[match];
  active_note_timings_[match] = active_note_timings_[active_note_timing_count_ - 1];
  --active_note_timing_count_;
  return true;
}

void MidiFxChain::process(const MidiEvent* in, size_t count, MidiFxBuffer* out) noexcept {
  process_chunk(in, count, 0, out, false, 0);
}

void MidiFxChain::process_chunk(const MidiEvent* in, size_t count, size_t input_ordinal_base,
                                MidiFxBuffer* out, bool from_clip, uint32_t clip_id,
                                int64_t grid_origin_frame) noexcept {
  if (in == nullptr || out == nullptr) return;
  out->clear();

  for (size_t i = 0; i < count; ++i) {
    const size_t input_ordinal = input_ordinal_base + i;
    MidiEvent ev = in[i];
    // SysEx passes through untouched: original frame and prepared operation.
    if (is_sysex_event(ev)) {
      push_or_overflow(ev, out);
      continue;
    }
    ev.ump.group = ump_group_from_word0(ev.ump.words[0]);
    const bool channel_voice = is_channel_voice(ev.ump);
    const bool note_on = channel_voice && ev.ump.is_note_on();
    const bool note_off = channel_voice && ev.ump.is_note_off();
    const uint8_t source_note = ev.ump.note_number();
    const bool note_addressed = channel_voice && is_midi2_note_addressed_form(ev.ump);
    const bool poly_pressure = channel_voice && is_poly_pressure_form(ev.ump);

    // ---- 1. Transpose (every note-addressed channel-voice message) ----
    // Every note-addressed message shifts by the same amount: aftertouch left on
    // the source note number would address a note the shifted voice never played.
    if (transpose_.enabled && (note_on || note_off || note_addressed || poly_pressure)) {
      const int shifted = clamp_note(static_cast<int64_t>(ev.ump.note_number()) +
                                     static_cast<int64_t>(transpose_.semitones));
      ev.ump = with_note_number(ev.ump, static_cast<uint8_t>(shifted));
    }

    // ---- 2. Velocity curve (note-on only) ----
    // The curve is defined in normalized [0, 1] space so the same config maps
    // cleanly onto either resolution: MIDI 2.0 notes are shaped in the full
    // 16-bit domain (no 7-bit round-trip), MIDI 1.0 notes in the 7-bit domain.
    // `offset` is in 7-bit velocity units, so it normalizes by 127.
    if (velocity_.enabled && note_on) {
      const bool midi2 = ev.ump.message_type() == UmpMessageType::kMidi2ChannelVoice;
      const float full_scale = midi2 ? 65535.0f : 127.0f;
      const float raw =
          static_cast<float>(midi2 ? note_velocity16(ev.ump) : note_velocity7(ev.ump));
      float normalized = raw / full_scale;
      if (velocity_.gamma != 1.0f && velocity_.gamma > 0.0f) {
        normalized = std::pow(normalized, velocity_.gamma);
      }
      const float shaped = normalized * velocity_.scale + velocity_.offset / 127.0f;
      const float bounded_shaped =
          std::isnan(shaped) || shaped <= 0.0f ? 0.0f : (shaped >= 1.0f ? 1.0f : shaped);
      if (midi2) {
        const int mapped =
            clamp_velocity16_on(static_cast<int64_t>(std::lround(bounded_shaped * 65535.0f)));
        ev.ump = make_midi2_note_on_velocity16(ev.ump, ev.ump.note_number(),
                                               static_cast<uint16_t>(mapped));
      } else {
        const int mapped =
            clamp_velocity_on(static_cast<int64_t>(std::lround(bounded_shaped * 127.0f)));
        ev.ump = make_note(ev.ump, true, ev.ump.note_number(),
                           Velocity16::from7(static_cast<uint8_t>(mapped)));
      }
    }

    // ---- 3. Chord expansion (note-on/off) ----
    // ---- 4. Arpeggiator expansion (note-on only) ----
    // These fan-out stages produce the events that go into `out`. We compute the
    // expanded note set, then (optionally) arpeggiate, then apply time shaping
    // (quantize/humanize) per emitted event.

    // Gather the base notes to emit (chord or just the single note).
    std::array<uint8_t, ChordConfig::kMaxChordNotes> notes{};
    size_t note_count = 0;
    const uint8_t base_note = ev.ump.note_number();
    const bool can_fan_out = note_on || note_off || note_addressed;
    if (chord_.enabled && can_fan_out && chord_.count > 0) {
      for (size_t c = 0; c < chord_.count && c < ChordConfig::kMaxChordNotes; ++c) {
        notes[note_count++] = static_cast<uint8_t>(clamp_note(
            static_cast<int64_t>(base_note) + static_cast<int64_t>(chord_.intervals[c])));
      }
    } else {
      notes[note_count++] = base_note;
    }

    auto shape_and_push = [&](MidiEvent shaped, size_t ordinal) noexcept {
      const int64_t original_frame = shaped.render_frame;
      ActiveNoteTiming paired_timing{};
      const bool paired_note_off = shaped.ump.is_note_off() && active_note_timing_count_ > 0 &&
                                   pop_note_timing(shaped, from_clip, clip_id, &paired_timing);
      // ---- 5. Quantize (render frame) ----
      if (paired_note_off) {
        // A note's gate is a duration, not a second grid event. Apply the exact
        // shift chosen for its note-on so a short note cannot quantize to
        // off-before-on (or off/on at the same frame after sorting).
        shaped.render_frame = numeric::saturating_add(original_frame, paired_timing.frame_shift);
        if (shaped.render_frame <= paired_timing.shaped_on_frame) {
          shaped.render_frame = numeric::saturating_add(paired_timing.shaped_on_frame, int64_t{1});
        }
      } else if (shaped.ump.is_note_on() && quantize_.enabled && quantize_.grid_frames > 0) {
        // Only note-ons snap; controllers and other events keep their position.
        const int64_t grid = quantize_.grid_frames;
        const int64_t f = shaped.render_frame;
        const int64_t snapped = numeric::saturating_add(
            nearest_quantized_frame(numeric::saturating_sub(f, grid_origin_frame), grid, quantize_),
            grid_origin_frame);
        const float strength = clamp01(quantize_.strength);
        if (strength <= 0.0f) {
          shaped.render_frame = f;
        } else if (strength >= 1.0f) {
          shaped.render_frame = snapped;
        } else {
          bool delta_overflow = false;
          if (f < 0 && snapped > std::numeric_limits<int64_t>::max() + f) {
            delta_overflow = true;
          } else if (f > 0 && snapped < std::numeric_limits<int64_t>::min() + f) {
            delta_overflow = true;
          }
          if (delta_overflow) {
            const double position =
                static_cast<double>(f) * (1.0 - strength) + static_cast<double>(snapped) * strength;
            shaped.render_frame = rounded_frame(position);
          } else {
            const int64_t delta = snapped - f;
            const int64_t adjustment = rounded_frame(static_cast<double>(delta) * strength);
            shaped.render_frame = numeric::saturating_add(f, adjustment);
          }
        }
      }
      // ---- 6. Humanize (timing + velocity), deterministic from seed ----
      if (humanize_.enabled) {
        HumanizeRandom rng(seed_for(humanize_.seed, ordinal));
        if (!paired_note_off && humanize_.timing_frames > 0) {
          // Apply the deterministic timing jitter, but never move an event to a
          // negative render frame. A hard clamp-to-0 would collapse several
          // distinct source frames onto 0 and could reorder events (e.g. a
          // note-off landing before its note-on). Instead, a negative-going jitter
          // that would push the frame below the origin is SKIPPED, leaving the
          // event at its original (non-negative) frame; this preserves the
          // relative order of all events. Positive jitter is always applied.
          const int64_t jitter = rng.symmetric(humanize_.timing_frames);
          const int64_t moved = numeric::saturating_add(shaped.render_frame, jitter);
          if (jitter >= 0 || moved >= 0) {
            shaped.render_frame = moved;
          }
        }
        if (humanize_.velocity_amount > 0 && is_channel_voice(shaped.ump) &&
            shaped.ump.is_note_on()) {
          // velocity_amount is expressed in 7-bit velocity units. MIDI 2.0 notes
          // are jittered in the full 16-bit domain (no 7-bit round-trip) so the
          // resolution is preserved, matching the velocity-curve stage above;
          // MIDI 1.0 notes stay in the 7-bit domain.
          const int64_t jitter7 = rng.symmetric(humanize_.velocity_amount);
          if (shaped.ump.message_type() == UmpMessageType::kMidi2ChannelVoice) {
            const int64_t jitter16 = jitter7 * 65535 / 127;
            const int v16 =
                clamp_velocity16_on(static_cast<int64_t>(note_velocity16(shaped.ump)) + jitter16);
            shaped.ump = make_midi2_note_on_velocity16(shaped.ump, shaped.ump.note_number(),
                                                       static_cast<uint16_t>(v16));
          } else {
            const int v =
                clamp_velocity_on(static_cast<int64_t>(note_velocity7(shaped.ump)) + jitter7);
            shaped.ump = make_note(shaped.ump, true, shaped.ump.note_number(),
                                   Velocity16::from7(static_cast<uint8_t>(v)));
          }
        }
      }
      const bool moves_time = (quantize_.enabled && quantize_.grid_frames > 0) ||
                              (humanize_.enabled && humanize_.timing_frames > 0);
      if (shaped.ump.is_note_on() && moves_time) {
        push_note_timing(shaped, numeric::saturating_sub(shaped.render_frame, original_frame),
                         shaped.render_frame, from_clip, clip_id);
      }
      push_or_overflow(shaped, out);
    };

    if (note_on && arpeggiator_.enabled && arpeggiator_.steps > 0 && arpeggiator_.step_frames > 0) {
      // Arpeggiate: walk the steps, emitting a gated note-on/off pair per step,
      // for each chord note. The source note-off is consumed (replaced by the
      // arpeggiated gates).
      const int64_t gate =
          arpeggiator_.gate_frames > 0 && arpeggiator_.gate_frames <= arpeggiator_.step_frames
              ? arpeggiator_.gate_frames
              : arpeggiator_.step_frames;
      size_t ordinal = input_ordinal;
      for (size_t n = 0; n < note_count; ++n) {
        for (size_t s = 0; s < arpeggiator_.steps && s < ArpeggiatorConfig::kMaxArpSteps; ++s) {
          const int arp_note = clamp_note(static_cast<int64_t>(notes[n]) +
                                          static_cast<int64_t>(arpeggiator_.intervals[s]));
          const int64_t onset = numeric::saturating_add(
              ev.render_frame,
              saturated_product_positive(static_cast<int64_t>(s), arpeggiator_.step_frames));

          MidiEvent on_ev;
          on_ev.render_frame = onset;
          on_ev.source_track_id = ev.source_track_id;
          // Preserve the source velocity at its native resolution (full 16-bit
          // for MIDI 2.0) instead of round-tripping through 7 bits.
          if (!retune_note_event(ev.ump, static_cast<uint8_t>(arp_note),
                                 static_cast<int64_t>(arp_note) - static_cast<int64_t>(source_note),
                                 &on_ev.ump)) {
            // A generated voice whose absolute pitch cannot be represented has
            // no matching gate to release, so discard the complete gate pair.
            continue;
          }
          // A generated gate is one logical note. Its on/off pair must share
          // timing jitter; deriving separate jitter from consecutive ordinals
          // could place the off before the on for short gates, leaving a hung
          // note after chronological sorting.
          const size_t note_ordinal = ordinal++;
          shape_and_push(on_ev, note_ordinal);

          MidiEvent off_ev;
          off_ev.render_frame = numeric::saturating_add(onset, gate);
          off_ev.source_track_id = ev.source_track_id;
          off_ev.ump =
              make_note(ev.ump, false, static_cast<uint8_t>(arp_note), Velocity16::from_raw(0));
          shape_and_push(off_ev, note_ordinal);
        }
      }
    } else if (note_off && arpeggiator_.enabled && arpeggiator_.steps > 0 &&
               arpeggiator_.step_frames > 0) {
      // The matching source note-off was replaced by the arpeggiated gates;
      // drop it.
    } else if (can_fan_out) {
      // Non-arpeggiated note-addressed path: emit each chord/single note.
      for (size_t n = 0; n < note_count; ++n) {
        MidiEvent note_ev = ev;
        if (!retune_note_event(ev.ump, notes[n],
                               static_cast<int64_t>(notes[n]) - static_cast<int64_t>(source_note),
                               &note_ev.ump)) {
          continue;
        }
        shape_and_push(note_ev, input_ordinal);
      }
    } else {
      // Non-note channel-voice / non-channel-voice events pass through with only
      // time shaping applied.
      shape_and_push(ev, input_ordinal);
    }
  }

  // Quantize/humanize move events in render_frame and arp/chord fan-out emits in
  // production order, so `out` is not time-ordered after the per-event passes.
  // Stable-sort the whole block by render_frame with the shared off-before-on
  // tiebreak (same contract as MidiClip / live render events), so every consumer
  // -- sequencer, AU/external MIDI ports -- receives in-block time order without
  // re-sorting downstream.
  sort_render_events_stable(out->events.data(), out->size);
}

}  // namespace sonare::midi
