#pragma once

/// @file rt_config_lifecycle.h
/// @brief Shared control-thread/audio-thread configuration lifecycle for
///        realtime processors.

#include <memory>
#include <utility>

#include "rt/processor_base.h"
#include "rt/rt_publisher.h"

namespace sonare::rt {

/// @brief CRTP base that owns the lock-free configuration hand-off and the
///        scalar automation path every realtime dynamics processor shares.
///
/// A processor keeps three views of its configuration:
///  - @c config_ — the control-thread mirror returned by @ref config().
///  - @c active_ — the audio thread's live working config, read by the
///    per-sample loop and mutated in place by RT-safe automation.
///  - the published snapshot — a lock-free @c RtPublisher slot the audio thread
///    adopts between blocks (see @ref adopt_snapshot_for_block).
///
/// The base centralises the seed/publish/adopt bookkeeping and is the only
/// @c set_parameter_impl; the derived processor supplies three hooks:
///  - @c static void @c Derived::validate_config(const ConfigT&) — throws on an
///    invalid config; called before every publish so a throw leaves both the
///    mirror and the snapshot unchanged.
///  - @c void @c Derived::update_coefficients(const ConfigT&) — re-derives the
///    scalar coefficients on the audio thread from the live config.
///  - @c bool @c Derived::apply_parameter(ConfigT&, unsigned int, float) —
///    writes one automated value into the live config, or returns false and
///    leaves it untouched. RT-safe: no allocation.
///
/// Ordering: automation adopts any pending snapshot before it is applied, so
/// whichever of set_config and automation came last wins, and @c config_
/// always mirrors the configuration the audio path runs.
///
/// Derived classes befriend this base so the hooks may stay private, e.g.
/// @code
///   class Compressor : public rt::RtConfigLifecycle<Compressor, CompressorConfig> {
///     using ConfigBase = rt::RtConfigLifecycle<Compressor, CompressorConfig>;
///     friend ConfigBase;
///     ...
///   };
/// @endcode
template <typename Derived, typename ConfigT>
class RtConfigLifecycle : public ProcessorBase {
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
      // A snapshot published after the last automation replaces the live
      // working config; earlier ones were already adopted by that automation.
      active_ = *current;
      static_cast<Derived*>(this)->update_coefficients(active_);
      applied_snapshot_ = current;
    }
    return &active_;
  }

  /// @brief RT-safe in-place automation, the single path for every processor.
  /// @details No publish and no allocation: adopts a pending snapshot first,
  ///          applies the value through @c Derived::apply_parameter, re-derives
  ///          the coefficients and mirrors the result into @c config_. Must not
  ///          run concurrently with @ref set_config (single producer).
  bool set_parameter_impl(unsigned int param_id, float value) final {
    adopt_snapshot_for_block();
    auto& derived = *static_cast<Derived*>(this);
    if (!derived.apply_parameter(active_, param_id, value)) return false;
    derived.update_coefficients(active_);
    config_ = active_;
    return true;
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
