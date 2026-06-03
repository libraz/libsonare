#include "midi/clock_sync.h"

#include <cmath>

#include "midi/tick_conversion.h"

namespace sonare::midi {
namespace {

bool valid_mtc_time(const MtcTime& time) noexcept {
  return time.hours < 24 && time.minutes < 60 && time.seconds < 60 &&
         time.frames < static_cast<uint8_t>(mtc_fps(time.rate));
}

void advance_mtc_frames(MtcTime* time, int frames) noexcept {
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
  const double ppq = tempo_map_->sample_to_ppq(frame);
  const double ticks = clock_ppq_to_ticks(ppq);
  int64_t tick = static_cast<int64_t>(std::ceil(ticks));
  if (tick < 0) {
    tick = 0;
  }
  // Guard against ceil rounding placing the tick before `frame`.
  while (tick > 0 && frame_of_tick(tick - 1) >= frame) {
    --tick;
  }
  while (frame_of_tick(tick) < frame) {
    ++tick;
  }
  return tick;
}

size_t ClockGenerator::generate_clock_block(int64_t block_start_frame, int num_frames,
                                            ClockByteOutput* out) noexcept {
  if (out == nullptr) {
    return 0;
  }
  out->clear();
  if (tempo_map_ == nullptr || num_frames <= 0) {
    return 0;
  }
  const int64_t block_end_frame = block_start_frame + num_frames;
  size_t ticks_emitted = 0;
  for (int64_t tick = first_tick_at_or_after(block_start_frame);
       frame_of_tick(tick) < block_end_frame; ++tick) {
    if (out->size >= ClockByteOutput::kCapacity) {
      out->overflowed = true;
      overflow_count_.bump();
      // Keep scanning so overflow_count reflects every dropped tick.
      continue;
    }
    out->bytes[out->size++] = kStatusClock;
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
  mtc_pieces_.fill(0);
  mtc_seen_.fill(false);
  mtc_complete_ = false;
  mtc_time_ = MtcTime{};
}

void ClockParser::assemble_mtc() noexcept {
  for (bool seen : mtc_seen_) {
    if (!seen) {
      return;
    }
  }
  MtcTime t;
  t.frames = static_cast<uint8_t>((mtc_pieces_[0] & 0x0Fu) | ((mtc_pieces_[1] & 0x01u) << 4u));
  t.seconds = static_cast<uint8_t>((mtc_pieces_[2] & 0x0Fu) | ((mtc_pieces_[3] & 0x03u) << 4u));
  t.minutes = static_cast<uint8_t>((mtc_pieces_[4] & 0x0Fu) | ((mtc_pieces_[5] & 0x03u) << 4u));
  t.hours = static_cast<uint8_t>((mtc_pieces_[6] & 0x0Fu) | ((mtc_pieces_[7] & 0x01u) << 4u));
  t.rate = static_cast<MtcFrameRate>((mtc_pieces_[7] >> 1u) & 0x03u);
  if (!valid_mtc_time(t)) {
    mtc_complete_ = false;
    return;
  }
  mtc_time_ = t;
  mtc_complete_ = true;
}

bool ClockParser::parse_byte(uint8_t byte) noexcept {
  // Status bytes (0x80+) interrupt any pending data assembly.
  if (byte & 0x80u) {
    switch (byte) {
      case kStatusClock:
        pending_ = Pending::kNone;
        ++clock_ticks_;
        return true;
      case kStatusStart:
      case kStatusContinue:
        pending_ = Pending::kNone;
        clock_ticks_ = 0;
        return true;
      case kStatusStop:
        pending_ = Pending::kNone;
        return true;
      case kStatusSongPosition:
        pending_ = Pending::kSppLsb;
        return false;
      case kStatusMtcQuarterFrame:
        pending_ = Pending::kMtcData;
        return false;
      default:
        // Other system real-time bytes pass through without changing state.
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
      if (piece == 0) {
        mtc_seen_.fill(false);
        mtc_complete_ = false;
      }
      mtc_pieces_[static_cast<size_t>(piece)] = nibble;
      mtc_seen_[static_cast<size_t>(piece)] = true;
      pending_ = Pending::kNone;
      if (piece == 7) {
        assemble_mtc();
        return mtc_complete_;
      }
      return false;
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
