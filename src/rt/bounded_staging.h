#pragma once

/// @file bounded_staging.h
/// @brief Fixed-capacity, order-preserving staging with one overflow policy.
///
/// Entries stay packed in acceptance order. A full container never overwrites
/// or reorders an accepted entry: it refuses the new one and counts the
/// refusal, or, where the caller asks for it, evicts the entry latest in time
/// and appends. Allocation free; every method is RT-safe.

#include <array>
#include <cstddef>
#include <utility>

namespace sonare::rt {

template <typename T, size_t Capacity>
class BoundedStaging {
 public:
  static constexpr size_t kCapacity = Capacity;

  /// Appends @p item; a full container refuses it and counts the refusal.
  bool push(const T& item) noexcept {
    if (count_ == Capacity) {
      ++refused_;
      return false;
    }
    items_[count_++] = item;
    return true;
  }

  /// push(), except that an @p item @p same(last, item) accepts replaces the
  /// last entry, so simultaneous entries resolve to the latest one.
  template <typename Same>
  bool push_coalescing(const T& item, Same&& same) noexcept {
    if (count_ > 0 && same(items_[count_ - 1], item)) {
      items_[count_ - 1] = item;
      return true;
    }
    return push(item);
  }

  /// push(), except that a full container evicts the entry latest by
  /// @p time_of that is strictly later than @p item -- the earliest accepted of
  /// equal ones -- into @p evicted, closes the gap and appends. Refuses and
  /// counts when no entry is later.
  template <typename TimeOf>
  bool push_evicting_latest(const T& item, TimeOf&& time_of, T* evicted) noexcept {
    if (count_ < Capacity) return push(item);
    size_t latest = count_;
    auto latest_time = time_of(item);
    for (size_t i = 0; i < count_; ++i) {
      if (time_of(items_[i]) > latest_time) {
        latest_time = time_of(items_[i]);
        latest = i;
      }
    }
    if (latest == count_) {
      ++refused_;
      return false;
    }
    *evicted = std::move(items_[latest]);
    for (size_t i = latest; i + 1 < count_; ++i) items_[i] = std::move(items_[i + 1]);
    items_[count_ - 1] = item;
    return true;
  }

  /// Visits every entry once, in order, and removes those @p pred returns true
  /// for; the rest keep their order.
  template <typename Pred>
  void remove_if(Pred&& pred) noexcept {
    size_t out = 0;
    for (size_t i = 0; i < count_; ++i) {
      if (pred(items_[i])) continue;
      if (out != i) items_[out] = std::move(items_[i]);
      ++out;
    }
    count_ = out;
  }

  /// Empties the staging; the refusal count is kept.
  void clear() noexcept { count_ = 0; }
  size_t size() const noexcept { return count_; }
  bool empty() const noexcept { return count_ == 0; }
  const T& operator[](size_t index) const noexcept { return items_[index]; }
  T& operator[](size_t index) noexcept { return items_[index]; }
  T* begin() noexcept { return items_.data(); }
  T* end() noexcept { return items_.data() + count_; }
  const T* begin() const noexcept { return items_.data(); }
  const T* end() const noexcept { return items_.data() + count_; }
  /// Entries refused since construction.
  size_t refused() const noexcept { return refused_; }

 private:
  std::array<T, Capacity> items_{};
  size_t count_ = 0;
  size_t refused_ = 0;
};

}  // namespace sonare::rt
