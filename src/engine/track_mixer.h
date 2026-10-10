#pragma once

/// @file track_mixer.h
/// @brief Realtime per-track lane mixer used by RealtimeEngine.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "engine/clip_player.h"
#include "engine/insert_automation_id.h"
#include "engine/insert_automation_targets.h"
#include "engine/insert_gain_reduction_board.h"
#include "mastering/eq/parametric.h"
#include "mixing/api/scene.h"
#include "mixing/channel_strip.h"
#include "mixing/fx_bus.h"
#include "mixing/panner.h"
#include "mixing/send.h"
#include "mixing/stereo_width.h"
#include "rt/param_smoother.h"
#include "rt/processor_base.h"
#include "rt/rt_publisher.h"
#include "rt/seqlock_cell.h"
#include "util/constants.h"

namespace sonare::engine {

class MeterTelemetryTap;
class ScopeTelemetryTap;

/// @param build_sample_rate Rate the strip runs at, or 0 when unknown. An equalizer insert built
///        for a known rate accepts band frequencies up to that rate's Nyquist.
std::unique_ptr<mixing::ChannelStrip> make_channel_strip_from_spec(const mixing::api::Strip& spec,
                                                                   double build_sample_rate = 0.0);

/// True when two insert chains have the same effective processors and semantic
/// parameters, so a strip or bus can be updated in place instead of rebuilt.
/// Track/master strips use the runtime's combined [pre...post...] order;
/// buses retain their declared order because their send graph addresses it.
bool strip_inserts_equal(const std::vector<mixing::api::Insert>& a,
                         const std::vector<mixing::api::Insert>& b,
                         bool canonical_track_order = true);

/// Applies every scalar of @p next to an existing strip through its setters
/// (smoothed values ramp rather than snap), and its EQ as a diff against
/// @p previous so unchanged bands keep their filter state.
void apply_strip_scalars(mixing::ChannelStrip& strip, const mixing::api::Strip& next,
                         const mixing::api::Strip& previous);

/// True when every band of @p eq can be installed at @p sample_rate, so a
/// caller can refuse a spec before applying any part of it.
bool strip_eq_acceptable(const mixing::api::StripEq& eq, double sample_rate) noexcept;

/// Records @p band at @p band_index in a retained spec, padding any gap with
/// default bands.
void store_eq_band(mixing::api::StripEq& eq, size_t band_index, const mastering::eq::EqBand& band);

/// The registered processor name at @p insert_index in @p spec's combined
/// insert order (PreFader entries in spec order, then PostFader entries in
/// spec order) -- the same order ChannelStrip::insert_parameter_id_for_key
/// addresses. nullptr when out of range. Shared by TrackMixerRuntime's lane
/// insert resolution and RealtimeEngine's master-strip one, since both read a
/// retained mixing::api::Strip.
const std::string* strip_insert_processor_name_at(const mixing::api::Strip& spec,
                                                  unsigned int insert_index) noexcept;

/// Per-lane cue tap. This state belongs to TrackMixerRuntime rather than the
/// legacy raw-strip MonitorRuntime, so lane reorder/persistence follows the
/// track-id keyed LaneState mapping.
enum class TrackMonitorMode : uint8_t {
  kOff = 0,
  kPfl = 1,
  kAfl = 2,
};

/// Message surfaces attach when a lane names a source layout other than stereo.
inline constexpr const char* kTrackLaneLayoutRefusal =
    "track lane sourceChannelLayout must be stereo; multichannel lanes are not implemented";

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
  /// Input channel layout of the source feeding this lane. Only Stereo is
  /// accepted: set_track_lanes refuses any other layout until multichannel
  /// lanes are implemented.
  ChannelLayout source_layout = ChannelLayout::Stereo;
};

struct TrackBusConfig {
  uint32_t bus_id = 0;
  float gain_db = 0.0f;
  /// Channel layout of this bus. A surround layout (5.1/7.1) makes this a
  /// surround group bus: lanes routed to it are surround-panned and its insert
  /// chain runs at the bus width (StereoPairOnly inserts see only the front
  /// pair). Its output, sends and key follow the width rule: a destination of
  /// equal width takes it plane by plane, a wider one on the same-index planes,
  /// a narrower one (the master included) through mixing::downmix.
  ChannelLayout layout = ChannelLayout::Stereo;
  /// Bus this bus's post-gain output sums into instead of the master mix; 0
  /// keeps it on the master. Must reference another declared bus.
  uint32_t output_bus_id = 0;
  /// Sends to other buses. A pre-fader send taps the bus before gain_db, a
  /// post-fader one after it. Same shape and semantics as a lane send.
  std::vector<TrackLaneConfig::Send> sends{};
};

/// Where a sidechain key is taken from: a track lane's post-strip signal, or a
/// bus's signal after its width stage and before its gain_db.
enum class SidechainSourceKind : uint8_t {
  Track = 0,
  Bus = 1,
};

/// Why a sidechain setter would refuse a binding. Values mirror
/// SonareSidechainRefusal. kPlanRefused covers every way the delay plan can
/// fail: an alignment past the ceiling or an overflow.
enum class SidechainRefusal : uint8_t {
  kNone = 0,
  kInvalidTarget = 1,
  kInsertOutOfRange = 2,
  kUndeclaredSource = 3,
  kInvalidSourceKind = 4,
  kSelfKey = 5,
  kCycle = 6,
  kTableFull = 7,
  kPlanRefused = 8,
};

class TrackMixerRuntime final : public rt::ProcessorBase {
 public:
  static constexpr size_t kMaxTrackLanes = 32;
  static constexpr size_t kMaxBusLanes = 8;
  static constexpr int kMaxLaneChannels = 2;
  /// Per-insert gain reduction published by the audio thread; empty for an unused slot.
  const InsertGainReductionBoard& lane_insert_gain_reduction(size_t lane_index) const noexcept {
    return lane_insert_gr_boards_[lane_index];
  }
  const InsertGainReductionBoard& bus_insert_gain_reduction(size_t bus_index) const noexcept {
    return bus_insert_gr_boards_[bus_index];
  }
  /// Brackets one host block so per-insert readings fold across its sub-blocks (audio thread).
  void begin_insert_gain_reduction_block() noexcept {
    for (InsertGainReductionBoard& board : lane_insert_gr_boards_) board.begin_block();
    for (InsertGainReductionBoard& board : bus_insert_gr_boards_) board.begin_block();
  }
  void end_insert_gain_reduction_block() noexcept {
    for (InsertGainReductionBoard& board : lane_insert_gr_boards_) board.end_block();
    for (InsertGainReductionBoard& board : bus_insert_gr_boards_) board.end_block();
  }
  // Widest master mix or group bus the lane scatter can drive (7.1). Lane source
  // buffers stay stereo (kMaxLaneChannels); the master mix and surround group
  // buses can be wider when a lane is surround-panned into them.
  static constexpr int kMaxBusChannels = 8;
  // Ceiling of every dB gain the mixer accepts (lane fader, bus fader, send
  // level); the floor is constants::kFloorDb. parameterInfo reports it too.
  static constexpr float kMaxGainDb = 24.0f;

  // CONTROL-thread staging for the raw-clip PDC banks. The banks are carried
  // by LaneState in the live runtime, while this value owns a complete
  // replacement set so a PDC recompute can fail without changing audio state.
  // A successful commit only swaps already-prepared objects and therefore
  // cannot allocate or fail on the audio path.
  struct ClipPdcPlan {
    std::array<mixing::AlignmentDelay, kMaxTrackLanes> banks;
    int delay_samples_q8 = 0;

    // A user-provided constructor makes the array elements default-initialize
    // through AlignmentDelay's explicit zero-delay constructor. Aggregate
    // brace initialization would otherwise reject that explicit constructor.
    ClipPdcPlan() noexcept {}
  };

  enum ParamId : unsigned int {
    kFaderDb = 1,
    kPan = 2,
    kWidth = 3,
  };

  bool set_track_lanes(std::vector<TrackLaneConfig> lanes);
  /// Pure: true exactly when set_track_lanes would accept @p lanes on validation grounds.
  bool validate_track_lanes(const std::vector<TrackLaneConfig>& lanes) const noexcept {
    return lane_config_valid(lanes);
  }
  bool set_buses(std::vector<TrackBusConfig> buses);
  /// AUDIO thread: acquires this block's lane config and sidechain binding table.
  void acquire_lanes() noexcept {
    lanes_.acquire();
    sidechains_reader_.try_load_into(&audio_sidechains_);
  }
  bool active() const noexcept;
  size_t lane_count() const noexcept;
  /// AUDIO thread: copies unique non-zero lane track ids into @p out. Call
  /// after begin_source_mix(), which acquires the current lane snapshot.
  size_t copy_lane_track_ids(uint32_t* out, size_t capacity) const noexcept;

