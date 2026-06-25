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
class ScopeTelemetryTap;

std::unique_ptr<mixing::ChannelStrip> make_channel_strip_from_spec(const mixing::api::Strip& spec);

struct TrackLaneConfig {
  struct Send {
    uint32_t bus_id = 0;
    float level_db = 0.0f;
    bool enabled = true;
    /// Whether the send taps the lane pre- or post-fader. Defaults to post-fader
    /// to match the historical lane-send behavior and the scene-JSON default.
    mixing::SendTiming timing = mixing::SendTiming::PostFader;
  };

  TrackLaneConfig() = default;
  TrackLaneConfig(uint32_t track_id_) : track_id(track_id_) {}

  uint32_t track_id = 0;
  std::vector<Send> sends;
  /// Bus the lane's post-fader output sums into instead of the master mix
  /// (group/folder routing); 0 keeps the lane on the master mix. Must
  /// reference a declared bus. Sends are unaffected by the routing.
  uint32_t output_bus_id = 0;
  /// Input channel layout of the source feeding this lane. Stored but inert
  /// until the surround DSP path lands (lanes still render stereo in phase 1).
  ChannelLayout source_layout = ChannelLayout::Stereo;
};

struct TrackBusConfig {
  uint32_t bus_id = 0;
  float gain_db = 0.0f;
  /// Channel layout of this bus. A surround layout (5.1/7.1) makes this a
  /// surround group bus: lanes routed to it are surround-panned, its insert
  /// chain runs at the bus width (StereoPairOnly inserts see only the front
  /// pair), and it sums into the master plane-by-plane.
  ChannelLayout layout = ChannelLayout::Stereo;
};

