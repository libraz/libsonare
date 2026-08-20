#include "midi/smf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "midi/tick_conversion.h"
#include "midi/ump.h"
#include "util/constants.h"

namespace sonare::midi {
namespace {

// SMF chunk type tags.
constexpr uint8_t kMThd[4] = {'M', 'T', 'h', 'd'};
constexpr uint8_t kMTrk[4] = {'M', 'T', 'r', 'k'};

// SMF event marker bytes.
constexpr uint8_t kMetaPrefix = 0xFFu;
constexpr uint8_t kSysExStart = 0xF0u;
constexpr uint8_t kSysExEscape = 0xF7u;

// Recognized meta event type bytes (the byte following 0xFF).
constexpr uint8_t kMetaText = 0x01u;
constexpr uint8_t kMetaTrackName = 0x03u;
constexpr uint8_t kMetaLyric = 0x05u;
constexpr uint8_t kMetaMarker = 0x06u;
constexpr uint8_t kMetaCuePoint = 0x07u;
constexpr uint8_t kMetaEndOfTrack = 0x2Fu;
constexpr uint8_t kMetaSetTempo = 0x51u;
constexpr uint8_t kMetaTimeSignature = 0x58u;
constexpr uint8_t kMetaKeySignature = 0x59u;

// Renders an SMF key signature (sf = fifths -7..7, mi = 0 major / 1 minor) to a
// human-readable tonic name like "C major" / "A minor", used as a display
// fallback alongside the structured fields. Out-of-range fifths fall back to C.
std::string key_signature_name(int8_t fifths, bool minor) {
  // Circle of fifths from -7 (7 flats) to +7 (7 sharps).
  static constexpr const char* kMajor[15] = {"Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F", "C",
                                             "G",  "D",  "A",  "E",  "B",  "F#", "C#"};
  static constexpr const char* kMinor[15] = {"Ab", "Eb", "Bb", "F",  "C",  "G",  "D", "A",
                                             "E",  "B",  "F#", "C#", "G#", "D#", "A#"};
  const int idx = static_cast<int>(fifths) + 7;
  const char* tonic = (idx >= 0 && idx < 15) ? (minor ? kMinor[idx] : kMajor[idx]) : "C";
  return std::string(tonic) + (minor ? " minor" : " major");
}

// Microseconds per minute — the SMF set-tempo meta encodes microseconds per
// quarter note, so BPM = (us-per-minute) / (us-per-quarter). This is an SMF
// domain constant, not a universal numeric constant.
constexpr double kMicrosPerMinute = 60000000.0;
constexpr double kDefaultBpm = sonare::constants::kDefaultBpm;

// SMF text meta events are byte strings and older files often contain a local
// code page rather than UTF-8. Project serialization exposes these values as
// JSON strings on every binding, so normalize once at import rather than
// leaking invalid bytes into a surface-specific JSON encoder.
std::string utf8_with_replacement(const uint8_t* bytes, size_t size) {
  constexpr char kReplacement[] = "\xEF\xBF\xBD";
  std::string out;
  out.reserve(size);
  for (size_t i = 0; i < size;) {
    const uint8_t first = bytes[i];
    size_t width = 0;
    if (first <= 0x7Fu) {
      out.push_back(static_cast<char>(first));
      ++i;
      continue;
    }
    if (first >= 0xC2u && first <= 0xDFu) {
      width = 2;
    } else if (first >= 0xE0u && first <= 0xEFu) {
      width = 3;
    } else if (first >= 0xF0u && first <= 0xF4u) {
      width = 4;
    }

    bool valid = width != 0 && i + width <= size;
    if (valid) {
      const uint8_t second = bytes[i + 1];
      valid = (second & 0xC0u) == 0x80u;
      if (width >= 3) valid = valid && (bytes[i + 2] & 0xC0u) == 0x80u;
      if (width == 4) valid = valid && (bytes[i + 3] & 0xC0u) == 0x80u;
      if (valid && first == 0xE0u) valid = second >= 0xA0u;
      if (valid && first == 0xEDu) valid = second <= 0x9Fu;
      if (valid && first == 0xF0u) valid = second >= 0x90u;
      if (valid && first == 0xF4u) valid = second <= 0x8Fu;
    }
    if (valid) {
      out.append(reinterpret_cast<const char*>(bytes + i), width);
      i += width;
    } else {
      out.append(kReplacement);
      ++i;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Byte-buffer reader with bounds checking. All reads validate remaining length
// and set `overflow` (never read out of bounds). Control-thread only.
// ---------------------------------------------------------------------------
class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  size_t remaining() const noexcept { return pos_ <= size_ ? size_ - pos_ : 0; }
  size_t pos() const noexcept { return pos_; }
  bool overflow() const noexcept { return overflow_; }

  uint8_t u8() noexcept {
    if (remaining() < 1) {
      overflow_ = true;
      return 0;
    }
    return data_[pos_++];
  }

  uint16_t u16() noexcept {
    const uint8_t hi = u8();
    const uint8_t lo = u8();
    return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
  }

  uint32_t u32() noexcept {
    const uint32_t b0 = u8();
    const uint32_t b1 = u8();
    const uint32_t b2 = u8();
    const uint32_t b3 = u8();
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
  }

  /// Reads a variable-length quantity (7 bits/byte, MSB = continuation). At most
  /// 4 bytes per the SMF spec; a 5th continuation byte is treated as overflow.
  uint32_t vlq() noexcept {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const uint8_t byte = u8();
      if (overflow_) return 0;
      value = (value << 7) | (byte & 0x7Fu);
      if ((byte & 0x80u) == 0) return value;
    }
    // A fifth byte with the continuation bit still set is malformed.
    overflow_ = true;
    return value;
  }

  /// Returns a pointer to `count` bytes and advances, or nullptr on overflow.
  const uint8_t* take(size_t count) noexcept {
    if (remaining() < count) {
      overflow_ = true;
      return nullptr;
    }
    const uint8_t* p = data_ + pos_;
    pos_ += count;
    return p;
  }

  const uint8_t* peek(size_t offset, size_t count) const noexcept {
    if (offset > remaining() || count > remaining() - offset) return nullptr;
    return data_ + pos_ + offset;
  }

  bool match_tag(const uint8_t (&tag)[4]) noexcept {
    const uint8_t* p = take(4);
    if (p == nullptr) return false;
    return p[0] == tag[0] && p[1] == tag[1] && p[2] == tag[2] && p[3] == tag[3];
  }

  /// Sets the read position directly, clamped to the buffer size. Used to
  /// resynchronize to a track chunk boundary after a trailing VLQ read
  /// crossed past it (the VLQ decoder only checks the overall buffer bound,
  /// not the current track's declared end).
  void seek(size_t pos) noexcept { pos_ = pos <= size_ ? pos : size_; }

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
  bool overflow_ = false;
};

// ---------------------------------------------------------------------------
// Byte-buffer writer helpers (export).
// ---------------------------------------------------------------------------
void put_u8(std::vector<uint8_t>* out, uint8_t v) { out->push_back(v); }

void put_u16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  out->push_back(static_cast<uint8_t>(v & 0xFFu));
}

void put_u32(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  out->push_back(static_cast<uint8_t>(v & 0xFFu));
}

void put_tag(std::vector<uint8_t>* out, const uint8_t (&tag)[4]) {
  out->insert(out->end(), tag, tag + 4);
}

void put_vlq(std::vector<uint8_t>* out, uint32_t value) {
  // An SMF variable-length quantity is at most 4 bytes (28 significant bits), so
  // clamp to the largest representable value. A 5th byte would carry a
  // continuation bit the reader rejects, making the stream unreadable to this
  // library and to spec-compliant DAWs.
  constexpr uint32_t kMaxVlq = 0x0FFFFFFFu;
  if (value > kMaxVlq) value = kMaxVlq;
  // Encode 7 bits/byte, big-endian, continuation bit on all but the last.
  std::array<uint8_t, 5> buf{};
  int n = 0;
  buf[n++] = static_cast<uint8_t>(value & 0x7Fu);
  while ((value >>= 7) != 0) {
    buf[n++] = static_cast<uint8_t>((value & 0x7Fu) | 0x80u);
  }
  for (int i = n - 1; i >= 0; --i) out->push_back(buf[i]);
}

// Decodes how many data bytes a MIDI 1.0 channel-voice status carries.
int channel_voice_data_count(uint8_t status) noexcept {
  switch (status & 0xF0u) {
    case 0xC0u:  // Program change.
    case 0xD0u:  // Channel pressure.
      return 1;
    case 0x80u:  // Note off.
    case 0x90u:  // Note on.
    case 0xA0u:  // Poly pressure.
    case 0xB0u:  // Control change.
    case 0xE0u:  // Pitch bend.
      return 2;
    default:
      return -1;
  }
}

int system_common_data_count(uint8_t status) noexcept {
  switch (status) {
    case 0xF1u:  // MIDI time-code quarter-frame.
    case 0xF3u:  // Song select.
      return 1;
    case 0xF2u:  // Song position pointer.
      return 2;
    case 0xF6u:  // Tune request.
      return 0;
    default:
      return -1;
  }
}

bool is_system_realtime(uint8_t status) noexcept { return status >= 0xF8u && status <= 0xFEu; }

// Imports a single MTrk chunk body of `length` bytes starting at the reader's
// current position. Appends parsed channel-voice events to `clip` (PPQ-timed),
// captures track name / markers / tempo / time-sig into the out params, and
// advances the reader to the chunk end. Returns false on malformed track data.
struct TrackParseState {
  MidiClip clip;
  std::string name;
  double length_ppq = 0.0;
  bool has_midi_events = false;
};

bool parse_track(Reader* reader, size_t length, uint16_t ppqn, TrackParseState* track,
                 std::vector<transport::TempoSegment>* tempos,
                 std::vector<transport::TimeSignatureSegment>* time_sigs,
                 std::vector<SmfMarker>* markers, SysExStore* sysex_store, uint32_t* skipped,
                 bool* timing_overflow, const resource::MidiImportResourceLimits& limits,
                 size_t* event_count, size_t* metadata_bytes, size_t* sysex_bytes,
                 bool* resource_exceeded, bool* track_truncated) {
  const size_t end_pos = reader->pos() + length;
  if (length > reader->remaining()) return false;

  uint64_t tick = 0;
  uint8_t running_status = 0;
  bool saw_end_of_track = false;
  std::vector<uint8_t> pending_sysex;
  double pending_sysex_ppq = 0.0;
  bool pending_sysex_active = false;

  // Bytes remaining before the declared track boundary. Every per-event read is
  // bounded by this so a corrupt in-track event length can never consume the
  // following track's bytes (which would corrupt it and fail the whole import);
  // on any overrun the track parse stops and the reader resynchronizes to
  // end_pos below, skipping / repairing just this track.
  const auto track_remaining = [&]() -> size_t {
    return reader->pos() < end_pos ? end_pos - reader->pos() : 0;
  };
  // Discards an unterminated split SysEx when a non-continuation event arrives,
  // so a later F7 packet cannot concatenate onto stale bytes.
  const auto discard_pending_sysex = [&]() {
    if (pending_sysex_active) {
      pending_sysex.clear();
      pending_sysex_active = false;
      ++(*skipped);
    }
  };
  const auto consume_event = [&]() {
    if (!resource::bounded_accumulate(1u, limits.max_events, event_count)) {
      *resource_exceeded = true;
      return false;
    }
    return true;
  };
  const auto mark_truncated = [&]() {
    if (!*track_truncated) {
      *track_truncated = true;
      ++(*skipped);
    }
  };

  while (reader->pos() < end_pos && !reader->overflow()) {
    const uint32_t delta = reader->vlq();
    if (reader->overflow()) return false;
    if (reader->pos() > end_pos) {
      mark_truncated();  // A VLQ delta ran past the track end.
      break;
    }
    if (tick > std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(delta)) {
      *timing_overflow = true;
      return false;
    }
    tick += static_cast<uint64_t>(delta);
    const double ppq = smf_ticks_to_ppq(tick, ppqn);

    if (track_remaining() == 0) {
      mark_truncated();  // No room for a status byte.
      break;
    }
    uint8_t status = reader->u8();
    if (reader->overflow()) return false;

    if (status == kMetaPrefix) {
      running_status = 0;
      discard_pending_sysex();
      if (track_remaining() < 1) {
        mark_truncated();  // No room for the meta type byte.
        break;
      }
      const uint8_t meta_type = reader->u8();
      const uint32_t meta_len = reader->vlq();
      if (reader->overflow()) return false;
      // Reject a meta payload whose declared length overruns the track boundary.
      if (reader->pos() > end_pos || meta_len > track_remaining()) {
        mark_truncated();
        break;
      }
      const uint8_t* payload = reader->take(meta_len);
      if (payload == nullptr) return false;

      switch (meta_type) {
        case kMetaSetTempo: {
          if (meta_len == 3) {
            if (!consume_event()) return false;
            const uint32_t us_per_quarter = (static_cast<uint32_t>(payload[0]) << 16) |
                                            (static_cast<uint32_t>(payload[1]) << 8) |
                                            static_cast<uint32_t>(payload[2]);
            const double bpm =
                us_per_quarter > 0 ? kMicrosPerMinute / static_cast<double>(us_per_quarter) : 0.0;
            if (transport::valid_public_tempo(bpm)) {
              transport::TempoSegment seg;
              seg.start_ppq = ppq;
              seg.bpm = bpm;
              tempos->push_back(seg);
            } else {
              ++(*skipped);
            }
          } else {
            ++(*skipped);
          }
          break;
        }
        case kMetaTimeSignature: {
          if (meta_len >= 2) {
            if (!consume_event()) return false;
            transport::TimeSignatureSegment seg;
            seg.start_ppq = ppq;
            seg.time_sig.numerator = static_cast<int>(payload[0]);
            // SMF stores the denominator as a power of two (2 => 2^2 = 4). The
            // exporter caps the exponent at 7 (128th-note denominator), so
            // reject any exponent it could not reproduce: this keeps
            // import -> export round-trips symmetric instead of silently
            // re-quantizing an oversized denominator down to 128 on write.
            // The numerator is required to be positive; do not pass zero into
            // the transport time-signature map, whose public invariant rejects
            // non-positive numerators.
            if (payload[0] == 0 || payload[1] > 7) {
              ++(*skipped);
              break;
            }
            seg.time_sig.denominator = 1 << payload[1];
            if (meta_len >= 4) {
              seg.clocks_per_metronome_click = payload[2];
              seg.thirty_seconds_per_quarter = payload[3];
            }
            time_sigs->push_back(seg);
          } else {
            ++(*skipped);
          }
          break;
        }
        case kMetaTrackName: {
          if (!resource::bounded_accumulate(meta_len, limits.max_metadata_bytes, metadata_bytes)) {
            *resource_exceeded = true;
            return false;
          }
          track->name = utf8_with_replacement(payload, meta_len);
          break;
        }
        case kMetaMarker:
        case kMetaText:
        case kMetaLyric:
        case kMetaCuePoint: {
          if (!consume_event() ||
              !resource::bounded_accumulate(meta_len, limits.max_metadata_bytes, metadata_bytes)) {
            *resource_exceeded = true;
            return false;
          }
          SmfMarker marker;
          marker.ppq = ppq;
          marker.text = utf8_with_replacement(payload, meta_len);
          marker.kind = meta_type == kMetaText       ? SmfMarkerKind::kText
                        : meta_type == kMetaLyric    ? SmfMarkerKind::kLyric
                        : meta_type == kMetaCuePoint ? SmfMarkerKind::kCuePoint
                                                     : SmfMarkerKind::kMarker;
          markers->push_back(std::move(marker));
          break;
        }
        case kMetaKeySignature: {
          if (meta_len >= 2) {
            if (!consume_event()) return false;
            SmfMarker marker;
            marker.ppq = ppq;
            marker.kind = SmfMarkerKind::kKeySignature;
            marker.key_fifths = static_cast<int8_t>(payload[0]);
            marker.key_minor = payload[1] != 0;
            marker.text = key_signature_name(marker.key_fifths, marker.key_minor);
            markers->push_back(std::move(marker));
          } else {
            ++(*skipped);
          }
          break;
        }
        case kMetaEndOfTrack:
          track->length_ppq = ppq;
          saw_end_of_track = true;
          break;
        default:
          ++(*skipped);
          break;
      }
      if (saw_end_of_track) break;
      continue;
    }

    if (status == kSysExStart || status == kSysExEscape) {
      running_status = 0;
      const uint32_t sysex_len = reader->vlq();
      if (reader->overflow()) return false;
      // Reject a SysEx payload whose declared length overruns the track boundary.
      if (reader->pos() > end_pos || sysex_len > track_remaining()) {
        mark_truncated();
        break;
      }
      const uint8_t* payload = reader->take(sysex_len);
      if (payload == nullptr) return false;
      if (!resource::bounded_accumulate(sysex_len, limits.max_sysex_bytes, sysex_bytes)) {
        *resource_exceeded = true;
        return false;
      }

      bool complete = false;
      if (status == kSysExStart) {
        // A new SysEx dump. Discard any still-pending split dump rather than
        // letting this one concatenate onto its unterminated bytes.
        if (pending_sysex_active) ++(*skipped);
        pending_sysex.assign(payload, payload + sysex_len);
        pending_sysex_ppq = ppq;
        complete = !pending_sysex.empty() && pending_sysex.back() == 0xF7u;
      } else if (pending_sysex_active) {
        // Continuation packet of an active split dump.
        pending_sysex.insert(pending_sysex.end(), payload, payload + sysex_len);
        complete = !pending_sysex.empty() && pending_sysex.back() == 0xF7u;
      } else {
        // Independent F7 escape event: its payload bytes are the complete byte
        // sequence to send. It need not be terminated by an F7 data byte.
        pending_sysex.assign(payload, payload + sysex_len);
        pending_sysex_ppq = ppq;
        complete = !pending_sysex.empty();
      }

      if (!complete) {
        pending_sysex_active = true;
        continue;
      }
      pending_sysex_active = false;

      if (!consume_event()) return false;
      const SysExHandle handle =
          sysex_store != nullptr ? sysex_store->add(pending_sysex) : SysExHandle{0};
      pending_sysex.clear();
      if (handle == 0) {
        ++(*skipped);
        continue;
      }
      MidiClipEvent ev;
      ev.ppq = pending_sysex_ppq;
      ev.ump = make_sysex_handle(/*group=*/0, handle);
      track->clip.add_event(ev);
      track->has_midi_events = true;
      continue;
    }

    if ((status & 0xF0u) == 0xF0u) {
      const int system_data = system_common_data_count(status);
      if (system_data > 0) {
        if (static_cast<size_t>(system_data) > track_remaining()) {
          mark_truncated();
          break;
        }
        if (reader->take(static_cast<size_t>(system_data)) == nullptr) return false;
        running_status = 0;
        discard_pending_sysex();
      } else if (system_data == 0) {
        running_status = 0;
        discard_pending_sysex();
      } else if (!is_system_realtime(status)) {
        running_status = 0;
        discard_pending_sysex();
      }
      ++(*skipped);
      continue;
    }

    // Channel-voice (possibly running status).
    discard_pending_sysex();
    uint8_t first_data = 0;
    bool have_first_data = false;
    if (status & 0x80u) {
      running_status = status;
    } else {
      // Running status: the byte we read was actually the first data byte; the
      // real status is the previously-seen running-status byte.
      if ((running_status & 0x80u) == 0) return false;  // No status to run with.
      first_data = status;
      have_first_data = true;
      status = running_status;
    }

    const int data_count = channel_voice_data_count(status);
    if (data_count < 0) {
      // System common / unknown — cannot safely resync; stop the track.
      ++(*skipped);
      return false;
    }

    // Assemble the up-to-3 raw MIDI bytes and parse via the ump.h adapter.
    std::array<uint8_t, 3> raw{};
    size_t raw_len = 0;
    raw[raw_len++] = status;
    int remaining_data = data_count;
    if (have_first_data) {
      raw[raw_len++] = first_data;
      --remaining_data;
    }
    if (static_cast<size_t>(remaining_data) > track_remaining()) {
      mark_truncated();
      break;
    }
    for (int i = 0; i < remaining_data; ++i) {
      raw[raw_len++] = reader->u8();
    }
    if (reader->overflow()) return false;

    Ump ump;
    uint8_t rs = status;
    const size_t consumed = midi1_bytes_to_ump(raw.data(), raw_len, /*group=*/0, &rs, &ump);
    if (consumed == 0) {
      ++(*skipped);
      continue;
    }
    MidiClipEvent ev;
    if (!consume_event()) return false;
    ev.ppq = ppq;
    ev.ump = ump;
    track->clip.add_event(ev);
    track->has_midi_events = true;
  }

  if (reader->overflow()) return false;
  if (!pending_sysex.empty()) {
    ++(*skipped);
    *track_truncated = true;
  }
  if (!saw_end_of_track && !*track_truncated) {
    mark_truncated();
  }
  if (!saw_end_of_track) {
    track->length_ppq = smf_ticks_to_ppq(tick, ppqn);
  }
  // Resynchronize to the declared chunk end (skips any trailing bytes after an
  // explicit end-of-track meta, or unconsumed padding). A malformed track's
  // trailing VLQ (delta time, meta length, or SysEx length) can run past
  // end_pos before the per-read overrun check catches it, since the VLQ
  // decoder is only bounded by the overall buffer, not the current track's
  // declared end; rewind to end_pos in that case so parsing resumes cleanly
  // at the next track's chunk header instead of desyncing into its bytes. A
  // well-formed file never triggers either branch below at a non-zero delta.
  if (reader->pos() > end_pos) {
    reader->seek(end_pos);
  } else if (reader->pos() < end_pos) {
    if (reader->take(end_pos - reader->pos()) == nullptr) return false;
  }
  return true;
}

// Parses the SMF byte stream into a raw result. Every failure path returns
// early, keeping whatever complete tracks / meta data were already recovered.
// It deliberately derives neither has_recovered_content nor the tempo /
// time-signature defaults: import_smf() does both on its single exit path, so
// no early return here can skip them.
SmfImportResult parse_smf(const uint8_t* data, size_t size,
                          const resource::MidiImportResourceLimits& limits) {
  SmfImportResult result;
  if (data == nullptr || size == 0) {
    result.status = SmfStatus::kInvalidArgument;
    result.diagnostic = "empty input";
    return result;
  }
  if (size > limits.max_file_bytes) {
    result.status = SmfStatus::kInvalidArgument;
    result.diagnostic = "SMF import resource limit exceeded: file bytes";
    return result;
  }

  Reader reader(data, size);
  if (!reader.match_tag(kMThd)) {
    result.status = SmfStatus::kBadHeader;
    result.diagnostic = "missing MThd header chunk";
    return result;
  }
  const uint32_t header_len = reader.u32();
  if (reader.overflow() || header_len < 6) {
    result.status = SmfStatus::kBadHeader;
    result.diagnostic = "truncated or short MThd chunk";
    return result;
  }
  const uint16_t format = reader.u16();
  const uint16_t num_tracks = reader.u16();
  const uint16_t division = reader.u16();
  if (reader.overflow()) {
    result.status = SmfStatus::kTruncated;
    result.diagnostic = "truncated MThd fields";
    return result;
  }
  if (num_tracks > limits.max_tracks) {
    result.status = SmfStatus::kInvalidArgument;
    result.diagnostic = "SMF import resource limit exceeded: track count";
    return result;
  }
  // Skip any extra header bytes beyond the standard 6.
  if (header_len > 6) {
    if (reader.take(header_len - 6) == nullptr) {
      result.status = SmfStatus::kTruncated;
      result.diagnostic = "truncated MThd extension";
      return result;
    }
  }

  if (format > 1) {
    result.status = SmfStatus::kUnsupportedFormat;
    result.diagnostic = "SMF format 2 is not supported";
    return result;
  }
  if (division & 0x8000u) {
    // SMPTE timing (negative frames/second) is not supported.
    result.status = SmfStatus::kUnsupportedFormat;
    result.diagnostic = "SMPTE division is not supported";
    return result;
  }
  if (division == 0) {
    result.status = SmfStatus::kBadHeader;
    result.diagnostic = "zero ticks-per-quarter division";
    return result;
  }

  result.format = format;
  result.ticks_per_quarter = division;

  size_t event_count = 0;
  size_t metadata_bytes = 0;
  size_t sysex_bytes = 0;
  bool any_track_truncated = false;
  uint16_t tracks_read = 0;
  while (tracks_read < num_tracks) {
    if (reader.remaining() == 0) {
      // Fewer track chunks than the header claimed: stop gracefully.
      break;
    }
    const uint8_t* chunk_tag = reader.take(4);
    if (chunk_tag == nullptr) {
      result.status = SmfStatus::kTruncated;
      result.diagnostic = "truncated SMF chunk header";
      return result;
    }
    const uint32_t chunk_len = reader.u32();
    if (reader.overflow()) {
      result.status = SmfStatus::kTruncated;
      result.diagnostic = "truncated SMF chunk length";
      return result;
    }
    const bool is_track = chunk_tag[0] == kMTrk[0] && chunk_tag[1] == kMTrk[1] &&
                          chunk_tag[2] == kMTrk[2] && chunk_tag[3] == kMTrk[3];
    if (!is_track) {
      // SMF chunks have no RIFF-style alignment padding. Advance by exactly
      // the declared length. A few legacy files append one zero byte after an
      // odd-sized unknown chunk; retain compatibility only when that byte is
      // immediately followed by an unambiguous MTrk header.
      const size_t skip_length = static_cast<size_t>(chunk_len);
      size_t compatibility_padding = 0;
      if ((chunk_len & 1u) != 0) {
        const uint8_t* lookahead = reader.peek(skip_length, 5);
        if (lookahead != nullptr && lookahead[0] == 0x00u && lookahead[1] == kMTrk[0] &&
            lookahead[2] == kMTrk[1] && lookahead[3] == kMTrk[2] && lookahead[4] == kMTrk[3]) {
          compatibility_padding = 1;
        }
      }
      if (reader.take(skip_length + compatibility_padding) == nullptr) {
        result.status = SmfStatus::kTruncated;
        result.diagnostic = "truncated non-MTrk chunk";
        return result;
      }
      ++result.skipped_events;
      continue;
    }

    TrackParseState track;
    bool timing_overflow = false;
    bool resource_exceeded = false;
    bool track_truncated = false;
    if (!parse_track(&reader, chunk_len, division, &track, &result.tempo_segments,
                     &result.time_signatures, &result.markers, &result.sysex_store,
                     &result.skipped_events, &timing_overflow, limits, &event_count,
                     &metadata_bytes, &sysex_bytes, &resource_exceeded, &track_truncated)) {
      if (resource_exceeded) {
        result.status = SmfStatus::kInvalidArgument;
        result.diagnostic = "SMF import resource limit exceeded";
        return result;
      }
      result.status = timing_overflow ? SmfStatus::kBadTrack : SmfStatus::kTruncated;
      result.diagnostic = timing_overflow ? "cumulative SMF tick exceeds uint64 range"
                                          : "malformed or truncated track data";
      return result;
    }
    any_track_truncated = any_track_truncated || track_truncated;
    ++tracks_read;

    if (track.has_midi_events) {
      track.clip.sort_stable();
      result.clips.push_back(std::move(track.clip));
      result.clip_names.push_back(std::move(track.name));
      result.clip_lengths_ppq.push_back(track.length_ppq);
    } else if (!track.name.empty() && result.sequence_name.empty()) {
      // Meta-only conductor track (track 0 in a format-1 file): its track-name
      // meta is the sequence / song title. Keep the first one we see.
      result.sequence_name = std::move(track.name);
    }
  }

  if (any_track_truncated) {
    result.status = SmfStatus::kTruncated;
    result.diagnostic = "one or more SMF tracks were truncated";
  } else {
    result.status = SmfStatus::kOk;
  }
  return result;
}

}  // namespace

SmfImportResult import_smf(const uint8_t* data, size_t size,
                           const resource::MidiImportResourceLimits& limits) {
  SmfImportResult result = parse_smf(data, size, limits);

  // Derived on the importer's single exit path, before the defaults below are
  // injected: a truncated track aborts parse_smf() at the failing chunk, and
  // the complete tracks parsed ahead of it are already in the result. Deciding
  // this per return site is what previously dropped them.
  result.has_recovered_content = !result.clips.empty() || !result.tempo_segments.empty() ||
                                 !result.time_signatures.empty() || !result.markers.empty() ||
                                 !result.sequence_name.empty();

  // Provide sane defaults so the consumer can hand the segment vectors straight
  // to TempoMap::set_segments without an empty-vector crash. A recovered
  // truncated import reaches the same consumers as a clean one, so the defaults
  // apply to every result rather than only to the clean parse.
  if (result.tempo_segments.empty()) {
    transport::TempoSegment seg;
    seg.start_ppq = 0.0;
    seg.bpm = kDefaultBpm;
    result.tempo_segments.push_back(seg);
  }
  if (result.time_signatures.empty()) {
    transport::TimeSignatureSegment seg;
    seg.start_ppq = 0.0;
    result.time_signatures.push_back(seg);
  }
  std::stable_sort(result.tempo_segments.begin(), result.tempo_segments.end(),
                   [](const transport::TempoSegment& a, const transport::TempoSegment& b) {
                     return a.start_ppq < b.start_ppq;
                   });
  std::stable_sort(
      result.time_signatures.begin(), result.time_signatures.end(),
      [](const transport::TimeSignatureSegment& a, const transport::TimeSignatureSegment& b) {
        return a.start_ppq < b.start_ppq;
      });
  return result;
}

namespace {

// Appends a complete MTrk chunk (with a 4-byte length prefix) built from
// `body`.
void append_track_chunk(std::vector<uint8_t>* out, const std::vector<uint8_t>& body) {
  put_tag(out, kMTrk);
  put_u32(out, static_cast<uint32_t>(body.size()));
  out->insert(out->end(), body.begin(), body.end());
}

// Encodes a meta event (0xFF type len payload) into `body` with a delta time.
void put_meta(std::vector<uint8_t>* body, uint32_t delta, uint8_t type, const uint8_t* payload,
              size_t len) {
  put_vlq(body, delta);
  put_u8(body, kMetaPrefix);
  put_u8(body, type);
  put_vlq(body, static_cast<uint32_t>(len));
  if (len > 0 && payload != nullptr) {
    body->insert(body->end(), payload, payload + len);
  }
}

// SysEx is stored as a reassembled payload with no record of its on-disk framing
// (a single F0 event, an F0 dump split across continuation packets, or an
// independent F7 escape all import to the same payload). Export therefore emits
// one canonical F0 SysEx event: split dumps are rejoined and F7-escape events are
// normalized to F0. This is intentional — the engine's SysEx dispatch treats the
// payload as an opaque blob and does not distinguish the origin framing, so the
// round-trip contract preserves the payload, not the byte-level event type.
void put_sysex(std::vector<uint8_t>* body, uint32_t delta, const std::vector<uint8_t>& payload) {
  put_vlq(body, delta);
  put_u8(body, kSysExStart);
  const bool has_terminal_f7 = !payload.empty() && payload.back() == 0xF7u;
  const size_t encoded_size = payload.size() + (has_terminal_f7 ? 0u : 1u);
  put_vlq(body, static_cast<uint32_t>(encoded_size));
  body->insert(body->end(), payload.begin(), payload.end());
  if (!has_terminal_f7) {
    put_u8(body, 0xF7u);
  }
}

}  // namespace

SmfExportResult export_smf(const std::vector<MidiClip>& clips,
                           const std::vector<transport::TempoSegment>& tempo_segments,
                           const std::vector<transport::TimeSignatureSegment>& time_signatures,
                           const std::vector<std::string>& clip_names,
                           const SmfExportOptions& options) {
  SmfExportResult result;
  const uint16_t ppqn = options.ticks_per_quarter != 0 ? options.ticks_per_quarter : 480;

  constexpr size_t kMaxDataTracks = static_cast<size_t>(std::numeric_limits<uint16_t>::max()) - 1;
  if (clips.size() > kMaxDataTracks) {
    result.status = SmfStatus::kInvalidArgument;
    result.diagnostic = "SMF format 1 supports at most 65534 data tracks";
    return result;
  }

  // Header: format 1, num_tracks = 1 meta track + one per clip.
  const uint16_t num_tracks = static_cast<uint16_t>(1 + clips.size());
  size_t written_tracks = 0;
  put_tag(&result.bytes, kMThd);
  put_u32(&result.bytes, 6);
  put_u16(&result.bytes, 1);  // Format 1.
  put_u16(&result.bytes, num_tracks);
  put_u16(&result.bytes, ppqn);

  // -------- Track 0: tempo + time-signature meta. --------
  {
    // Merge tempo + time-sig events sorted by tick so delta times are correct.
    struct MetaItem {
      int64_t tick;
      int kind;  // 0 = tempo, 1 = time-sig, 2 = text-class marker.
      double bpm;
      int numerator;
      int denominator;
      uint8_t clocks_per_metronome_click;
      uint8_t thirty_seconds_per_quarter;
      std::string text;
      SmfMarkerKind marker_kind = SmfMarkerKind::kMarker;  // kind == 2 only.
      int8_t key_fifths = 0;                               // key signature only.
      bool key_minor = false;                              // key signature only.
    };
    std::vector<MetaItem> items;
    for (const auto& seg : tempo_segments) {
      items.push_back({smf_ppq_to_ticks(seg.start_ppq, ppqn),
                       0,
                       seg.bpm > 0.0 ? seg.bpm : kDefaultBpm,
                       0,
                       0,
                       24,
                       8,
                       {}});
    }
    for (const auto& seg : time_signatures) {
      items.push_back({smf_ppq_to_ticks(seg.start_ppq, ppqn),
                       1,
                       0.0,
                       seg.time_sig.numerator,
                       seg.time_sig.denominator,
                       seg.clocks_per_metronome_click,
                       seg.thirty_seconds_per_quarter,
                       {}});
    }
    for (const auto& marker : options.markers) {
      items.push_back({smf_ppq_to_ticks(marker.ppq, ppqn), 2, 0.0, 0, 0, 24, 8, marker.text,
                       marker.kind, marker.key_fifths, marker.key_minor});
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const MetaItem& a, const MetaItem& b) { return a.tick < b.tick; });

    std::vector<uint8_t> body;
    int64_t prev_tick = 0;
    for (const auto& item : items) {
      const uint32_t delta = static_cast<uint32_t>(std::max<int64_t>(0, item.tick - prev_tick));
      prev_tick = item.tick;
      if (item.kind == 0) {
        const double us = kMicrosPerMinute / item.bpm;
        // SMF stores tempo as a 24-bit microseconds-per-quarter field; clamp so
        // an extremely slow tempo (bpm < ~3.576) does not wrap to an unrelated
        // fast tempo when masked to 3 bytes.
        const uint32_t us_per_quarter =
            static_cast<uint32_t>(std::min<int64_t>(std::llround(us), 0xFFFFFF));
        const uint8_t payload[3] = {static_cast<uint8_t>((us_per_quarter >> 16) & 0xFFu),
                                    static_cast<uint8_t>((us_per_quarter >> 8) & 0xFFu),
                                    static_cast<uint8_t>(us_per_quarter & 0xFFu)};
        put_meta(&body, delta, kMetaSetTempo, payload, 3);
      } else if (item.kind == 1) {
        // Encode denominator as a power of two. SMF can only store the
        // denominator as an exponent (note value 1/2^dd), so a non-power-of-two
        // request is rounded up lossily; count it so callers can detect the loss.
        uint8_t dd = 0;
        int den = item.denominator > 0 ? item.denominator : 4;
        while ((1 << dd) < den && dd < 7) ++dd;
        if ((1 << dd) != den) {
          ++result.skipped_events;
        }
        // The numerator occupies a single byte, so a request outside 1..255
        // cannot be stored. Clamp it and count the loss the way the denominator
        // does: a bare narrowing rewrites the meter into an unrelated but valid
        // one (260 becomes 4) with nothing to tell the caller it happened, and
        // 256 becomes the zero the reader above rejects.
        const int stored_numerator = std::clamp(item.numerator, 1, 255);
        if (stored_numerator != item.numerator) {
          ++result.skipped_events;
        }
        const uint8_t payload[4] = {static_cast<uint8_t>(stored_numerator), dd,
                                    item.clocks_per_metronome_click,
                                    item.thirty_seconds_per_quarter};
        put_meta(&body, delta, kMetaTimeSignature, payload, 4);
      } else if (item.kind == 2) {
        const auto* text = reinterpret_cast<const uint8_t*>(item.text.data());
        switch (item.marker_kind) {
          case SmfMarkerKind::kKeySignature: {
            const uint8_t payload[2] = {static_cast<uint8_t>(item.key_fifths),
                                        static_cast<uint8_t>(item.key_minor ? 1 : 0)};
            put_meta(&body, delta, kMetaKeySignature, payload, 2);
            break;
          }
          case SmfMarkerKind::kText:
            put_meta(&body, delta, kMetaText, text, item.text.size());
            break;
          case SmfMarkerKind::kLyric:
            put_meta(&body, delta, kMetaLyric, text, item.text.size());
            break;
          case SmfMarkerKind::kCuePoint:
            put_meta(&body, delta, kMetaCuePoint, text, item.text.size());
            break;
          case SmfMarkerKind::kMarker:
          default:
            put_meta(&body, delta, kMetaMarker, text, item.text.size());
            break;
        }
      }
    }
    // End-of-track.
    put_meta(&body, 0, kMetaEndOfTrack, nullptr, 0);
    append_track_chunk(&result.bytes, body);
    ++written_tracks;
  }