  bool set_lane_parameter(size_t lane_index, unsigned int param_id, float value) noexcept;
  /// set_lane_parameter for the lane @p track_id holds in this block's snapshot;
  /// false when no lane holds it.
  bool set_track_parameter(uint32_t track_id, unsigned int param_id, float value) noexcept;
  /// Glides a track lane's fader (kFaderDb) or pan (kPan) back to the value it holds with
  /// no automation: 0 dB, centre. Same thread contract as set_lane_parameter.
  bool restore_track_parameter(uint32_t track_id, unsigned int param_id) noexcept;
  bool set_lane_solo_mute(size_t lane_index, bool solo, bool mute) noexcept;
  /// AUDIO thread: applies a queued lane monitor mode. The mode is deliberately
  /// plain lane state (not a raw ChannelStrip registration) so it survives lane
  /// reorder by track id and is allocation/lock free in the render path.
  bool set_lane_monitor_mode(size_t lane_index, TrackMonitorMode mode) noexcept;
  /// AUDIO thread: true when at least one configured lane has a PFL/AFL tap.
  bool monitor_active() const noexcept;
  /// AUDIO thread: points lane monitor taps at the RealtimeEngine's prepared
  /// monitor-bus planes for the current sub-block. The pointer is borrowed and
  /// never dereferenced when @p channels is nullptr.
  void set_monitor_bus(float* const* channels, int num_channels) noexcept {
    monitor_bus_ = channels;
    monitor_bus_channel_count_ =
        channels == nullptr ? 0 : std::clamp(num_channels, 0, kMaxBusChannels);
  }
  /// Routes another lane's post-strip audio into one insert of a lane strip as
  /// its sidechain key (ducking/sidechainRouter inserts). Lanes render in key
  /// order and the key is delay-compensated to the destination strip input, so
  /// the result depends on neither lane order nor block size.
  /// source_track_id 0 removes the binding. Bindings are keyed by track id and
  /// survive lane republishes. Refuses a self key, a binding that closes a cycle
  /// over the binding table, a full table and a delay past the alignment
  /// ceiling; a refusal changes nothing. CONTROL thread only, not concurrent
  /// with process(): a binding reorders the lanes and re-derives the delays.
  bool set_lane_sidechain(uint32_t track_id, unsigned int insert_index,
                          uint32_t source_track_id) noexcept;
  /// Keys insert @p insert_index of bus @p bus_id (its scene `inserts` order)
  /// from a track lane or another bus. source_id 0 removes the binding. Refuses
  /// an unknown bus / source, an out-of-range insert, a self key, a key that
  /// closes a cycle with the bus outputs and sends, and a full table; a refusal
  /// changes nothing. CONTROL thread only, not concurrent with process(): a bus
  /// source reorders the buses and re-derives the edge delays.
  bool set_bus_sidechain(uint32_t bus_id, unsigned int insert_index, SidechainSourceKind kind,
                         uint32_t source_id) noexcept;
  /// Keys insert @p insert_index of the master strip. Same contract as
  /// set_bus_sidechain; the master is processed after every bus, so any
  /// declared source is acyclic. The index must be below the count published
  /// by commit_master_strip_update().
  bool set_master_sidechain(unsigned int insert_index, SidechainSourceKind kind,
                            uint32_t source_id) noexcept;
  /// The verdict the matching setter reaches, without changing anything. Each
  /// setter commits only when its query answers kNone, so the two cannot
  /// disagree. When several reasons hold, the first check the setter runs wins.
  /// CONTROL thread only, like the setters.
  SidechainRefusal can_set_lane_sidechain(uint32_t track_id, unsigned int insert_index,
                                          uint32_t source_track_id) const noexcept;
  SidechainRefusal can_set_bus_sidechain(uint32_t bus_id, unsigned int insert_index,
                                         SidechainSourceKind kind,
                                         uint32_t source_id) const noexcept;
  SidechainRefusal can_set_master_sidechain(unsigned int insert_index, SidechainSourceKind kind,
                                            uint32_t source_id) const noexcept;
  /// CONTROL thread: stage a master-strip PDC update without changing live
  /// pointers, delay banks, insert counts, or sidechain bindings.
  struct PreparedMasterStripUpdate;
  bool prepare_master_strip_update(const mixing::ChannelStrip* candidate, size_t next_insert_count,
                                   PreparedMasterStripUpdate* out) const noexcept;
  /// CONTROL thread: publish a previously staged update. The prepared delay
  /// objects and fixed-size sidechain table make this operation no-fail.
  void commit_master_strip_update(const mixing::ChannelStrip* durable,
                                  PreparedMasterStripUpdate& update) noexcept;
  /// AUDIO thread: hands this block's master keys (aligned to each target
  /// insert's input) to @p strip's inserts. A key the mixer did not compute
  /// this block is left unassigned; a shorter computed key is zero-padded.
  void deliver_master_sidechains(mixing::ChannelStrip* strip, int num_samples) noexcept;
  bool bind_track_strip(uint32_t track_id, mixing::ChannelStrip* strip);
  /// Unbinds @p track_id's lane and destroys its owned strip, binding record and
  /// lane sidechain bindings. CONTROL thread only, not concurrent with process().
  /// True when nothing was bound.
  bool release_track_strip(uint32_t track_id);
  /// Asked with a rebuilt insert chain before it replaces the current one; false
  /// refuses the change with nothing applied. Not asked when the chain is kept.
  using StripAdmission = bool (*)(void* context, uint32_t track_id,
                                  const mixing::ChannelStrip& strip);
  using BusAdmission = bool (*)(void* context, uint32_t bus_id, const mixing::FxBus& bus);
  bool set_track_strip(uint32_t track_id, const mixing::api::Strip& strip,
                       StripAdmission admit = nullptr, void* admit_context = nullptr);
  bool set_track_insert_bypassed(uint32_t track_id, unsigned int insert_index, bool bypassed,
                                 bool reset_on_bypass = false) noexcept;
  // Toggles bypass for a bus insert. Control-thread only (not safe concurrently
  // with process()). Resolves the bus by id like resolve_bus_insert_param, then
  // applies the bypass like set_track_insert_bypassed. Returns false if the bus
  // or insert is unknown.
  bool set_bus_insert_bypassed(uint32_t bus_id, unsigned int insert_index, bool bypassed,
                               bool reset_on_bypass = false) noexcept;
  // Control-thread-only resolution for a realtime insert-parameter change:
  // maps a track id + JSON-key parameter name to the strip's lane index and
  // integer param_id. It is read-only with respect to the audio snapshot, but
  // the std::string lookup and control-side binding table are not an audio-RT
  // operation; callers must reject it on the process thread and enqueue the
  // resolved ids for apply_lane_insert_parameter(). The track must be present
  // in the currently published lane config. Returns false if the track,
  // insert, or key is unknown.
  bool resolve_track_insert_param(uint32_t track_id, unsigned int insert_index,
                                  const std::string& key, size_t* out_lane_index,
                                  unsigned int* out_param_id) noexcept;
  /// Applies a by-name lane insert edit on the owning thread. The target is
  /// retargeted through the normal per-insert smoother and never enters the
  /// realtime command queue.
  bool apply_track_insert_param_by_name_now(uint32_t track_id, unsigned int insert_index,
                                            const std::string& key, float value) noexcept;
  /// Applies a retained lane insert value exactly and retires its matching
  /// smoother slot. Used while replaying a strip scene after a topology edit.
  bool restore_track_insert_param_by_name(uint32_t track_id, unsigned int insert_index,
                                          const std::string& key, float value) noexcept;
  /// Reads the immutable construction-time baseline attached to a lane insert.
  /// Returns false when the strip or parameter has no captured value.
  bool track_insert_constructed_parameter_value(uint32_t track_id, unsigned int insert_index,
                                                unsigned int param_id,
                                                float* out_value) const noexcept;
  /// Same lookup read from the audio-side lane state, so it is safe on the audio
  /// thread; false when @p track_id has no prepared lane.
  bool lane_insert_constructed_parameter_value(uint32_t track_id, unsigned int insert_index,
                                               unsigned int param_id,
                                               float* out_value) const noexcept;
  // Audio-thread application of a resolved insert-parameter change. Allocation
  // free; must run from the audio callback (engine command drain), never
  // concurrently with process().
  bool apply_lane_insert_parameter(size_t lane_index, unsigned int insert_index,
                                   unsigned int param_id, float value) noexcept;
  // Control-thread-only resolution for a realtime BUS insert-parameter change:
  // maps a bus id + JSON-key parameter name to the bus index and integer
  // param_id. The string lookup and control-side binding table are not
  // audio-realtime safe; process-thread callers must reject the request and
  // enqueue the resolved ids for apply_bus_insert_parameter(). Returns false if
  // the bus, insert, or key is unknown.
  bool resolve_bus_insert_param(uint32_t bus_id, unsigned int insert_index, const std::string& key,
                                size_t* out_bus_index, unsigned int* out_param_id) noexcept;
  bool apply_bus_insert_param_by_name_now(uint32_t bus_id, unsigned int insert_index,
                                          const std::string& key, float value) noexcept;
  bool restore_bus_insert_param_by_name(uint32_t bus_id, unsigned int insert_index,
                                        const std::string& key, float value) noexcept;
  /// Reads the immutable construction-time baseline attached to a bus insert.
  /// Returns false when the bus or parameter has no captured value.
  bool bus_insert_constructed_parameter_value(uint32_t bus_id, unsigned int insert_index,
                                              unsigned int param_id,
                                              float* out_value) const noexcept;
  // Audio-thread application of a resolved BUS insert-parameter change.
  // Allocation free; mirrors apply_lane_insert_parameter for the bus chain.
  bool apply_bus_insert_parameter(size_t bus_index, unsigned int insert_index,
                                  unsigned int param_id, float value) noexcept;
  // CONTROL thread: the registered processor name at (track, insert), read from
  // the retained strip spec rather than a live processor -- for parameterInfo,
  // which needs a name to look up in the insert catalog rather than a param id
  // to apply. False for an unbound lane, an out-of-range insert index, or a
  // strip with no retained spec (externally bound via bind_track_strip, or
  // automation-seeded with no inserts).
  bool track_insert_processor_name(uint32_t track_id, unsigned int insert_index,
                                   std::string* out_name) const noexcept;
  // Mirrors track_insert_processor_name for a bus, addressed by the positional
  // bus index used internally by the render graph.
  bool bus_insert_processor_name(size_t bus_index, unsigned int insert_index,
                                 std::string* out_name) const noexcept;
  bool bus_insert_processor_name_by_id(uint32_t bus_id, unsigned int insert_index,
                                       std::string* out_name) const noexcept;