class TrackMixerRuntime final : public rt::ProcessorBase {
 public:
  static constexpr size_t kMaxTrackLanes = 32;
  static constexpr size_t kMaxBusLanes = 8;
  static constexpr int kMaxLaneChannels = 2;
  // Widest master mix or group bus the lane scatter can drive (7.1). Lane source
  // buffers stay stereo (kMaxLaneChannels); the master mix and surround group
  // buses can be wider when a lane is surround-panned into them.
  static constexpr int kMaxBusChannels = 8;

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
  // Control-thread resolution for a realtime insert-parameter change: maps a
  // track id + JSON-key parameter name to the strip's lane index and integer
  // param_id. Reads the lane snapshot and the processor's static descriptor
  // table only (no audio-state mutation). Returns false if the track, insert, or
  // key is unknown. The engine enqueues the resolved ids and applies them on the
  // audio thread via apply_lane_insert_parameter().
  bool resolve_track_insert_param(uint32_t track_id, unsigned int insert_index,
                                  const std::string& key, size_t* out_lane_index,
                                  unsigned int* out_param_id) noexcept;
  // Audio-thread application of a resolved insert-parameter change. Allocation
  // free; must run from the audio callback (engine command drain), never
  // concurrently with process().
  bool apply_lane_insert_parameter(size_t lane_index, unsigned int insert_index,
                                   unsigned int param_id, float value) noexcept;
  bool set_track_eq_band(uint32_t track_id, size_t band_index,
                         const sonare::mastering::eq::EqBand& band) noexcept;
  // Granular realtime panner/channel-delay updates for a track lane strip.
  // Control-thread only (run while process() is not concurrent, the same
  // contract as set_track_insert_bypassed / set_track_eq_band). pan/pan-law/
  // pan-mode/dual-pan write atomics and are glitch-free; channel delay
  // reallocates the alignment delay line (like a PDC recompute) and so adjusts
  // strip latency — callers should treat it as a structural change. Each returns
  // false if the track id has no bound lane strip.
  bool set_track_pan(uint32_t track_id, float pan) noexcept;
  bool set_track_pan_law(uint32_t track_id, mixing::PanLaw law) noexcept;
  bool set_track_pan_mode(uint32_t track_id, mixing::PanMode mode) noexcept;
  bool set_track_dual_pan(uint32_t track_id, float left_pan, float right_pan) noexcept;
  bool set_track_channel_delay_samples(uint32_t track_id, int delay_samples) noexcept;
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
                    int64_t render_frame = 0, ScopeTelemetryTap* scope_tap = nullptr) noexcept;
  /// Snaps every lane fader/pan/gate and bus gain smoother to its current
  /// target. Lane smoothers only advance while lanes render, so a freshly
  /// configured runtime would otherwise ramp from its reset values over the
  /// first audible milliseconds. Intended for offline rendering between
  /// process() calls; not safe concurrently with the audio thread.
  void settle_smoothers() noexcept;
  bool mix_source(uint32_t track_id, float* const* source, float* const* channels, int num_channels,
                  int num_samples, MeterTelemetryTap* meter_tap = nullptr, int64_t render_frame = 0,
                  ScopeTelemetryTap* scope_tap = nullptr) noexcept;

  // Staged variant of mix_source for mixing several sources (e.g. an instrument
  // rack) into shared buses within one block. A bus's insert chain is stateful
  // (reverb tails, compressor envelopes) and must advance exactly once per block:
  // calling mix_source() per source would clear and re-process every bus once per
  // source, advancing reverb N times and summing N bus passes into the master.
  // Instead: begin_source_mix() once (clears all buses), mix_source_into_lane()
  // per source (accumulates each lane's dry signal and sends without touching the
  // bus chain), then finish_source_mix() once (runs the bus chains and sums them
  // into the master). A single source through this trio is bit-identical to
  // mix_source(). Note: a track carrying BOTH clips (rendered via render_clips)
  // and rack instruments still routes its bus twice in a block -- keep a track
  // clip-only or instrument-only when it feeds a shared bus.
  bool begin_source_mix(int num_channels, int num_samples) noexcept;
  // Mixes one source into its matching lane. Reads the lane snapshot prepared by
  // begin_source_mix(); does NOT clear or process buses. Returns false only when
  // the lane config is empty/invalid (caller should sum the source directly);
  // both a matched lane and a direct-summed unmatched source return true. Sets
  // @p routed_through_lane true only when the source matched a lane, so the
  // caller knows finish_source_mix() must run.
  bool mix_source_into_lane(uint32_t track_id, float* const* source, float* const* channels,
                            int num_channels, int num_samples, bool& routed_through_lane,
                            MeterTelemetryTap* meter_tap = nullptr, int64_t render_frame = 0,
                            ScopeTelemetryTap* scope_tap = nullptr) noexcept;
  // Runs every bus insert chain once and sums the buses into the master. Call
  // once after the last mix_source_into_lane() of a block.
  void finish_source_mix(float* const* channels, int num_channels, int num_samples,
                         MeterTelemetryTap* meter_tap = nullptr, int64_t render_frame = 0,
                         ScopeTelemetryTap* scope_tap = nullptr) noexcept;

 private:
  struct LaneState {
    uint32_t track_id = 0;
    rt::ParamSmoother fader_db{0.0f, 5.0f, 48000.0};
    rt::ParamSmoother pan{0.0f, 5.0f, 48000.0};
    rt::ParamSmoother gate{1.0f, 10.0f, 48000.0};
    bool solo = false;
    bool mute = false;
    mixing::ChannelStrip* strip = nullptr;
    // Per-output-plane scatter gains carried block-to-block so a moving surround
    // pan ramps click-free. Unused on the stereo path.
    std::array<float, kMaxBusChannels> surround_gain{};
    // False until the first surround block has run: the first block snaps the
    // scatter gains to their target (no fade-in from silence) so a bounce is
    // deterministic regardless of the pre-roll settle pass, and a live first
    // block does not click. Subsequent blocks ramp from the carried value.
    bool surround_primed = false;
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
  // Render width of a configured bus. A surround group bus (5.1/7.1 layout)
  // renders at its full layout width (6/8). Every non-surround bus renders at
  // min(master_channels, kMaxLaneChannels) — exactly the historical
  // render_channels — so mono/stereo buses stay bit-identical regardless of the
  // master width.
  int bus_render_channels(size_t bus_index, int master_channels) const noexcept;
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
  // Resolves a track id to its bound lane strip, refreshing the lane snapshot
  // first. Returns nullptr if the track has no active lane strip.
  mixing::ChannelStrip* lane_strip_for_track(uint32_t track_id) noexcept;
  float* key_channel(size_t lane_index, int channel) noexcept;
  void mix_lane_sends(size_t lane_index, int num_channels, int num_samples,
                      int64_t timeline_sample) noexcept;
  // Processes every configured bus at its own declared width (FxBus insert
  // chain, gain, meter/scope) and sums it into the master mix, plane-by-plane,
  // up to min(bus_width, master_channels). A surround group bus thus widens the
  // master; a stereo bus into a surround master reaches only the front pair.
  void process_buses(float* const* channels, int master_channels, int num_samples,
                     MeterTelemetryTap* meter_tap, int64_t render_frame,
                     ScopeTelemetryTap* scope_tap) noexcept;
  void apply_lane_to_mix(size_t lane_index, float* const* channels, int num_channels,
                         int num_samples, bool any_solo, MeterTelemetryTap* meter_tap,
                         int64_t render_frame, ScopeTelemetryTap* scope_tap,
                         int master_channels) noexcept;
  // Scatters a lane's (mono/stereo-summed) post-fader signal across a >2-channel
  // destination (the master mix or a surround group bus) using the strip's
  // surround pan. @p dest holds dest_channels plane pointers; @p lane_channels
  // is the lane's own width (1 or 2). Stereo/mono destinations use the legacy
  // stereo path in apply_lane_to_mix instead.
  void apply_lane_to_mix_surround(size_t lane_index, float* const* dest, int lane_channels,
                                  int dest_channels, int num_samples) noexcept;

  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  std::vector<float> scratch_;
  std::vector<float> bus_scratch_;
  // Post-strip, pre-fader snapshots of sidechain SOURCE lanes (the lane
  // buffers themselves are mutated in place by the fader/gate/pan stage).
  std::vector<float> key_scratch_;
  // Sized to the widest buffer it ever addresses (a surround group bus), not the
  // ≤2-wide lane buffers, so it can carry up to kMaxBusChannels plane pointers
  // for bus processing. Lane code fills only the first ≤2 slots.
  std::array<float*, kMaxBusChannels> lane_channel_ptrs_{};
  std::array<uint32_t, kMaxTrackLanes> active_track_ids_{};
  std::array<LaneState, kMaxTrackLanes> lane_states_{};
  std::array<mixing::AlignmentDelay, kMaxTrackLanes> lane_pdc_delays_;
  std::array<BusState, kMaxBusLanes> bus_states_{};
  std::vector<TrackBusConfig> bus_configs_;
  std::array<SidechainBinding, kMaxSidechainBindings> sidechain_bindings_{};
  size_t sidechain_binding_count_ = 0;
  std::vector<OwnedStrip> owned_strips_;
  mutable rt::RtPublisher<std::vector<TrackLaneConfig>> lanes_;
  // The lane snapshot whose arrangement lane_states_ currently reflects. Set by
  // prepare_lanes_from_snapshot; the hot control-thread automation commands
  // (fader/pan/solo/mute) compare lanes_.current() against it and skip the
  // 2 x kMaxTrackLanes LaneState remap when the published config is unchanged.
  // Compared by identity only, never dereferenced (so a retired snapshot pointer
  // is safe); the sole publisher (set_track_lanes) always remaps synchronously,
  // so a still-current snapshot is always the applied one.
  const std::vector<TrackLaneConfig>* applied_lane_snapshot_ = nullptr;
  int latency_samples_q8_ = 0;
};

}  // namespace sonare::engine