  // -------- One MTrk per clip. --------
  for (size_t ci = 0; ci < clips.size(); ++ci) {
    MidiClip clip = clips[ci];
    clip.sort_stable();
    std::vector<uint8_t> body;

    // Optional track name meta at tick 0.
    if (ci < clip_names.size() && !clip_names[ci].empty()) {
      put_meta(&body, 0, kMetaTrackName, reinterpret_cast<const uint8_t*>(clip_names[ci].data()),
               clip_names[ci].size());
    }

    int64_t prev_tick = 0;
    for (const MidiClipEvent& ev : clip.events()) {
      const int64_t tick = smf_ppq_to_ticks(ev.ppq, ppqn);

      if (ev.ump.sysex_handle != 0 || ev.ump.message_type() == UmpMessageType::kData64 ||
          ev.ump.message_type() == UmpMessageType::kData128) {
        const std::vector<uint8_t>* payload = options.sysex_store != nullptr
                                                  ? options.sysex_store->lookup(ev.ump.sysex_handle)
                                                  : nullptr;
        if (payload == nullptr) {
          ++result.skipped_events;
          continue;
        }
        const uint32_t delta = static_cast<uint32_t>(std::max<int64_t>(0, tick - prev_tick));
        prev_tick = tick;
        put_sysex(&body, delta, *payload);
        continue;
      }

      // Down-convert MIDI 2.0 events to MIDI 1.0 before serialization. Banked
      // MIDI 2.0 program changes lower to CC#0, CC#32, then Program Change at
      // the same tick; MIDI 1.0 events pass through unchanged; 2.0-only
      // controller forms yield count 0.
      const Midi1MessageList lowered = midi2_to_midi1_messages(ev.ump);
      if (lowered.count == 0) {
        ++result.skipped_events;
        continue;  // Unresolved SysEx / dropped 2.0-only messages are not emitted.
      }
      uint32_t delta = static_cast<uint32_t>(std::max<int64_t>(0, tick - prev_tick));
      for (uint8_t mi = 0; mi < lowered.count; ++mi) {
        const Ump& ump = lowered.messages[mi];
        if (ump.message_type() != UmpMessageType::kMidi1ChannelVoice || ump.word_count == 0) {
          ++result.skipped_events;
          continue;
        }
        std::array<uint8_t, 3> raw{};
        const size_t n = ump_to_midi1_bytes(ump, raw.data(), raw.size());
        if (n == 0) {
          ++result.skipped_events;
          continue;
        }

        put_vlq(&body, delta);
        // Always write an explicit status byte (no running-status compression)
        // so the output is unambiguous and simple to re-import.
        body.insert(body.end(), raw.begin(), raw.begin() + n);
        delta = 0;
      }
      prev_tick = tick;
    }
    put_meta(&body, 0, kMetaEndOfTrack, nullptr, 0);
    append_track_chunk(&result.bytes, body);
    ++written_tracks;
  }

  if (written_tracks != static_cast<size_t>(num_tracks)) {
    result.status = SmfStatus::kBadTrack;
    result.diagnostic = "SMF header track count does not match emitted MTrk chunks";
    result.bytes.clear();
    return result;
  }

  result.status = SmfStatus::kOk;
  return result;
}

}  // namespace sonare::midi