  /// CONTROL thread: @p track_id's position in the published lane snapshot, for
  /// the positional legacy insert command. False when the track has no lane.
  bool track_lane_position(uint32_t track_id, uint32_t* out_position) const noexcept;
  /// CONTROL thread: insert count and per-slot processor of a track's bound strip
  /// (0 / nullptr when the track has no lane in the published snapshot or no
  /// strip) and of a configured bus (0 / nullptr when it is not configured).
  size_t track_insert_count(uint32_t track_id) const noexcept;
  /// CONTROL thread: insert count of the strip bound to @p track_id, lane or not.
  size_t bound_track_insert_count(uint32_t track_id) const noexcept;
  const rt::ProcessorBase* track_insert_processor(uint32_t track_id,
                                                  unsigned int insert_index) const noexcept;
  size_t bus_insert_count(uint32_t bus_id) const noexcept;
  const rt::ProcessorBase* bus_insert_processor(uint32_t bus_id,
                                                unsigned int insert_index) const noexcept;
  /// CONTROL thread: track ids of the published lane snapshot and configured bus
  /// ids, each in configuration order.
  size_t copy_control_lane_track_ids(uint32_t* out, size_t capacity) const noexcept;
  const std::vector<TrackBusConfig>& bus_configs() const noexcept { return bus_configs_; }
  bool configured_bus(uint32_t bus_id) const noexcept { return configured_bus_index(bus_id) >= 0; }
  /// AUDIO-thread safe: true when @p track_id's prepared lane, or configured bus
  /// @p bus_id, holds a processor of layout @p processor at @p insert_index.
  bool lane_insert_holds(uint32_t track_id, unsigned int insert_index,
                         const InsertProcessorLayout& processor) const noexcept;
  bool bus_insert_holds(uint32_t bus_id, unsigned int insert_index,
                        const InsertProcessorLayout& processor) const noexcept;

  // Sets the smoothed target of a lane / bus insert parameter from a reserved
  // automation lane. The matching per-(strip, insert, param) one-pole smoother is
  // advanced once per sub-block (advance_insert_automations) before the strip /
  // bus chain renders, then pushed to the processor, so a stepped breakpoint lane
  // glides instead of zipping. Audio-thread only. Returns false when no slot is
  // free (overflow telemetry).
  bool route_lane_insert_param_smoothed(size_t lane_index, unsigned int insert_index,
                                        unsigned int param_id, float value) noexcept;
  bool route_bus_insert_param_smoothed(size_t bus_index, unsigned int insert_index,
                                       unsigned int param_id, float value) noexcept;
  bool route_bus_insert_param_smoothed_by_id(uint32_t bus_id, unsigned int insert_index,
                                             unsigned int param_id, float value) noexcept;
  bool route_track_insert_param_smoothed_by_id(uint32_t track_id, unsigned int insert_index,
                                               unsigned int param_id, float value) noexcept;
  // Number of insert-automation target requests dropped because the slot table
  // was full (advisory telemetry; mirrors the other *_overflow counters).
  uint32_t insert_automation_overflow_count() const noexcept {
    return insert_automation_overflow_count_;
  }
  // Releases the insert-automation slots referencing @p lane_index (e.g. when a
  // lane is removed or fully republished) / every slot. Marks them inactive,
  // never deallocates. Audio-thread / control-thread between blocks.
  void clear_insert_automation_for_lane(size_t lane_index) noexcept;
  void clear_insert_automations() noexcept;
  bool set_track_eq_band(uint32_t track_id, size_t band_index,
                         const sonare::mastering::eq::EqBand& band) noexcept;
  // Granular realtime panner/channel-delay updates for a track lane strip. The
  // strip is resolved read-only from the control-thread binding table, and the
  // pan/pan-law/pan-mode/dual-pan setters write strip atomics, so those four are
  // glitch-free and safe to call while process() renders. Channel delay moves the
  // lane relative to the others without changing strip latency or PDC, but it
  // reallocates the strip's delay line, so callers should treat it as a
  // structural change (not concurrent with process()). Each returns false if the
  // track id has no bound lane strip.
  bool set_track_pan(uint32_t track_id, float pan) noexcept;
  bool set_track_pan_law(uint32_t track_id, mixing::PanLaw law) noexcept;
  bool set_track_pan_mode(uint32_t track_id, mixing::PanMode mode) noexcept;
  bool set_track_dual_pan(uint32_t track_id, float left_pan, float right_pan) noexcept;
  // Surround placement used when the lane feeds a >2-channel destination. False
  // for a non-finite field; distance <= 0 stores the default 1.
  bool set_track_surround_pan(uint32_t track_id, const mixing::SurroundPanParams& params) noexcept;
  bool set_track_channel_delay_samples(uint32_t track_id, int delay_samples) noexcept;
  /// AUDIO thread (fader automation): glides @p bus_id's fader to @p gain_db,
  /// clamped to the fader range. False for an unknown bus or a non-finite value.
  bool set_bus_gain_db(uint32_t bus_id, float gain_db) noexcept;
  // Applies a bus spec. Everything is validated before anything is applied. An
  // unchanged insert chain is kept (with its tails and insert automation);
  // otherwise it is rebuilt. Pan and EQ live outside the chain either way, and
  // EQ is applied as a diff against the retained spec. False for an unknown
  // bus, a non-default pan on a bus wider than two channels, or an EQ band the
  // bus cannot host.
  bool set_bus_strip(uint32_t bus_id, const mixing::api::Bus& bus, BusAdmission admit = nullptr,
                     void* admit_context = nullptr);
  // Granular bus output pan, mirroring the track pan setters: atomic writes,
  // safe concurrently with process(), also recorded in the retained spec.
  // False for an unknown bus or one wider than two channels.
  bool set_bus_pan(uint32_t bus_id, float pan) noexcept;
  bool set_bus_pan_law(uint32_t bus_id, mixing::PanLaw law) noexcept;
  bool set_bus_pan_mode(uint32_t bus_id, mixing::PanMode mode) noexcept;
  bool set_bus_dual_pan(uint32_t bus_id, float left_pan, float right_pan) noexcept;
  // One band of a bus's dedicated EQ, also recorded in the retained spec.
  // Control-thread contract as set_track_eq_band (not concurrent with process()).
  bool set_bus_eq_band(uint32_t bus_id, size_t band_index,
                       const sonare::mastering::eq::EqBand& band) noexcept;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void flush_pdc_delays() noexcept;
  /// CONTROL thread: sets the compensation applied to raw clip audio before it
  /// enters a lane strip. Each lane owns an independent bank, so processing
  /// several clips in sequence cannot mix their delay histories. The update is
  /// staged for every lane and committed only after all banks accept it.
  bool set_clip_pdc_delay_q8(int delay_samples_q8) noexcept;
  /// AUDIO thread: advances every configured lane's raw-clip PDC bank with
  /// silence. Call after begin_block() when the transport is stopped so a held
  /// delay line cannot reappear as stale audio when playback resumes.
  void drain_clip_pdc_delays(int num_channels, int num_samples) noexcept;
  /// CONTROL thread: stages a complete raw-clip PDC bank set without touching
  /// the live lanes. Existing bank history is copied when the storage shape is
  /// unchanged. Returns false on allocation failure.
  bool prepare_clip_pdc_delay_q8(int delay_samples_q8, ClipPdcPlan& out) const noexcept;
  /// CONTROL thread: commits a plan produced by prepare_clip_pdc_delay_q8().
  /// This is a no-fail swap of already-prepared banks.
  void commit_clip_pdc_delay(ClipPdcPlan& prepared) noexcept;
  /// AUDIO thread: clears raw-clip PDC state during a transport discontinuity.
  void flush_clip_pdc_delays() noexcept;
  /// @brief Total number of times any PDC alignment bank reallocated storage.
  /// @details The observable form of the contract that a control-thread edit
  ///          which does not change an alignment must not disturb the delay
  ///          history that alignment is carrying: an unchanged value across an
  ///          edit means no bank threw away audio in flight. Reallocating a
  ///          bank zero-fills it, so a spurious bump is a dropout the length of
  ///          the compensation delay.
  uint64_t pdc_storage_generation() const noexcept;
  int latency_samples() const noexcept override { return latency_samples_q8() >> 8; }
  int latency_samples_q8() const noexcept override { return latency_samples_q8_; }
  /// Longest audible tail from any lane to the master, in samples: the lane
  /// strip (channel delay included) followed by the longest of its output route
  /// and its send routes, each bus adding its own chain. Key paths carry no
  /// audio and are excluded. INT_MAX means unbounded. CONTROL thread only.
  int tail_samples() const noexcept override;
  /// Returns every lane strip, bus and alignment/key delay line to its prepared
  /// processing state, keeping configuration, solo/mute, automation and meters.
  /// Not concurrent with process().
  void reset_processing() noexcept;
  /// Ends the signal for every lane strip's and bus's meters (MeterProcessor::flush_true_peak).
  void flush_meters() noexcept;

