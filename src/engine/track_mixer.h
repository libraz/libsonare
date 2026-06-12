#pragma once

/// @file track_mixer.h
/// @brief Realtime per-track lane mixer used by RealtimeEngine.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/clip_player.h"
#include "mixing/api/scene.h"
#include "mixing/channel_strip.h"
#include "mixing/fx_bus.h"
#include "rt/param_smoother.h"
#include "rt/processor_base.h"
#include "rt/rt_publisher.h"

namespace sonare::engine {

class MeterTelemetryTap;

std::unique_ptr<mixing::ChannelStrip> make_channel_strip_from_spec(const mixing::api::Strip& spec);

struct TrackLaneConfig {
  struct Send {
    uint32_t bus_id = 0;
    float level_db = 0.0f;
    bool enabled = true;
  };

  TrackLaneConfig() = default;
  TrackLaneConfig(uint32_t track_id_) : track_id(track_id_) {}

  uint32_t track_id = 0;
  std::vector<Send> sends;
  /// Bus the lane's post-fader output sums into instead of the master mix
  /// (group/folder routing); 0 keeps the lane on the master mix. Must
  /// reference a declared bus. Sends are unaffected by the routing.
  uint32_t output_bus_id = 0;
};

struct TrackBusConfig {
  uint32_t bus_id = 0;
  float gain_db = 0.0f;
};

class TrackMixerRuntime final : public rt::ProcessorBase {
 public:
  static constexpr size_t kMaxTrackLanes = 32;
  static constexpr size_t kMaxBusLanes = 8;
  static constexpr int kMaxLaneChannels = 2;

  enum ParamId : unsigned int {
    kFaderDb = 1,
    kPan = 2,
    kWidth = 3,
  };

  bool set_track_lanes(std::vector<TrackLaneConfig> lanes);
  bool set_buses(std::vector<TrackBusConfig> buses);
  void acquire_lanes() noexcept { lanes_.acquire(); }
  bool active() const noexcept;
  size_t lane_count() const noexcept;

  bool set_lane_parameter(size_t lane_index, unsigned int param_id, float value) noexcept;
  bool set_lane_solo_mute(size_t lane_index, bool solo, bool mute) noexcept;
  /// Routes another lane's most recent post-strip audio into one insert of a
  /// lane strip as its sidechain key (ducking/sidechainRouter inserts).
  /// Source lanes rendered earlier in the block deliver same-block audio;
  /// later ones deliver the previous block (one block of key latency).
  /// source_track_id 0 removes the binding. Control-thread only (not safe
  /// concurrently with process()); bindings are keyed by track id and survive
  /// lane republishes. Returns false when the binding table is full.
  bool set_lane_sidechain(uint32_t track_id, unsigned int insert_index,
                          uint32_t source_track_id) noexcept;
  bool bind_track_strip(uint32_t track_id, mixing::ChannelStrip* strip);
  bool set_track_strip(uint32_t track_id, const mixing::api::Strip& strip);
  bool set_track_insert_bypassed(uint32_t track_id, unsigned int insert_index, bool bypassed,
                                 bool reset_on_bypass = false) noexcept;
  bool set_track_eq_band(uint32_t track_id, size_t band_index,
                         const sonare::mastering::eq::EqBand& band) noexcept;
  bool set_bus_gain_db(uint32_t bus_id, float gain_db) noexcept;
  bool set_bus_gain_db_by_index(size_t bus_index, float gain_db) noexcept;
  bool set_bus_strip(uint32_t bus_id, const mixing::api::Bus& bus);

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void flush_pdc_delays() noexcept;
  int latency_samples() const noexcept override { return latency_samples_q8() >> 8; }
  int latency_samples_q8() const noexcept override { return latency_samples_q8_; }

  bool render_clips(ClipPlayer& player, float* const* channels, int num_channels, int num_samples,
                    int64_t timeline_sample, MeterTelemetryTap* meter_tap = nullptr,
                    int64_t render_frame = 0) noexcept;
  /// Snaps every lane fader/pan/gate and bus gain smoother to its current
  /// target. Lane smoothers only advance while lanes render, so a freshly
  /// configured runtime would otherwise ramp from its reset values over the
  /// first audible milliseconds. Intended for offline rendering between
  /// process() calls; not safe concurrently with the audio thread.
  void settle_smoothers() noexcept;
  bool mix_source(uint32_t track_id, float* const* source, float* const* channels, int num_channels,
                  int num_samples, MeterTelemetryTap* meter_tap = nullptr,
                  int64_t render_frame = 0) noexcept;

