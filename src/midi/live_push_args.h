#pragma once

/// @file live_push_args.h
/// @brief The argument-range rule shared by every live MIDI push entry point.
///
/// Each check returns nullptr when its arguments lie in the MIDI domain, or a static message
/// naming the first refused argument by its C-ABI parameter name. The C ABI, the Node addon and
/// the WASM module prefix the message with their own entry-point name; the Python binding
/// mirrors the same ranges and wording. Parameters are wide and signed so a caller holding a
/// negative or a byte-wide value reaches the same verdict as one holding the narrowed C type.
/// The UMP shape rule itself is RealtimeEngine::is_pushable_midi_ump; only its message is here.

#include <cstdint>

namespace sonare::midi {

/// @brief Message for a word sequence RealtimeEngine::is_pushable_midi_ump refuses.
inline constexpr const char* kLivePushUmpShapeError = "words do not form a pushable UMP message";

namespace live_push_detail {

inline constexpr int64_t kMaxNibble = 15;
inline constexpr int64_t kMax7 = 127;
inline constexpr int64_t kMax14 = 16383;

constexpr bool in_range(int64_t value, int64_t max_value) noexcept {
  return value >= 0 && value <= max_value;
}

constexpr const char* address_error(int64_t group, int64_t channel) noexcept {
  if (!in_range(group, kMaxNibble)) return "group must be in [0, 15]";
  if (!in_range(channel, kMaxNibble)) return "channel must be in [0, 15]";
  return nullptr;
}

}  // namespace live_push_detail

/// @brief Note-on / note-off: group, channel, note, velocity.
constexpr const char* live_push_note_error(int64_t group, int64_t channel, int64_t note,
                                           int64_t velocity) noexcept {
  using namespace live_push_detail;
  if (const char* error = address_error(group, channel)) return error;
  if (!in_range(note, kMax7)) return "note must be in [0, 127]";
  if (!in_range(velocity, kMax7)) return "velocity must be in [0, 127]";
  return nullptr;
}

/// @brief Control change: group, channel, controller, value.
constexpr const char* live_push_cc_error(int64_t group, int64_t channel, int64_t controller,
                                         int64_t value) noexcept {
  using namespace live_push_detail;
  if (const char* error = address_error(group, channel)) return error;
  if (!in_range(controller, kMax7)) return "controller must be in [0, 127]";
  if (!in_range(value, kMax7)) return "value must be in [0, 127]";
  return nullptr;
}

/// @brief Pitch bend: group, channel, unsigned 14-bit bend14.
constexpr const char* live_push_pitch_bend_error(int64_t group, int64_t channel,
                                                 int64_t bend14) noexcept {
  using namespace live_push_detail;
  if (const char* error = address_error(group, channel)) return error;
  if (!in_range(bend14, kMax14)) return "bend14 must be in [0, 16383]";
  return nullptr;
}

/// @brief Channel pressure: group, channel, pressure.
constexpr const char* live_push_channel_pressure_error(int64_t group, int64_t channel,
                                                       int64_t pressure) noexcept {
  using namespace live_push_detail;
  if (const char* error = address_error(group, channel)) return error;
  if (!in_range(pressure, kMax7)) return "pressure must be in [0, 127]";
  return nullptr;
}

/// @brief Polyphonic key pressure: group, channel, note, pressure.
constexpr const char* live_push_poly_pressure_error(int64_t group, int64_t channel, int64_t note,
                                                    int64_t pressure) noexcept {
  using namespace live_push_detail;
  if (const char* error = address_error(group, channel)) return error;
  if (!in_range(note, kMax7)) return "note must be in [0, 127]";
  if (!in_range(pressure, kMax7)) return "pressure must be in [0, 127]";
  return nullptr;
}

/// @brief MIDI 2.0 program change: a bank is carried only when @p bank_valid is set.
constexpr const char* live_push_program_error(int64_t group, int64_t channel, int64_t program,
                                              bool bank_valid, int64_t bank_msb,
                                              int64_t bank_lsb) noexcept {
  using namespace live_push_detail;
  if (const char* error = address_error(group, channel)) return error;
  if (!in_range(program, kMax7)) return "program must be in [0, 127]";
  if (!in_range(bank_msb, kMax7)) return "bank_msb must be in [0, 127]";
  if (!in_range(bank_lsb, kMax7)) return "bank_lsb must be in [0, 127]";
  if (!bank_valid && (bank_msb != 0 || bank_lsb != 0)) {
    return "bank MSB and LSB must be 0 when bank-valid is false";
  }
  return nullptr;
}

}  // namespace sonare::midi