  bool render_clips(ClipPlayer& player, float* const* channels, int num_channels, int num_samples,
                    int64_t timeline_sample, MeterTelemetryTap* meter_tap = nullptr,
                    int64_t render_frame = 0, ScopeTelemetryTap* scope_tap = nullptr) noexcept;
  /// Snaps every lane fader/pan/gate and bus gain smoother to its current
  /// target. Lane smoothers only advance while lanes render, so a freshly
  /// configured runtime would otherwise ramp from its reset values over the
  /// first audible milliseconds. Intended for offline rendering between
  /// process() calls; not safe concurrently with the audio thread.
  void settle_smoothers() noexcept;
  /// Adopts the published lane snapshot and refreshes every lane's mute/solo
  /// gate target without rendering, so settle_smoothers() has the targets a
  /// block would set while no strip, send or bus processor advances.
  /// Offline pre-roll only; not safe concurrently with the audio thread.
  void prime_lane_controls() noexcept;
  /// Snap only insert automation slots without changing fader/pan ramps.
  void settle_insert_automations() noexcept;
  void settle_insert_automations(const std::vector<TrackLaneConfig>& next_lanes,
                                 bool preserve_surviving_lanes) noexcept;
  /// Mixes one source through its lane and the buses. Call once per block: it
  /// advances the shared direct and master delays.
  bool mix_source(uint32_t track_id, float* const* source, float* const* channels, int num_channels,
                  int num_samples, MeterTelemetryTap* meter_tap = nullptr, int64_t render_frame = 0,
                  ScopeTelemetryTap* scope_tap = nullptr) noexcept;

  // Block-level aggregation: the one begin / accumulate / finish sequence that
  // routes EVERY contribution of a block -- clip audio and hosted-instrument
  // audio alike -- through each bus exactly once.
  //
  // A bus insert chain is stateful (reverb tails, compressor envelopes) and
  // non-linear (compression, saturation), so it must see the SUM of its
  // contributors, once. render_clips() followed by a separate source-mix pass
  // runs it twice per block: once over the clip contribution and once over the
  // instrument contribution. A compressor then acts on two partial signals
  // instead of the summed bus, which is not the same signal, and a reverb
  // advances twice per block.
  //
  //   begin_block()             clears the lane accumulators and every bus, and
  //                             advances the insert-parameter smoothers once
  //   render_clips_into_lanes() accumulates clip audio into the lanes
  //   mix_source_into_lane()    accumulates one instrument source into its lane
  //   finish_block()            runs every configured lane's strip / sends / fader
  //                             and each bus chain exactly once, into the master
  //
  // An opened block processes configured lanes that received audio and silent
  // lanes alike; zero input keeps stateful strip/bus tails advancing.
  bool begin_block(int num_channels, int num_samples) noexcept;
  // Accumulates this block's clip audio into the lanes (and sums lane-less
  // clips into the runtime's direct scratch). Does not run any strip, send, or
  // bus. When @p direct_output is supplied, unmatched clips are written there
  // for an outer source-PDC stage and must later be passed back through
  // mix_source_into_lane() before finish_block().
  // Returns false when the lane config is empty/invalid.
  bool render_clips_into_lanes(ClipPlayer& player, float* const* channels, int num_channels,
                               int num_samples, int64_t timeline_sample,
                               float* const* direct_output = nullptr) noexcept;
  // Runs every active lane's strip / sends / fader and every bus insert chain
  // once, summing the result into @p channels. Call once per block, after the
  // last accumulate.
  void finish_block(float* const* channels, int num_channels, int num_samples,
                    int64_t timeline_sample, MeterTelemetryTap* meter_tap = nullptr,
                    int64_t render_frame = 0, ScopeTelemetryTap* scope_tap = nullptr) noexcept;

  // Source-only staging, kept for callers that mix instrument sources without a
  // clip pass (the offline stem collector). begin_source_mix() is begin_block();
  // finish_source_mix() is finish_block() at timeline 0 with the historical
  // per-lane interleaving, which the offline bounce goldens depend on.
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

  enum class SidechainTargetKind : uint8_t { Lane = 0, Bus = 1, Master = 2 };
  // One table for every keyed insert. target_id is the lane's track id or the
  // bus id (0 for the master); source_id a track id or a bus id per source_kind.
  struct SidechainBinding {
    uint32_t target_id = 0;
    unsigned int insert_index = 0;
    uint32_t source_id = 0;
    uint8_t target_kind = 0;
    uint8_t source_kind = 0;
    uint8_t key_slot = 0;  // key delay line / buffer index; travels with the binding on compaction
  };
  static constexpr size_t kMaxSidechainBindings = 32;
  // Crosses to the audio thread as one seqlock snapshot, so compaction is never seen half-applied.
  struct SidechainTable {
    std::array<SidechainBinding, kMaxSidechainBindings> bindings{};
    size_t count = 0;
  };
  // One bus edge towards another bus: its output (slot 0) or a send (1 + index).
  static constexpr size_t kBusEdgesPerBus = 1 + mixing::ChannelStrip::kMaxSends;

