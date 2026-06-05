#pragma once

#include <cstdint>

namespace sonare::midi::synth {

/// Tracks MIDI RPN/NRPN selection and data-entry routing for one channel.
struct ChannelParamState {
  enum class Mode : uint8_t { kNone = 0, kRpn, kNrpn };

  Mode mode = Mode::kNone;
  uint8_t rpn_msb = 127;
  uint8_t rpn_lsb = 127;
  uint8_t nrpn_msb = 127;
  uint8_t nrpn_lsb = 127;

  void reset() noexcept {
    mode = Mode::kNone;
    rpn_msb = 127;
    rpn_lsb = 127;
    nrpn_msb = 127;
    nrpn_lsb = 127;
  }

  void select_rpn_msb(uint8_t value) noexcept {
    rpn_msb = value;
    mode = Mode::kRpn;
  }

  void select_rpn_lsb(uint8_t value) noexcept {
    rpn_lsb = value;
    mode = Mode::kRpn;
  }

  void select_nrpn_msb(uint8_t value) noexcept {
    nrpn_msb = value;
    mode = Mode::kNrpn;
  }

  void select_nrpn_lsb(uint8_t value) noexcept {
    nrpn_lsb = value;
    mode = Mode::kNrpn;
  }

  bool selected_rpn(uint8_t msb, uint8_t lsb) const noexcept {
    return mode == Mode::kRpn && rpn_msb == msb && rpn_lsb == lsb;
  }

  bool selected_nrpn() const noexcept { return mode == Mode::kNrpn; }
};

}  // namespace sonare::midi::synth