 private:
  struct LaneState {
    uint32_t track_id = 0;
    rt::ParamSmoother fader_db{0.0f, 5.0f, 48000.0};
    rt::ParamSmoother pan{0.0f, 5.0f, 48000.0};
    rt::ParamSmoother gate{1.0f, 10.0f, 48000.0};
    bool solo = false;
    bool mute = false;
    mixing::ChannelStrip* strip = nullptr;
  };

  struct OwnedStrip {
    uint32_t track_id = 0;
    std::unique_ptr<mixing::ChannelStrip> strip;
  };

  struct BusState {
    uint32_t bus_id = 0;
    rt::ParamSmoother gain_db{0.0f, 5.0f, 48000.0};
    std::unique_ptr<mixing::FxBus> bus;
  };

  struct SidechainBinding {
    uint32_t track_id = 0;
    unsigned int insert_index = 0;
    uint32_t source_track_id = 0;
  };
  static constexpr size_t kMaxSidechainBindings = 16;

  bool lane_config_valid(const std::vector<TrackLaneConfig>& lanes) const noexcept;
  bool bus_config_valid(const std::vector<TrackBusConfig>& buses) const noexcept;
  mixing::ChannelStrip* owned_strip_for(uint32_t track_id) noexcept;
  mixing::ChannelStrip* ensure_owned_strip_for(uint32_t track_id);
  BusState* bus_state_for(uint32_t bus_id) noexcept;
  const BusState* bus_state_for(uint32_t bus_id) const noexcept;
  float* lane_channel(size_t lane_index, int channel) noexcept;
  float* bus_channel(size_t bus_index, int channel) noexcept;
  void clear_lane(size_t lane_index, int num_channels, int num_samples) noexcept;
  void clear_bus(size_t bus_index, int num_channels, int num_samples) noexcept;
  void add_source_to_mix(float* const* source, float* const* channels, int num_channels,
                         int num_samples) noexcept;
  bool any_lane_solo(const std::vector<TrackLaneConfig>& lanes) const noexcept;
  void prepare_lanes_from_snapshot(const std::vector<TrackLaneConfig>& lanes) noexcept;
  void recompute_lane_pdc(const std::vector<TrackLaneConfig>& lanes) noexcept;
  void configure_lane_sends(const std::vector<TrackLaneConfig>& lanes);
  void process_lane_strip(size_t lane_index, int num_channels, int num_samples,
                          int64_t timeline_sample) noexcept;
  void deliver_lane_sidechains(size_t lane_index, int num_channels, int num_samples) noexcept;
  void snapshot_sidechain_key(size_t lane_index, int num_channels, int num_samples) noexcept;
  int lane_index_for_track(uint32_t track_id) const noexcept;
  float* key_channel(size_t lane_index, int channel) noexcept;
  void mix_lane_sends(size_t lane_index, int num_channels, int num_samples,
                      int64_t timeline_sample) noexcept;
  void process_buses(float* const* channels, int num_channels, int num_samples,
                     MeterTelemetryTap* meter_tap, int64_t render_frame) noexcept;
  void apply_lane_to_mix(size_t lane_index, float* const* channels, int num_channels,
                         int num_samples, bool any_solo, MeterTelemetryTap* meter_tap,
                         int64_t render_frame) noexcept;

  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  std::vector<float> scratch_;
  std::vector<float> bus_scratch_;
  // Post-strip, pre-fader snapshots of sidechain SOURCE lanes (the lane
  // buffers themselves are mutated in place by the fader/gate/pan stage).
  std::vector<float> key_scratch_;
  std::array<float*, kMaxLaneChannels> lane_channel_ptrs_{};
  std::array<uint32_t, kMaxTrackLanes> active_track_ids_{};
  std::array<LaneState, kMaxTrackLanes> lane_states_{};
  std::array<mixing::AlignmentDelay, kMaxTrackLanes> lane_pdc_delays_;
  std::array<BusState, kMaxBusLanes> bus_states_{};
  std::vector<TrackBusConfig> bus_configs_;
  std::array<SidechainBinding, kMaxSidechainBindings> sidechain_bindings_{};
  size_t sidechain_binding_count_ = 0;
  std::vector<OwnedStrip> owned_strips_;
  mutable rt::RtPublisher<std::vector<TrackLaneConfig>> lanes_;
  int latency_samples_q8_ = 0;
};

}  // namespace sonare::engine