  // Every alignment delay (Q8) the mixer applies, indexed like the banks.
  // These control-thread staging records are public only so the master-strip
  // transaction token can be constructed by RealtimeEngine; audio code never
  // observes them.
  struct PdcPlan {
    std::array<int, kMaxTrackLanes> lane_q8{};
    std::array<int, kMaxTrackLanes> lane_pre_q8{};
    // Pre-strip delay p(L) that puts every lane key on its destination's strip input.
    std::array<int, kMaxTrackLanes> lane_in_q8{};
    // Stable topological lane render order over the lane-key edges.
    std::array<uint8_t, kMaxTrackLanes> lane_order{};
    size_t lane_order_count = 0;
    std::array<int, kMaxBusLanes> bus_in_q8{};
    std::array<int, kMaxBusLanes * kBusEdgesPerBus> edge_q8{};
    std::array<int, kMaxSidechainBindings> key_q8{};
    int direct_q8 = 0;
    int master_q8 = 0;
    int latency_q8 = 0;
  };
  struct PreparedPdc {
    PdcPlan plan{};
    std::array<int, kMaxTrackLanes> lane_source{};
    std::array<bool, kMaxTrackLanes> lane_reset{};
    std::array<mixing::AlignmentDelay::PreparedUpdate, kMaxTrackLanes> lane_updates{};
    std::array<mixing::AlignmentDelay::PreparedUpdate, kMaxTrackLanes> lane_pre_send_updates{};
    std::array<mixing::AlignmentDelay::PreparedUpdate, kMaxTrackLanes> lane_in_updates{};
    std::array<mixing::AlignmentDelay::PreparedUpdate, kMaxBusLanes> bus_updates{};
    std::array<mixing::AlignmentDelay::PreparedUpdate, kMaxBusLanes * kBusEdgesPerBus>
        edge_updates{};
    std::array<mixing::AlignmentDelay::PreparedUpdate, kMaxSidechainBindings> key_updates{};
    mixing::AlignmentDelay::PreparedUpdate direct_update{};
    mixing::AlignmentDelay::PreparedUpdate master_update{};
  };
  struct PreparedMasterStripUpdate {
    const mixing::ChannelStrip* candidate = nullptr;
    size_t next_insert_count = 0;
    SidechainTable next_sidechains{};
    PreparedPdc pdc{};
    bool has_pdc = false;
  };
  // A lane strip binding whose failure points are all behind it.
  struct StagedTrackStrip {
    size_t lane_index = 0;
    PreparedPdc pdc{};
    bool has_pdc = false;
  };

 private:
  struct LaneState {
    uint32_t track_id = 0;
    // Linear gain, even though the public parameter is dB. Converting once at
    // control rate matches ChannelStrip's gain ramp and avoids pow() per sample.
    rt::ParamSmoother fader_gain{1.0f, 5.0f, sonare::constants::kDefaultDawSampleRate};
    rt::ParamSmoother pan{0.0f, 5.0f, sonare::constants::kDefaultDawSampleRate};
    rt::ParamSmoother gate{1.0f, 10.0f, sonare::constants::kDefaultDawSampleRate};
    bool solo = false;
    bool mute = false;
    TrackMonitorMode monitor_mode = TrackMonitorMode::kOff;
    mixing::ChannelStrip* strip = nullptr;
    // Raw clip compensation is before the strip and therefore cannot share the
    // engine's one direct/unmatched clip delay bank. This bank lives with the
    // lane state so prepare_lanes_from_snapshot() carries its history by track
    // id across a positional reorder and resets it for a new track identity.
    mixing::AlignmentDelay clip_pdc_delay{0};
    // Peak level of the source entering the lane strip, captured before its
    // input trim. The render path reuses this fixed array when publishing the
    // lane's post-fader telemetry record, so no audio-thread allocation is
    // needed to report both sides of the dynamics stage.
    std::array<float, mixing::kMaxMeterChannels> input_peak_db =
        mixing::detail::meter_floor_array();
    // Surround placement carried block-to-block so a moving surround pan glides
    // click-free. Unused on the stereo path. Like the stereo lane pan it smooths
    // the parameters with the 5 ms sample-rate time constant and evaluates the
    // law per sample, so the gain at an absolute sample position does not depend
    // on the block partitioning, and a block can end mid-glide.
    mixing::SurroundPanGlide surround_glide;
    // The destination width this lane last rendered at, or -1 before its first
    // block. A surround block whose width differs snaps the placement to its
    // target (no fade-in from silence) so a bounce is deterministic
    // regardless of the pre-roll settle pass and a live first block does not
    // click; consecutive surround blocks at one width ramp from the carried
    // value. The width is the condition rather than a bare "has run" flag
    // because the gains are computed against a layout, so carried across a width
    // change -- including a stereo interlude, which records its own width here
    // and computes no scatter gains at all -- they are a different quantity, and
    // gliding from them places the lane along a path neither layout describes. A
    // lane returning to a width it held earlier therefore snaps rather than
    // glides, which is the same contract its first block gets.
    int surround_primed_channels = -1;
  };

  static_assert(std::is_nothrow_move_constructible_v<LaneState>);
  static_assert(std::is_nothrow_move_assignable_v<LaneState>);
  static_assert(std::is_nothrow_swappable_v<mixing::AlignmentDelay>);

  struct OwnedStrip {
    uint32_t track_id = 0;
    std::unique_ptr<mixing::ChannelStrip> strip;
    // Last spec applied via set_track_strip(). Used to detect when only
    // smoothable scalars changed (identical insert topology) so the strip can be
    // updated in place -- preserving its smoother state -- instead of rebuilt.
    // Default-constructed for externally-bound / automation-seeded strips.
    mixing::api::Strip spec;
  };

  struct BusState {
    uint32_t bus_id = 0;
    rt::ParamSmoother gain{1.0f, 5.0f, sonare::constants::kDefaultDawSampleRate};
    // Bus output trim / width / polarity, mirroring a strip. Trim and polarity
    // run before the insert chain, width after it. At their defaults (0 dB /
    // width 1 / no invert) the per-block processing is skipped entirely, so a
    // bus that never engages them stays bit-identical. The trim smoother holds a
    // linear gain (not dB), smoothing in the linear domain exactly like the
    // strip's GainProcessor, so a bus and a strip ramp identically.
    rt::ParamSmoother input_trim_gain{1.0f, 5.0f, sonare::constants::kDefaultDawSampleRate};
    mixing::StereoWidthProcessor width{1.0f, 5.0f};
    std::atomic<float> polarity_left{1.0f};
    std::atomic<float> polarity_right{1.0f};
    // Output pan (after the inserts) and dedicated EQ (before them), kept outside
    // the FxBus so an insert rebuild does not lose them. Each is skipped at rest,
    // so a bus that never engages them stays bit-identical.
    mixing::PannerProcessor panner{mixing::PannerConfig{}};
    mastering::eq::ParametricEq eq;
    std::atomic<bool> eq_enabled{true};
    // Whether any EQ band is enabled; refreshed on the control thread after each EQ edit.
    std::atomic<bool> eq_active{false};
    std::unique_ptr<mixing::FxBus> bus;
    // Last spec applied via set_bus_strip() and the bus setters. Read for
    // parameterInfo's insert-name resolution, for the in-place and EQ-diff
    // decisions, and for the surround pan check in set_buses.
    mixing::api::Bus spec;
  };

  // Control-thread mirror of one lane strip binding, keyed by track id. Covers
  // every path that sets a lane's strip pointer: bind_track_strip (external or
  // owned strips), set_track_strip (via bind_track_strip), and the send-seeded
  // strips created by configure_lane_sends. The control-thread resolution paths
  // (lane_strip_for_track, resolve_track_insert_param, set_track_insert_bypassed,
  // set_track_eq_band) read this table instead of lane_states_ -- which the audio
  // thread rewrites every block -- and never call acquire_lanes(), whose
  // single-consumer side belongs to the audio thread. Entries survive lane
  // republishes (like sidechain bindings) and are only touched on the control
  // thread.
  struct TrackStripBinding {
    uint32_t track_id = 0;
    mixing::ChannelStrip* strip = nullptr;
  };

  // Resolved routing of one configured bus, rebuilt on the control thread.
  struct BusRoute {
    int output_index = -1;  // -1 = master
    std::array<int, mixing::ChannelStrip::kMaxSends> send_index{};
    size_t send_count = 0;
    bool any_pre_send = false;
    bool key_source = false;
  };
  struct KeyEdge {
    uint32_t source_bus = 0;
    uint32_t target_bus = 0;
  };

