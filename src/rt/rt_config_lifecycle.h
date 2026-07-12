#pragma once

/// @file rt_config_lifecycle.h
/// @brief Shared control-thread/audio-thread configuration lifecycle for
///        realtime processors.

#include <memory>
#include <utility>

#include "rt/rt_publisher.h"

namespace sonare::rt {

/// @brief CRTP mixin that owns the lock-free configuration hand-off every
///        realtime dynamics processor shares.
///
/// A processor keeps three views of its configuration:
///  - @c config_ — the control-thread mirror returned by @ref config().
///  - @c active_ — the audio thread's live working config, read by the
///    per-sample loop and mutated in place by RT-safe automation.
///  - the published snapshot — a lock-free @c RtPublisher slot the audio thread
///    adopts between blocks (see @ref adopt_snapshot_for_block).
///
/// The mixin centralises the seed/publish/adopt bookkeeping; the derived
/// processor supplies two hooks:
///  - @c static void @c Derived::validate_config(const ConfigT&) — throws on an
///    invalid config; called before every publish so a throw leaves both the
///    mirror and the snapshot unchanged.
///  - @c void @c Derived::update_coefficients(const ConfigT&) — re-derives the
///    scalar coefficients on the audio thread when a new snapshot is adopted.
///
/// Derived classes befriend this mixin so the hooks may stay private, e.g.
/// @code
///   class Compressor : public rt::ProcessorBase,
///                      public rt::RtConfigLifecycle<Compressor, CompressorConfig> {
///     using ConfigBase = rt::RtConfigLifecycle<Compressor, CompressorConfig>;
///     friend ConfigBase;
///     ...
///   };
/// @endcode
template <typename Derived, typename ConfigT>
class RtConfigLifecycle {
 public:
  /// @brief Returns the most recently published configuration as observed by
  ///        the configuration thread. NOT realtime-safe and NOT safe to call
  ///        concurrently with @ref set_config.
  const ConfigT& config() const { return config_; }

  /// @brief Publishes a new configuration to the realtime processing chain.
  /// @details Validates (via @c Derived::validate_config) before publishing, so
  ///          on throw the control-thread mirror and the audio-thread snapshot
  ///          are both unchanged. The audio thread adopts the snapshot at the
  ///          start of its next block. May allocate; call from the configuration
  ///          thread only, and never concurrently with another @ref set_config.
  void set_config(const ConfigT& config) {
    Derived::validate_config(config);
    config_ = config;
    publish_current_config();
  }

 protected:
  /// @brief Seeds the mirror/live config and publishes an initial snapshot so a
  ///        downstream audio thread that starts before prepare() sees a defined
  ///        configuration. Validates via @c Derived::validate_config.
  explicit RtConfigLifecycle(ConfigT config)
      : config_(std::move(config)),
        config_publisher_(std::make_unique<rt::RtPublisher<ConfigT>>()) {
    Derived::validate_config(config_);
    active_ = config_;
    config_publisher_->publish(std::make_shared<const ConfigT>(config_));
  }

  /// @brief Publishes the current mirror as a fresh snapshot and immediately
  ///        adopts it on the audio side. Called at the tail of prepare() so the
  ///        audio thread observes exactly the snapshot prepare() already applied
  ///        (adopt_snapshot_for_block then skips the redundant recomputation).
  void republish_after_prepare() {
    auto fresh = std::make_shared<const ConfigT>(config_);
    applied_snapshot_ = fresh.get();
    config_publisher_->publish(std::move(fresh));
    config_publisher_->acquire();
  }

  /// @brief Audio-thread hand-off: adopts any pending snapshot and, if a new one
  ///        was adopted, re-derives coefficients via
  ///        @c Derived::update_coefficients. Returns the live working config the
  ///        block should use (never null once seeded in the constructor).
  const ConfigT* adopt_snapshot_for_block() noexcept {
    config_publisher_->acquire();
    const ConfigT* current = config_publisher_->current();
    if (current && current != applied_snapshot_) {
      // A new set_config snapshot supersedes any in-place automation: copy it
      // into the live working config and re-derive coefficients from it.
      active_ = *current;
      static_cast<Derived*>(this)->update_coefficients(active_);
      applied_snapshot_ = current;
    }
    return &active_;
  }

  /// @brief Publishes @c config_ as a new snapshot (control thread; allocates).
  void publish_current_config() {
    config_publisher_->publish(std::make_shared<const ConfigT>(config_));
  }

  /// @brief Control-thread mirror; returned by @ref config().
  ConfigT config_{};
  /// @brief Audio thread's live working configuration, read by the per-sample
  ///        loop and mutated in place by RT-safe automation (no publish).
  ConfigT active_{};
  /// @brief Lock-free single-producer / single-consumer snapshot publisher.
  std::unique_ptr<rt::RtPublisher<ConfigT>> config_publisher_;
  /// @brief The snapshot pointer the audio thread last applied to derived
  ///        coefficients; a differing current() triggers recomputation.
  const ConfigT* applied_snapshot_ = nullptr;
};

}  // namespace sonare::rt
