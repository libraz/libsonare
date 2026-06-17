#include "midi/midi_fx.h"

#include <cmath>
#include <cstdint>

namespace sonare::midi {
namespace {

// ---------------------------------------------------------------------------
// Deterministic PRNG: SplitMix64.
//
// We use SplitMix64 (a public-domain mixing function by Sebastiano Vigna) as the
// per-event deterministic generator. It is tiny, alloc-free, branch-free and
// produces well-distributed 64-bit outputs from a 64-bit state, which makes it
// ideal for RT-safe humanize jitter: the same seed always yields the same
// sequence, and no std::rand / clock is involved.
class SplitMix64 {
 public:
  explicit SplitMix64(uint64_t seed) noexcept : state_(seed) {}

  uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ull;
    uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  // Uniform integer in [-amount, +amount]. amount must be >= 0.
  int64_t symmetric(int64_t amount) noexcept {
    if (amount <= 0) return 0;
    const uint64_t span = static_cast<uint64_t>(amount) * 2ull + 1ull;
    const int64_t v = static_cast<int64_t>(next() % span);
    return v - amount;
  }

 private:
  uint64_t state_;
};

// Mix the chain seed with an event ordinal so each event in the block gets an
// independent (but deterministic) jitter draw.
uint64_t seed_for(uint32_t base_seed, size_t ordinal) noexcept {
  return (static_cast<uint64_t>(base_seed) << 32) ^ (0xD1B54A32D192ED03ull * (ordinal + 1));
}

int clamp_note(int note) noexcept {
  if (note < 0) return 0;
  if (note > 127) return 127;
  return note;
}

int clamp_velocity_on(int velocity) noexcept {
  // A note-on keeps at least velocity 1 so it never degrades into a note-off.
  if (velocity < 1) return 1;
  if (velocity > 127) return 127;
  return velocity;
}

int clamp_velocity16_on(int velocity) noexcept {
  // MIDI 2.0 equivalent of clamp_velocity_on: keep at least 1 (never a
  // note-off) and cap at the full 16-bit range.
  if (velocity < 1) return 1;
  if (velocity > 65535) return 65535;
  return velocity;
}

float clamp01(float v) noexcept {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

float clamp_groove_offset(float v) noexcept {
  if (!std::isfinite(v)) return 0.0f;
  if (v < -1.0f) return -1.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

size_t bounded_groove_steps(const QuantizeConfig& config) noexcept {
  return config.groove_steps > QuantizeConfig::kMaxGrooveSteps ? QuantizeConfig::kMaxGrooveSteps
                                                               : config.groove_steps;
}

size_t groove_index(int64_t line, size_t steps) noexcept {
  if (steps == 0) return 0;
  const int64_t s = static_cast<int64_t>(steps);
  int64_t idx = line % s;
  if (idx < 0) idx += s;
  return static_cast<size_t>(idx);
}

int64_t swung_grid_frame(int64_t line, int64_t grid, const QuantizeConfig& config) noexcept {
  int64_t frame = line * grid;
  const float swing = clamp01(config.swing);
  if ((line & 1ll) != 0) {
    frame += static_cast<int64_t>(
        std::llround(static_cast<double>(grid) * 0.5 * static_cast<double>(swing)));
  }
  const size_t steps = bounded_groove_steps(config);
  if (steps > 0) {
    const float offset = clamp_groove_offset(config.groove_offsets[groove_index(line, steps)]);
    frame +=
        static_cast<int64_t>(std::llround(static_cast<double>(grid) * static_cast<double>(offset)));
  }
  return frame;
}

int64_t abs_i64(int64_t v) noexcept { return v < 0 ? -v : v; }

int64_t nearest_quantized_frame(int64_t frame, int64_t grid,
                                const QuantizeConfig& config) noexcept {
  if (grid <= 0) return frame;
  const int64_t base = frame >= 0 ? frame / grid : -(((-frame) + grid - 1) / grid);
  int64_t best = swung_grid_frame(base - 2, grid, config);
  int64_t best_dist = abs_i64(frame - best);
  for (int64_t line = base - 1; line <= base + 3; ++line) {
    const int64_t candidate = swung_grid_frame(line, grid, config);
    const int64_t dist = abs_i64(frame - candidate);
    if (dist < best_dist || (dist == best_dist && candidate < best)) {
      best = candidate;
      best_dist = dist;
    }
  }
  return best;
}

bool is_midi_channel_voice(const Ump& ump) noexcept {
  const UmpMessageType mt = ump.message_type();
  return mt == UmpMessageType::kMidi1ChannelVoice || mt == UmpMessageType::kMidi2ChannelVoice;
}

bool is_midi2_per_note_controller_form(const Ump& ump) noexcept {
  if (ump.message_type() != UmpMessageType::kMidi2ChannelVoice) return false;
  const uint8_t status = ump.status_nibble();
  return status == static_cast<uint8_t>(UmpStatus::kRegisteredPerNoteController) ||
         status == static_cast<uint8_t>(UmpStatus::kAssignablePerNoteController);
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

Ump make_note(const Ump& src, bool note_on, uint8_t note, uint8_t velocity7) noexcept {
  if (src.message_type() == UmpMessageType::kMidi2ChannelVoice) {
    const uint16_t velocity16 = scale_velocity_7_to_16(velocity7);
    if (note_on) {
      const uint8_t attr_type = static_cast<uint8_t>(src.words[0] & 0xFFu);
      const uint16_t attr_data = static_cast<uint16_t>(src.words[1] & 0xFFFFu);
      return make_midi2_note_on(src.group, src.channel(), note, velocity16, attr_type, attr_data);
    }
    return make_midi2_note_off(src.group, src.channel(), note, velocity16);
  }
  if (note_on) {
    return make_midi1_note_on(src.group, src.channel(), note, velocity7);
  }
  return make_midi1_note_off(src.group, src.channel(), note, velocity7);
}

Ump make_note_preserving_velocity(const Ump& src, bool note_on, uint8_t note) noexcept {
  if (src.message_type() == UmpMessageType::kMidi2ChannelVoice) {
    const uint16_t velocity16 = static_cast<uint16_t>(src.words[1] >> 16u);
    if (note_on) {
      const uint8_t attr_type = static_cast<uint8_t>(src.words[0] & 0xFFu);
      const uint16_t attr_data = static_cast<uint16_t>(src.words[1] & 0xFFFFu);
      return make_midi2_note_on(src.group, src.channel(), note, velocity16, attr_type, attr_data);
    }
    return make_midi2_note_off(src.group, src.channel(), note, velocity16);
  }
  return make_note(src, note_on, note, midi1_velocity7(src));
}

Ump make_per_note_controller_preserving_value(const Ump& src, uint8_t note) noexcept {
  const uint8_t index = static_cast<uint8_t>(src.words[0] & 0xFFu);
  const uint32_t value = src.words[1];
  if (src.status_nibble() == static_cast<uint8_t>(UmpStatus::kAssignablePerNoteController)) {
    return make_midi2_assignable_per_note_controller(src.group, src.channel(), note, index, value);
  }
  return make_midi2_per_note_controller(src.group, src.channel(), note, index, value);
}

}  // namespace

void MidiFxChain::push_or_overflow(const MidiEvent& ev, MidiFxBuffer* out) noexcept {
  if (!out->push(ev)) {
    overflow_count_.bump();
  }
}

void MidiFxChain::process(const MidiEvent* in, size_t count, MidiFxBuffer* out) noexcept {
  if (in == nullptr || out == nullptr) return;
  out->clear();

  for (size_t i = 0; i < count; ++i) {
    MidiEvent ev = in[i];
    const bool channel_voice = is_midi_channel_voice(ev.ump);
    const bool note_on = channel_voice && ev.ump.is_note_on();
    const bool note_off = channel_voice && ev.ump.is_note_off();
    const bool per_note_controller = channel_voice && is_midi2_per_note_controller_form(ev.ump);

    // ---- 1. Transpose (note-on/off and MIDI 2.0 per-note controller note) ----
    if (transpose_.enabled && (note_on || note_off)) {
      const int shifted = clamp_note(static_cast<int>(ev.ump.note_number()) + transpose_.semitones);
      ev.ump = make_note_preserving_velocity(ev.ump, note_on, static_cast<uint8_t>(shifted));
    } else if (transpose_.enabled && per_note_controller) {
      const int shifted = clamp_note(static_cast<int>(ev.ump.note_number()) + transpose_.semitones);
      ev.ump = make_per_note_controller_preserving_value(ev.ump, static_cast<uint8_t>(shifted));
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
      if (midi2) {
        const int mapped = clamp_velocity16_on(static_cast<int>(std::lround(shaped * 65535.0f)));
        ev.ump = make_midi2_note_on_velocity16(ev.ump, ev.ump.note_number(),
                                               static_cast<uint16_t>(mapped));
      } else {
        const int mapped = clamp_velocity_on(static_cast<int>(std::lround(shaped * 127.0f)));
        ev.ump = make_note(ev.ump, true, ev.ump.note_number(), static_cast<uint8_t>(mapped));
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
    if (chord_.enabled && (note_on || note_off || per_note_controller) && chord_.count > 0) {
      for (size_t c = 0; c < chord_.count && c < ChordConfig::kMaxChordNotes; ++c) {
        notes[note_count++] =
            static_cast<uint8_t>(clamp_note(static_cast<int>(base_note) + chord_.intervals[c]));
      }
    } else {
      notes[note_count++] = base_note;
    }

    auto shape_and_push = [&](MidiEvent shaped, size_t ordinal) noexcept {
      // ---- 5. Quantize (render frame) ----
      if (quantize_.enabled && quantize_.grid_frames > 0) {
        const int64_t grid = quantize_.grid_frames;
        const int64_t f = shaped.render_frame;
        const int64_t snapped = nearest_quantized_frame(f, grid, quantize_);
        const float strength = clamp01(quantize_.strength);
        const int64_t delta = snapped - f;
        shaped.render_frame = f + static_cast<int64_t>(std::llround(static_cast<double>(delta) *
                                                                    static_cast<double>(strength)));
      }
      // ---- 6. Humanize (timing + velocity), deterministic from seed ----
      if (humanize_.enabled) {
        SplitMix64 rng(seed_for(humanize_.seed, ordinal));
        if (humanize_.timing_frames > 0) {
          // Apply the deterministic timing jitter, but never move an event to a
          // negative render frame. A hard clamp-to-0 would collapse several
          // distinct source frames onto 0 and could reorder events (e.g. a
          // note-off landing before its note-on). Instead, a negative-going jitter
          // that would push the frame below the origin is SKIPPED, leaving the
          // event at its original (non-negative) frame; this preserves the
          // relative order of all events. Positive jitter is always applied.
          const int64_t jitter = rng.symmetric(humanize_.timing_frames);
          if (jitter >= 0 || shaped.render_frame + jitter >= 0) {
            shaped.render_frame += jitter;
          }
        }
        if (humanize_.velocity_amount > 0 && is_midi_channel_voice(shaped.ump) &&
            shaped.ump.is_note_on()) {
          const int jitter = static_cast<int>(rng.symmetric(humanize_.velocity_amount));
          const int v = clamp_velocity_on(static_cast<int>(note_velocity7(shaped.ump)) + jitter);
          shaped.ump =
              make_note(shaped.ump, true, shaped.ump.note_number(), static_cast<uint8_t>(v));
        }
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
      size_t ordinal = i;
      for (size_t n = 0; n < note_count; ++n) {
        for (size_t s = 0; s < arpeggiator_.steps && s < ArpeggiatorConfig::kMaxArpSteps; ++s) {
          const int arp_note = clamp_note(static_cast<int>(notes[n]) + arpeggiator_.intervals[s]);
          const int64_t onset =
              ev.render_frame + static_cast<int64_t>(s) * arpeggiator_.step_frames;

          MidiEvent on_ev;
          on_ev.render_frame = onset;
          // Preserve the source velocity at its native resolution (full 16-bit
          // for MIDI 2.0) instead of round-tripping through 7 bits.
          on_ev.ump = make_note_preserving_velocity(ev.ump, true, static_cast<uint8_t>(arp_note));
          shape_and_push(on_ev, ordinal++);

          MidiEvent off_ev;
          off_ev.render_frame = onset + gate;
          off_ev.ump = make_note(ev.ump, false, static_cast<uint8_t>(arp_note), 0);
          shape_and_push(off_ev, ordinal++);
        }
      }
    } else if (note_off && arpeggiator_.enabled && arpeggiator_.steps > 0 &&
               arpeggiator_.step_frames > 0) {
      // The matching source note-off was replaced by the arpeggiated gates;
      // drop it.
    } else if (note_on || note_off || per_note_controller) {
      // Non-arpeggiated note/per-note-controller path: emit each chord/single note.
      for (size_t n = 0; n < note_count; ++n) {
        MidiEvent note_ev = ev;
        if (per_note_controller) {
          note_ev.ump = make_per_note_controller_preserving_value(ev.ump, notes[n]);
        } else {
          // Preserve the source velocity at its native resolution (full 16-bit
          // for MIDI 2.0) instead of round-tripping through 7 bits.
          note_ev.ump = make_note_preserving_velocity(ev.ump, note_on, notes[n]);
        }
        shape_and_push(note_ev, i);
      }
    } else {
      // Non-note channel-voice / non-channel-voice events pass through with only
      // time shaping applied.
      shape_and_push(ev, i);
    }
  }
}

}  // namespace sonare::midi
