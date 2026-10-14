#pragma once

/// @file cross_channel_delay.h
/// @brief Ring delay for a stereo side, cleared when it is reassigned to the other side.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace sonare::mastering::stereo {

/// Buffered samples belong to the side the line was last routed to; routing it to the other
/// side drops them so they never surface in the opposite channel.
class CrossChannelDelay {
 public:
  /// Resizes to at least one slot and clears the history.
  void assign(size_t size) {
    buffer_.assign(std::max(size, size_t{1}), 0.0f);
    index_ = 0;
  }

  void clear() {
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    index_ = 0;
  }

  /// Selects the side the line serves; a change clears the history.
  void route(bool right) {
    if (right == right_) return;
    right_ = right;
    clear();
  }

  /// Returns the oldest sample and stores `input` in its slot.
  float exchange(float input) {
    const float oldest = buffer_[index_];
    buffer_[index_] = input;
    advance();
    return oldest;
  }

  /// Stores `input` at the write position without advancing.
  void write(float input) { buffer_[index_] = input; }

  /// Sample written `offset` positions before the write position.
  float tap(size_t offset) const {
    const size_t size = buffer_.size();
    return buffer_[(index_ + size - offset) % size];
  }

  void advance() { index_ = (index_ + 1) % buffer_.size(); }

  size_t size() const { return buffer_.size(); }

 private:
  std::vector<float> buffer_;
  size_t index_ = 0;
  bool right_ = true;
};

}  // namespace sonare::mastering::stereo