  // One automated lane/bus insert parameter. A target id is decoded by the engine
  // router into a (selector, insert, param) triple; the slot's one-pole smoother
  // carries the value so a stepped breakpoint glides. A slot is identified by
  // is_bus + index so a track lane and a bus with the same numeric index stay
  // distinct. Selectors hold integer indices only (no pointers), so a stale slot
  // resolves to a no-op rather than dangling after a strip swap / republish.
  struct InsertAutoSlot {
    bool active = false;
    bool assigned = false;
    bool is_bus = false;
    size_t index = 0;     // lane index (track) or bus index
    uint32_t bus_id = 0;  // stable identity for bus slots; never a positional index
    unsigned int insert_index = 0;
    unsigned int param_id = 0;
    rt::ParamSmoother smoother{};
  };
  static constexpr size_t kMaxInsertAutomations = 64;

  // Finds the slot matching (is_bus, index/bus_id, insert, param) or claims a
  // free one. Bus slots are keyed by identity so reordering cannot retarget a
  // live smoother.
  // Returns nullptr only when the table is full (overflow counter bumped). On the
  // first claim the smoother starts from the processor's last-applied value,
  // then immutable construction metadata, and finally @p value for custom
  // inserts without either kind of metadata.
  InsertAutoSlot* find_or_claim_insert_slot(bool is_bus, size_t index, uint32_t bus_id,
                                            unsigned int insert_index, unsigned int param_id,
                                            float value) noexcept;
  // Advances every active insert-automation smoother by @p num_samples and pushes
  // the result to its target. Call exactly once per sub-block, before the lane /
  // bus chains render, so the smoother cadence matches the lane fader smoother.
  void advance_insert_automations(int num_samples) noexcept;
  void clear_lane_insert_automations() noexcept;
  void clear_bus_insert_automations() noexcept;

