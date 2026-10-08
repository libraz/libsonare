#include "midi/clock_sync.h"

#include <cmath>
#include <limits>

#include "midi/tick_conversion.h"
#include "util/numeric_validation.h"

namespace sonare::midi {
namespace {

bool valid_mtc_time(const MtcTime& time) noexcept {
  return time.hours < 24 && time.minutes < 60 && time.seconds < 60 &&
         time.frames < static_cast<uint8_t>(mtc_fps(time.rate));
}

void advance_mtc_frames(MtcTime* time, int frames) noexcept {
  // Advances by integer frame counting. For kFps29_97Drop this rolls over at a
  // true 30 frames/second (mtc_fps reports 30): it does NOT skip drop-frame
  // numbers (frames 0/1 at each non-tenth minute) nor apply the ~0.1% 29.97
  // pulldown, so generated drop-frame timecode tracks frame count, not broadcast
  // wall-clock. Drop-frame compensation is a caller concern; add it here only
  // when a consumer that needs broadcast-accurate drop-frame is wired up.
  if (time == nullptr || frames <= 0 || !valid_mtc_time(*time)) {
    return;
  }
  const int fps = mtc_fps(time->rate);
  int total = static_cast<int>(time->frames) + frames;
  time->frames = static_cast<uint8_t>(total % fps);
  total /= fps;
  if (total == 0) {
    return;
  }
  total += static_cast<int>(time->seconds);
  time->seconds = static_cast<uint8_t>(total % 60);
  total /= 60;
  if (total == 0) {
    return;
  }
  total += static_cast<int>(time->minutes);
  time->minutes = static_cast<uint8_t>(total % 60);
  total /= 60;
  if (total == 0) {
    return;
  }
  time->hours = static_cast<uint8_t>((static_cast<int>(time->hours) + total) % 24);
}

}  // namespace

int mtc_fps(MtcFrameRate rate) noexcept {
  switch (rate) {
    case MtcFrameRate::kFps24:
      return 24;
    case MtcFrameRate::kFps25:
      return 25;
    case MtcFrameRate::kFps29_97Drop:
    case MtcFrameRate::kFps30:
      return 30;
  }
  return 25;
}

size_t encode_spp(uint16_t midi_beats, uint8_t* out, size_t cap) noexcept {
  if (out == nullptr || cap < 3) {
    return 0;
  }
  const uint16_t value = static_cast<uint16_t>(midi_beats & 0x3FFFu);
  out[0] = kStatusSongPosition;
  out[1] = static_cast<uint8_t>(value & 0x7Fu);          // LSB (7 bits)
  out[2] = static_cast<uint8_t>((value >> 7u) & 0x7Fu);  // MSB (7 bits)
  return 3;
}

uint16_t ppq_to_spp_beats(double ppq) noexcept {
  if (ppq < 0.0) {
    ppq = 0.0;
  }
  // One sixteenth note = 0.25 quarter notes -> SPP beat units.
  const double beats = ppq / 0.25;
  const double truncated = std::floor(beats);
  if (truncated >= 16383.0) {
    return 0x3FFFu;
  }
  return static_cast<uint16_t>(truncated);
}

double spp_beats_to_ppq(uint16_t midi_beats) noexcept {
  return static_cast<double>(midi_beats & 0x3FFFu) * 0.25;
}

size_t encode_mtc_quarter_frame(const MtcTime& time, int piece, uint8_t* out, size_t cap) noexcept {
  if (out == nullptr || cap < 2 || piece < 0 || piece > 7 || !valid_mtc_time(time)) {
    return 0;
  }
  uint8_t nibble = 0;
  switch (piece) {
    case 0:
      nibble = static_cast<uint8_t>(time.frames & 0x0Fu);
      break;
    case 1:
      nibble = static_cast<uint8_t>((time.frames >> 4u) & 0x01u);
      break;
    case 2:
      nibble = static_cast<uint8_t>(time.seconds & 0x0Fu);
      break;
    case 3:
      nibble = static_cast<uint8_t>((time.seconds >> 4u) & 0x03u);
      break;
    case 4:
      nibble = static_cast<uint8_t>(time.minutes & 0x0Fu);
      break;
    case 5:
      nibble = static_cast<uint8_t>((time.minutes >> 4u) & 0x03u);
      break;
    case 6:
      nibble = static_cast<uint8_t>(time.hours & 0x0Fu);
      break;
    case 7:
      // High nibble of hours (1 bit) plus the 2-bit frame-rate code shifted in.
      nibble = static_cast<uint8_t>(((time.hours >> 4u) & 0x01u) |
                                    ((static_cast<uint8_t>(time.rate) & 0x03u) << 1u));
      break;
    default:
      return 0;
  }
  out[0] = kStatusMtcQuarterFrame;
  out[1] = static_cast<uint8_t>((static_cast<uint8_t>(piece) << 4u) | (nibble & 0x0Fu));
  return 2;
}

size_t encode_mtc_full_frame(const MtcTime& time, uint8_t device_id, uint8_t* out,
                             size_t cap) noexcept {
  if (out == nullptr || cap < 10 || !valid_mtc_time(time)) {
    return 0;
  }
  out[0] = 0xF0u;
  out[1] = 0x7Fu;
  out[2] = static_cast<uint8_t>(device_id & 0x7Fu);
  out[3] = 0x01u;
  out[4] = 0x01u;
  out[5] = static_cast<uint8_t>(((static_cast<uint8_t>(time.rate) & 0x03u) << 5u) |
                                (time.hours & 0x1Fu));
  out[6] = static_cast<uint8_t>(time.minutes & 0x3Fu);
  out[7] = static_cast<uint8_t>(time.seconds & 0x3Fu);
  out[8] = static_cast<uint8_t>(time.frames & 0x1Fu);
  out[9] = 0xF7u;
  return 10;
}

size_t encode_transport_command(uint8_t status, uint8_t* out, size_t cap) noexcept {
  if (out == nullptr || cap < 1) {
    return 0;
  }
  if (status != kStatusStart && status != kStatusContinue && status != kStatusStop) {
    return 0;
  }
  out[0] = status;
  return 1;
}

bool MtcQuarterFrameGenerator::reset(const MtcTime& start, int next_piece) noexcept {
  if (!valid_mtc_time(start) || next_piece < 0 || next_piece > 7) {
    return false;
  }
  time_ = start;
  next_piece_ = next_piece;
  return true;
}

size_t MtcQuarterFrameGenerator::next(uint8_t* out, size_t cap) noexcept {
  const int piece = next_piece_;
  const size_t written = encode_mtc_quarter_frame(time_, piece, out, cap);
  if (written == 0) {
    return 0;
  }
  next_piece_ = (next_piece_ + 1) & 0x07;
  if (next_piece_ == 0) {
    advance_mtc_frames(&time_, 2);
  }
  return written;
}

int64_t ClockGenerator::frame_of_tick(int64_t tick) const noexcept {
  if (tempo_map_ == nullptr) {
    return 0;
  }
  const double ppq = clock_ticks_to_ppq(tick);
  return tempo_map_->ppq_to_sample(ppq);
}

int64_t ClockGenerator::first_tick_at_or_after(int64_t frame) const noexcept {
  if (tempo_map_ == nullptr) {
    return 0;
  }
  constexpr int64_t kMaxTick = std::numeric_limits<int64_t>::max();
  // frame_of_tick never decreases, so the answer is bracketed by doubling steps
  // from the estimate and then bisected: bounded work whatever the tempo.
  const auto reaches = [&](int64_t tick) noexcept { return frame_of_tick(tick) >= frame; };
  const double estimate = std::ceil(clock_ppq_to_ticks(tempo_map_->sample_to_ppq(frame)));
  constexpr double kMaxTickDouble = static_cast<double>(kMaxTick);
  int64_t hi = !(estimate > 0.0)            ? 0
               : estimate >= kMaxTickDouble ? kMaxTick
                                            : static_cast<int64_t>(estimate);
  int64_t lo = -1;  // a tick before `frame`, or -1 when none is known
  if (reaches(hi)) {
    for (int64_t step = 1; hi > 0; step = numeric::saturating_add(step, step)) {
      const int64_t candidate = std::max<int64_t>(0, numeric::saturating_sub(hi, step));
      if (!reaches(candidate)) {
        lo = candidate;
        break;
      }
      hi = candidate;
    }
    if (lo < 0) return hi;
  } else {
    lo = hi;
    for (int64_t step = 1;; step = numeric::saturating_add(step, step)) {
      if (lo == kMaxTick) return kMaxTick;
      const int64_t candidate = numeric::saturating_add(lo, step);
      if (reaches(candidate)) {
        hi = candidate;
        break;
      }
      lo = candidate;
    }
  }
  while (hi - lo > 1) {
    const int64_t mid = lo + (hi - lo) / 2;
    if (reaches(mid)) {
      hi = mid;
    } else {
      lo = mid;
    }
  }
  return hi;
}

size_t ClockGenerator::generate_clock_block(int64_t block_start_frame, int num_frames,
                                            ClockByteOutput* out) noexcept {
  if (out == nullptr) {
    return 0;
  }
  out->clear();
  const auto append_byte = [](void* context, int64_t) noexcept {
    auto* output = static_cast<ClockByteOutput*>(context);
    output->bytes[output->size++] = kStatusClock;
  };
  bool overflowed = false;
  const size_t emitted =
      generate_clock_block(block_start_frame, num_frames, out, append_byte, &overflowed);
  out->overflowed = overflowed;
  return emitted;
}

size_t ClockGenerator::generate_clock_block(int64_t block_start_frame, int num_frames,
                                            void* context, TickSink sink,
                                            bool* overflowed) noexcept {
  if (overflowed != nullptr) *overflowed = false;
  if (sink == nullptr) return 0;
  if (tempo_map_ == nullptr || num_frames <= 0) {
    return 0;
  }
  const int64_t block_end_frame =
      numeric::saturating_add(block_start_frame, static_cast<int64_t>(num_frames));
  size_t ticks_emitted = 0;
  for (int64_t tick = first_tick_at_or_after(block_start_frame);
       frame_of_tick(tick) < block_end_frame; ++tick) {
    if (ticks_emitted >= ClockByteOutput::kCapacity) {
      if (overflowed != nullptr) *overflowed = true;
      overflow_count_.bump();
      // A hostile/internal tempo map must not turn fixed-output overflow into
      // unbounded audio-thread work. The counter records the overflow
      // occurrence; surplus ticks in this block are intentionally dropped.
      break;
    }
    sink(context, frame_of_tick(tick));
    ++ticks_emitted;
  }
  return ticks_emitted;
}

void ClockParser::reset() noexcept {
  pending_ = Pending::kNone;
  spp_lsb_ = 0;
  has_spp_ = false;
  spp_beats_ = 0;
  clock_ticks_ = 0;
  running_ = true;
  mtc_pieces_.fill(0);
  mtc_next_piece_ = -1;
  mtc_complete_ = false;
  mtc_time_ = MtcTime{};
}

bool ClockParser::assemble_mtc() noexcept {
  MtcTime t;
  t.frames = static_cast<uint8_t>((mtc_pieces_[0] & 0x0Fu) | ((mtc_pieces_[1] & 0x01u) << 4u));
  t.seconds = static_cast<uint8_t>((mtc_pieces_[2] & 0x0Fu) | ((mtc_pieces_[3] & 0x03u) << 4u));
  t.minutes = static_cast<uint8_t>((mtc_pieces_[4] & 0x0Fu) | ((mtc_pieces_[5] & 0x03u) << 4u));
  t.hours = static_cast<uint8_t>((mtc_pieces_[6] & 0x0Fu) | ((mtc_pieces_[7] & 0x01u) << 4u));
  t.rate = static_cast<MtcFrameRate>((mtc_pieces_[7] >> 1u) & 0x03u);
  if (!valid_mtc_time(t)) return false;
  mtc_time_ = t;
  mtc_complete_ = true;
  return true;
}

bool ClockParser::parse_byte(uint8_t byte) noexcept {
  // Status bytes (0x80+) interrupt any pending data assembly, EXCEPT the System
  // Real-Time range (0xF8..0xFF), which the MIDI 1.0 spec allows to appear
  // anywhere -- including between the status and data bytes of a System Common
  // message -- and which therefore has to leave the assembly alone.
  if (byte & 0x80u) {
    switch (byte) {
      case kStatusClock:
        // A clock tick may arrive between the status and data bytes of a System
        // Common message, so it must not disturb a half-assembled SPP or MTC:
        // pending_ is deliberately left alone here.
        if (running_) ++clock_ticks_;
        return true;
      case kStatusStart:
        // Every System Real-Time byte is transparent to System Common assembly,
        // not just the clock tick, so pending_ survives here too. Start begins
        // the sequence at its origin and discards any previous SPP anchor.
        has_spp_ = false;
        spp_beats_ = 0;
        clock_ticks_ = 0;
        running_ = true;
        return true;
      case kStatusContinue:
        // Continue resumes at the stopped position. It is still transparent to
        // a pending System Common message, but must leave both the SPP anchor
        // and accumulated clock ticks untouched.
        running_ = true;
        return true;
      case kStatusStop:
        running_ = false;
        return true;
      case kStatusSongPosition:
        pending_ = Pending::kSppLsb;
        return false;
      case kStatusMtcQuarterFrame:
        pending_ = Pending::kMtcData;
        return false;
      default:
        // Everything else. Only System Real-Time (0xF8..0xFF) is transparent to
        // a half-assembled System Common message -- the cases above are the ones
        // of those this parser acts on, and the rest (0xF9, 0xFD, Active Sensing
        // 0xFE, System Reset 0xFF) are equally transparent and equally ignored.
        //
        // Any OTHER status byte ends the message being assembled. A Channel
        // Voice status or an unhandled System Common arriving after an 0xF2 used
        // to leave pending_ set, so the next two data bytes -- which belong to
        // the new message -- were eaten by the SPP state machine and published
        // as a song position nobody sent.
        if (byte < 0xF8u) {
          pending_ = Pending::kNone;
        }
        return false;
    }
  }

  // Data byte: complete a pending two-/one-byte common message.
  switch (pending_) {
    case Pending::kSppLsb:
      spp_lsb_ = static_cast<uint8_t>(byte & 0x7Fu);
      pending_ = Pending::kSppMsb;
      return false;
    case Pending::kSppMsb: {
      const uint16_t beats =
          static_cast<uint16_t>(spp_lsb_ | (static_cast<uint16_t>(byte & 0x7Fu) << 7u));
      spp_beats_ = beats;
      has_spp_ = true;
      clock_ticks_ = 0;  // SPP repositions; tick count restarts from the anchor.
      pending_ = Pending::kNone;
      return true;
    }
    case Pending::kMtcData: {
      const int piece = (byte >> 4u) & 0x07u;
      const uint8_t nibble = static_cast<uint8_t>(byte & 0x0Fu);
      pending_ = Pending::kNone;
      // A cycle starts at piece 0 and accepts only the next piece in order.
      if (piece == 0) mtc_next_piece_ = 0;
      if (piece != mtc_next_piece_) {
        mtc_next_piece_ = -1;
        return false;
      }
      mtc_pieces_[static_cast<size_t>(piece)] = nibble;
      if (piece < 7) {
        ++mtc_next_piece_;
        return false;
      }
      mtc_next_piece_ = -1;
      return assemble_mtc();
    }
    case Pending::kNone:
    default:
      return false;
  }
}

double ClockParser::position_ppq() const noexcept {
  const double anchor = has_spp_ ? spp_beats_to_ppq(spp_beats_) : 0.0;
  return anchor + clock_ticks_to_ppq(clock_ticks_);
}

}  // namespace sonare::midi