  bool lane_config_valid(const std::vector<TrackLaneConfig>& lanes) const noexcept;
  bool bus_config_valid(const std::vector<TrackBusConfig>& buses) const noexcept;
  // Topologically orders @p buses over their output, send and bus-to-bus key
  // edges (Kahn, lowest declared index first, so an edge-free list keeps its
  // declaration order). False on an undeclared or self-referencing target or a
  // cycle.
  static bool validate_bus_graph(const std::vector<TrackBusConfig>& buses, const KeyEdge* key_edges,
                                 size_t key_edge_count,
                                 std::array<size_t, kMaxBusLanes>* order) noexcept;
  // Bus-sourced edges between two buses in the current binding table, skipping
  // entry @p skip.
  size_t collect_key_edges(std::array<KeyEdge, kMaxSidechainBindings>& out,
                           size_t skip) const noexcept;
  // Re-derives bus_order_ and bus_routes_ from bus_configs_ and the binding
  // table. Control thread; the current state is always valid here.
  void refresh_bus_graph() noexcept;
  // Routes of @p buses against the binding table, skipping flagged bindings.
  void build_routes(const std::vector<TrackBusConfig>& buses,
                    const std::array<bool, kMaxSidechainBindings>& skip,
                    std::array<BusRoute, kMaxBusLanes>* routes,
                    const SidechainTable* sidechains = nullptr) const noexcept;
  // A bus graph PDC can be planned against: the live one, or a candidate a
  // setter checks before committing.
  struct BusGraphView {
    const std::vector<TrackBusConfig>* buses = nullptr;
    std::array<size_t, kMaxBusLanes> order{};
    std::array<BusRoute, kMaxBusLanes> routes{};
    // Bus at each index of *buses, which a reorder has not yet moved into bus_states_.
    std::array<const mixing::FxBus*, kMaxBusLanes> bus{};
    std::array<int, kMaxBusLanes> latency_q8{};
    std::array<bool, kMaxSidechainBindings> skip_binding{};
  };
  BusGraphView current_bus_graph_view() const noexcept;
  // Pure: derives every delay for @p view. False when one exceeds
  // mixing::kMaxAlignmentDelaySamples, which the caller refuses.
  bool plan_pdc(const std::vector<TrackLaneConfig>& lanes, const BusGraphView& view, PdcPlan* plan,
                const std::array<mixing::ChannelStrip*, kMaxTrackLanes>* candidate_strips = nullptr,
                const mixing::ChannelStrip* candidate_master_strip = nullptr,
                bool use_candidate_master_strip = false,
                const SidechainTable* candidate_sidechains = nullptr) const noexcept;
  void make_lane_pdc_sources(const std::vector<TrackLaneConfig>& lanes,
                             std::array<int, kMaxTrackLanes>* sources,
                             std::array<bool, kMaxTrackLanes>* reset) const noexcept;
  bool prepare_pdc_updates(
      const PdcPlan& plan, const std::array<int, kMaxTrackLanes>& sources,
      const std::array<bool, kMaxTrackLanes>& reset, PreparedPdc* prepared,
      const std::array<int, kMaxBusLanes>* bus_sources = nullptr) const noexcept;
  void commit_pdc_updates(PreparedPdc& prepared) noexcept;
  // Stages @p plan over the live banks, every lane keeping its own history.
  bool prepare_identity_pdc(const PdcPlan& plan, PreparedPdc* prepared) const noexcept;
  bool apply_pdc(const PdcPlan& plan) noexcept;
  // @p source without the bindings on one target's inserts at or past @p insert_count.
  static SidechainTable without_inserts_from(const SidechainTable& source,
                                             SidechainTargetKind target_kind, uint32_t target_id,
                                             size_t insert_count) noexcept;
  // Removes binding @p index (swap with the last entry). Control thread.
  void remove_sidechain_binding(size_t index) noexcept;
  // Hands the control table to the audio thread as one snapshot. Every edit of
  // sidechains_ ends here.
  void publish_sidechains() noexcept { sidechains_published_.store(sidechains_); }
  void prune_lane_sidechains(uint32_t track_id, size_t insert_count) noexcept;
  // Destroys owned strip @p index and every pointer to it (binding record, lane
  // states). Control thread, not concurrent with process().
  void erase_owned_strip(size_t index) noexcept;
  // True when keying @p track_id from @p source_track_id is a self key or closes
  // a cycle over the lane bindings by track id, skipping entry @p skip.
  bool lane_key_closes_cycle(uint32_t track_id, uint32_t source_track_id,
                             size_t skip) const noexcept;
  // A key slot no binding holds; call only while the table has room.
  size_t free_key_slot() const noexcept;
  // Lane index rendered at @p position of a block over @p lane_count lanes.
  size_t lane_at(size_t position, size_t lane_count) const noexcept {
    return lane_order_count_ == lane_count ? lane_order_[position] : position;
  }
  // Adds or replaces the bus/master binding (kind, target, insert).
  bool store_keyed_binding(SidechainTargetKind target_kind, uint32_t target_id,
                           unsigned int insert_index, SidechainSourceKind kind,
                           uint32_t source_id) noexcept;
  // store_keyed_binding plus the graph and PDC refresh, for a binding the
  // matching can_set_* query accepted; restores the previous binding when a
  // delay bank cannot be allocated.
  bool commit_keyed_binding(SidechainTargetKind target_kind, uint32_t target_id,
                            unsigned int insert_index, SidechainSourceKind kind,
                            uint32_t source_id) noexcept;
  bool sidechain_source_declared(SidechainSourceKind kind, uint32_t source_id) const noexcept;
  // Fills @p planes with the aligned key of binding @p binding_index for a
  // bus/master target; returns its channel count, or 0 when there is none.
  // @p into_slot forces the key into the binding's own buffer even when it
  // needs no delay, so it outlives the block (master keys).
  int build_keyed_input(size_t binding_index, int lane_channels, int num_samples,
                        std::array<const float*, kMaxLaneChannels>& planes,
                        bool into_slot) noexcept;
  int find_sidechain_binding(SidechainTargetKind target_kind, uint32_t target_id,
                             unsigned int insert_index) const noexcept;
  void deliver_bus_sidechains(size_t bus_index, int lane_channels, int num_samples) noexcept;
  // Adds @p source (from_channels wide) into @p dest (to_channels wide) by the
  // width rule: plane by plane when equal, same-index planes when the
  // destination is wider, mixing::downmix when it is narrower.
  void add_with_width_rule(const float* const* source, int from_channels, float* const* dest,
                           int to_channels, int num_samples) noexcept;
  float* bus_edge_channel(int channel) noexcept;
  float* bus_pre_tap_channel(int channel) noexcept;
  float* bus_fold_channel(int channel) noexcept;
  float* bus_key_channel(size_t bus_index, int channel) noexcept;
  float* keyed_input_channel(size_t slot, int channel) noexcept;
  mixing::ChannelStrip* owned_strip_for(uint32_t track_id) noexcept;
  mixing::ChannelStrip* ensure_owned_strip_for(uint32_t track_id);
  // Looks a track's strip up in the control-thread binding table. Returns the
  // recorded pointer (nullptr for an explicit unbind) or nullptr when the track
  // was never bound. Control-thread only; never reads audio-thread state.
  mixing::ChannelStrip* bound_strip_for(uint32_t track_id) const noexcept;
  // Records (or updates) a track's strip in the binding table. May allocate;
  // control-thread only.
  void record_track_strip_binding(uint32_t track_id, mixing::ChannelStrip* strip);
  BusState* bus_state_for(uint32_t bus_id) noexcept;
  const BusState* bus_state_for(uint32_t bus_id) const noexcept;
  // Index of @p bus_id in the current bus config, or -1.
  int configured_bus_index(uint32_t bus_id) const noexcept;
  // The configured bus @p bus_id when its layout is at most two channels wide.
  BusState* pannable_bus_state_for(uint32_t bus_id) noexcept;
  // Moves one bus's contents into another slot (set_buses keys buses by id).
  // Full in-flight DSP scalar and smoother state follows the bus identity;
  // dedicated EQ history follows it too and resets only when set_buses changes
  // that bus's channel layout. Staging copies everything but the owned FxBus and
  // spec and may throw, leaving @p from intact; adopting moves all of it and cannot.
  static void stage_bus_state(const BusState& from, BusState& to);
  static void adopt_bus_state(BusState& from, BusState& to) noexcept;
  // Returns a slot to the state of a bus that was never configured.
  static void retire_bus_state(BusState& state);
  static void apply_bus_pan(BusState& state, const mixing::api::Bus& bus) noexcept;
  static void refresh_bus_eq_active(BusState& state) noexcept;
  void remap_lane_insert_automations(const std::vector<TrackLaneConfig>& lanes) noexcept;
  float* lane_channel(size_t lane_index, int channel) noexcept;
  float* bus_channel(size_t bus_index, int channel) noexcept;
  float* direct_channel(int channel) noexcept;
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
  // Delays the direct/unmatched contribution to the lane-stage timebase and
  // adds it before lane outputs. With a zero delay this preserves the legacy
  // accumulation order and leaves caller-provided channel contents intact.
  void add_direct_to_mix(float* const* channels, int num_channels, int num_samples) noexcept;
  bool any_lane_solo(const std::vector<TrackLaneConfig>& lanes) const noexcept;
  void prepare_lanes_from_snapshot(const std::vector<TrackLaneConfig>& lanes) noexcept;
  void prepare_lanes_from_snapshot(
      const std::vector<TrackLaneConfig>& lanes,
      const std::array<mixing::ChannelStrip*, kMaxTrackLanes>* candidate_strips) noexcept;
  /// Re-derives every PDC alignment bank (lane stage and bus stage) and the
  /// runtime's advertised latency. Returns false, changing nothing, when a
  /// delay would exceed mixing::kMaxAlignmentDelaySamples, and false when a
  /// bank could not grow its storage; `noexcept` so the `noexcept bool`
  /// control-thread setters that call it can report that instead of letting an
  /// allocation failure escape and terminate the process.
  bool recompute_lane_pdc(const std::vector<TrackLaneConfig>& lanes) noexcept;
  // CONTROL thread: stages binding @p strip to @p track_id's lane -- the lane
  // slot, its sends, binding storage and PDC -- without touching live state.
  bool stage_track_strip(uint32_t track_id, mixing::ChannelStrip& strip, StagedTrackStrip* out);
  // CONTROL thread: publishes a staged binding. No-fail.
  void commit_track_strip(uint32_t track_id, mixing::ChannelStrip* strip,
                          StagedTrackStrip& staged) noexcept;
  // The lane slot holding @p track_id, else the first free one; -1 when full.
  int lane_slot_for(uint32_t track_id) const noexcept;
  // A spec's soloed edit lands in the lane's solo, the one state the audible set reads;
  // an unchanged flag leaves a live solo alone.
  void apply_spec_solo(uint32_t track_id, bool soloed) noexcept;
  // Rebuilds lane send tables, every lane or only @p only_track_id's. A lane's
  // table is built in full before it replaces the live one.
  void configure_lane_sends(const std::vector<TrackLaneConfig>& lanes, uint32_t only_track_id = 0);
  // A strip for a lane that has sends but no bound strip, prepared at the mixer's rate.
  std::unique_ptr<mixing::ChannelStrip> make_seed_strip() const;
  // Builds @p config's send table into @p strip. Throws on an unknown bus or a failed allocation.
  void configure_strip_sends(const TrackLaneConfig& config, mixing::ChannelStrip& strip) const;
  void process_lane_strip(size_t lane_index, int num_channels, int num_samples,
                          int64_t timeline_sample) noexcept;
  void add_lane_monitor_pfl(size_t lane_index, int num_channels, int num_samples) noexcept;
  void deliver_lane_sidechains(size_t lane_index, int num_channels, int num_samples) noexcept;
  void snapshot_sidechain_key(size_t lane_index, int num_channels, int num_samples) noexcept;
  int lane_index_for_track(uint32_t track_id) const noexcept;
  // Resolves a track id to its bound lane strip via the control-thread binding
  // table (never lane_states_ or acquire_lanes(), both owned by the audio thread
  // while rendering). Returns nullptr if the track has no bound lane strip.
  mixing::ChannelStrip* lane_strip_for_track(uint32_t track_id) noexcept;
  float* key_channel(size_t lane_index, int channel) noexcept;
  // This block's per-sample lane fader x gate ramp, materialized once by
  // advance_lane_gain() because two stages consume it: the lane's sends and the
  // lane's own pan/sum into the mix. The smoothers can only be advanced once per
  // block, and reading the ramp back is what keeps the sends and the direct path
  // on the same gain.
  float* lane_gain(size_t lane_index) noexcept;
  // This block's per-sample gate ramp alone, written by advance_lane_gain()
  // beside lane_gain(): pre-fader sends follow mute and solo but not the fader.
  float* lane_gate(size_t lane_index) noexcept;
  // The gate snaps to its target within this distance, so a muted lane
  // contributes exact zeros and an unmuted one returns to exactly 1.0f.
  // -120 dB, the level kFloorDb treats as silence.
  static constexpr float kLaneGateSnap = 1e-6f;
  // Scratch the lane's send sources are built in: the post-fader source (the
  // aligned lane buffer scaled by the gain ramp) and the pre-fader source (the
  // strip's pre-fader tap, aligned). One lane's sends are mixed at a time, so a
  // single pair of banks serves every lane.
  float* send_source_channel(int channel) noexcept;
  float* pre_send_source_channel(int channel) noexcept;
  // Advances this lane's fader and gate smoothers once for the block and stores
  // the product, after refreshing the gate target from the lane's own mute and
  // the block's solo state. Must run before mix_lane_sends()/apply_lane_to_mix().
  void advance_lane_gain(size_t lane_index, int num_samples, bool any_solo) noexcept;
  void update_lane_gate_target(size_t lane_index, bool any_solo) noexcept;
  void mix_lane_sends(size_t lane_index, int num_channels, int num_samples,
                      int64_t timeline_sample) noexcept;
  // Processes every configured bus at its own declared width in bus_order_
  // (keys, FxBus insert chain, gain, meter/scope), then feeds its output (the
  // master or another bus) and its sends along aligned edges by the width
  // rule: a wider destination takes the same-index planes, a narrower one a
  // mixing::downmix fold. Finally prepares the master keys.
  void process_buses(float* const* channels, int master_channels, int num_samples,
                     MeterTelemetryTap* meter_tap, int64_t render_frame,
                     ScopeTelemetryTap* scope_tap) noexcept;
  void apply_lane_to_mix(size_t lane_index, float* const* channels, int num_channels,
                         int num_samples, MeterTelemetryTap* meter_tap, int64_t render_frame,
                         ScopeTelemetryTap* scope_tap, int master_channels) noexcept;
  // Scatters a lane's (mono/stereo-summed) post-fader signal across a >2-channel
  // destination (the master mix or a surround group bus) using the strip's
  // surround pan. @p dest holds dest_channels plane pointers; @p lane_channels
  // is the lane's own width (1 or 2). Stereo/mono destinations use the legacy
  // stereo path in apply_lane_to_mix instead.
  void apply_lane_to_mix_surround(size_t lane_index, float* const* dest, int lane_channels,
                                  int dest_channels, int num_samples) noexcept;

  double sample_rate_ = sonare::constants::kDefaultDawSampleRate;
  int max_block_size_ = 0;
  std::vector<float> scratch_;
  std::vector<float> bus_scratch_;
  // Post-strip, pre-lane-PDC snapshots of sidechain SOURCE lanes (the lane
  // buffers themselves are mutated in place by the fader/gate/pan stage).
  std::vector<float> key_scratch_;
  // Frames each lane's key snapshot actually holds. process() is split into
  // sub-blocks of differing lengths, so a snapshot taken at a shorter length
  // leaves an older, longer sub-block's audio in the tail; consumers zero that
  // tail rather than reading it. Audio-thread only.
  std::array<int, kMaxTrackLanes> key_frames_{};
  // One mono plane per lane holding this block's fader x gate ramp.
  std::vector<float> lane_gain_scratch_;
  // One mono plane per lane holding this block's gate ramp alone.
  std::vector<float> lane_gate_scratch_;
  // Two lane-wide banks (post-fader source, then pre-fader source) reused by
  // whichever lane's sends are being mixed.
  std::vector<float> send_source_scratch_;
  // Direct/unmatched clips and sources use a separate scratch so their delay
  // history is independent of every lane and can be added before lane output.
  std::vector<float> direct_scratch_;
  // Sized to the widest buffer it ever addresses (a surround group bus), not the
  // ≤2-wide lane buffers, so it can carry up to kMaxBusChannels plane pointers
  // for bus processing. Lane code fills only the first ≤2 slots.
  std::array<float*, kMaxBusChannels> lane_channel_ptrs_{};
  std::array<uint32_t, kMaxTrackLanes> active_track_ids_{};
  std::array<LaneState, kMaxTrackLanes> lane_states_{};
  std::array<mixing::AlignmentDelay, kMaxTrackLanes> lane_pdc_delays_;
  // Pre-strip delay p(L), one bank per lane: a lane keyed from another lane waits
  // for the source's strip so the key meets its own strip input. Rests at zero
  // without a lane-key edge across a latent strip.
  std::array<mixing::AlignmentDelay, kMaxTrackLanes> lane_in_pdc_delays_;
  // Lane render order committed with the PDC plan; identity without lane keys.
  std::array<uint8_t, kMaxTrackLanes> lane_order_{};
  size_t lane_order_count_ = 0;
  // Pre-fader send alignment, one bank per lane. A pre-fader send taps the strip
  // upstream of its post-insert chain, so it leaves the strip earlier than the
  // lane's output does and lane_pdc_delays_ (which compensates the strip's full
  // latency) is the wrong amount for it. This bank carries the widest strip
  // latency minus the lane's own PRE-fader latency instead, which puts a
  // pre-fader send on the same lane timebase as the post-fader sends, the
  // output-bus routing and the direct master sum. It rests at zero -- and
  // process() short-circuits -- whenever no strip carries latency.
  std::array<mixing::AlignmentDelay, kMaxTrackLanes> lane_pre_send_pdc_delays_;
  // Common lane-stage delay for direct/unmatched master contributions.
  mixing::AlignmentDelay direct_pdc_delay_;
  // Bus-stage PDC. A lane can reach the master both directly and through a bus
  // (its output bus, or any bus it sends to), so one per-lane delay cannot
  // align both paths. lane_pdc_delays_ therefore aligns everything as it leaves
  // the strips -- one common timebase for every bus input and for the direct
  // master sum -- and this second stage aligns what happens after: each bus
  // output carries (widest bus latency - its own), and the master mix
  // accumulated from lanes that skipped the buses entirely carries the widest
  // bus latency. Both rest at zero and short-circuit whenever no bus insert
  // chain reports latency, so a project without a latent bus insert is
  // untouched.
  // Bus input stage: in(b) - L, applied before the bus renders, where L is the
  // lane-stage alignment and in(b) the latest arrival over the bus's incoming
  // edges (outputs, sends and bus-sourced keys). Rests at zero on a flat bus list.
  std::array<mixing::AlignmentDelay, kMaxBusLanes> bus_pdc_delays_;
  // Per-edge alignment, in(destination) - out(source), one bank per bus edge
  // (output and each send). An edge re-times only its own path, so one bus
  // feeding destinations of different depth stays aligned at each.
  std::array<mixing::AlignmentDelay, kMaxBusLanes * kBusEdgesPerBus> bus_edge_delays_;
  // Per-binding key alignment for bus and master targets, indexed by key_slot.
  std::array<mixing::AlignmentDelay, kMaxSidechainBindings> key_edge_delays_;
  mixing::AlignmentDelay master_pdc_delay_;
  // Topological bus processing order and resolved routes (control-written).
  std::array<size_t, kMaxBusLanes> bus_order_{};
  std::array<BusRoute, kMaxBusLanes> bus_routes_{};
  std::array<std::array<std::unique_ptr<mixing::SendProcessor>, mixing::ChannelStrip::kMaxSends>,
             kMaxBusLanes>
      bus_sends_{};
  // Edge scratch, pre-fader tap and downmix fold, each kMaxBusChannels planes.
  std::vector<float> bus_edge_scratch_;
  // Bus key snapshots (2 planes per bus) and per-binding key buffers (2 planes per slot).
  std::vector<float> bus_key_scratch_;
  std::array<int, kMaxBusLanes> bus_key_frames_{};
  std::array<int, kMaxBusLanes> bus_key_channels_{};
  std::vector<float> keyed_input_scratch_;
  std::array<int, kMaxSidechainBindings> master_key_frames_{};
  std::array<int, kMaxSidechainBindings> master_key_channels_{};
  size_t master_insert_count_ = 0;
  const mixing::ChannelStrip* master_strip_ = nullptr;
  std::array<bool, kMaxTrackLanes> source_mix_lane_active_{};
  std::array<BusState, kMaxBusLanes> bus_states_{};
  float* const* monitor_bus_ = nullptr;
  int monitor_bus_channel_count_ = 0;
  std::vector<TrackBusConfig> bus_configs_;
  // Last committed raw-clip PDC target. The engine owns the common direct-clip
  // bank; this target is carried by each lane's staged bank.
  int clip_pdc_delay_q8_ = 0;
  // Control-thread binding table; the audio thread never reads it.
  SidechainTable sidechains_{};
  rt::SeqlockCell<SidechainTable> sidechains_published_{};
  // Audio-thread copy, refreshed in acquire_lanes() so one block reads one
  // table. A torn read keeps the previous block's table.
  rt::SeqlockCell<SidechainTable>::Reader sidechains_reader_ = sidechains_published_.reader();
  SidechainTable audio_sidechains_{};
  // Fixed-capacity insert-automation slot table (lane + bus). Prepared once in
  // prepare(); claimed/advanced on the audio thread with no allocation.
  std::array<InsertAutoSlot, kMaxInsertAutomations> insert_auto_slots_{};
  uint32_t insert_automation_overflow_count_ = 0;
  std::vector<OwnedStrip> owned_strips_;
  std::vector<TrackStripBinding> track_strip_bindings_;
  mutable rt::RtPublisher<std::vector<TrackLaneConfig>> lanes_;
  // The lane snapshot whose arrangement lane_states_ currently reflects. Set by
  // prepare_lanes_from_snapshot; the hot control-thread automation commands
  // (fader/pan/solo/mute) compare lanes_.current() against it and skip the
  // 2 x kMaxTrackLanes LaneState remap when the published config is unchanged.
  // Compared by identity only, never dereferenced (so a retired snapshot pointer
  // is safe); the sole publisher (set_track_lanes) always remaps synchronously,
  // so a still-current snapshot is always the applied one.
  const std::vector<TrackLaneConfig>* applied_lane_snapshot_ = nullptr;
  // Number of leading LaneState slots that represented the previously applied
  // active snapshot. Retired metadata remains in the inactive tail for a
  // remove/re-add, but its raw clip delay bank is reset before reuse.
  size_t applied_lane_count_ = 0;
  std::array<InsertGainReductionBoard, kMaxTrackLanes> lane_insert_gr_boards_{};
  std::array<InsertGainReductionBoard, kMaxBusLanes> bus_insert_gr_boards_{};
  int latency_samples_q8_ = 0;
};

}  // namespace sonare::engine
