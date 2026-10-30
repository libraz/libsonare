#include "engine/track_mixer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include "engine/track_mixer_internal.h"
#include "util/constants.h"
#include "util/db.h"

namespace sonare::engine {

using constants::kDefaultDawSampleRate;

using sonare::constants::kFloorDb;

namespace {

bool lane_sends_equal(const TrackLaneConfig& left, const TrackLaneConfig& right) noexcept {
  if (left.sends.size() != right.sends.size()) return false;
  for (size_t send_index = 0; send_index < left.sends.size(); ++send_index) {
    const TrackLaneConfig::Send& lhs = left.sends[send_index];
    const TrackLaneConfig::Send& rhs = right.sends[send_index];
    if (lhs.bus_id != rhs.bus_id || lhs.level_db != rhs.level_db || lhs.enabled != rhs.enabled ||
        lhs.timing != rhs.timing) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool TrackMixerRuntime::set_track_lanes(std::vector<TrackLaneConfig> lanes) {
  last_refusal_ = MixerRefusal::kArgument;
  if (!lane_config_valid(lanes)) return false;
  constexpr int kMaxLaneClipDelayQ8 = mixing::kMaxAlignmentDelaySamples << 8;
  // Refuse a non-empty lane snapshot when the retained direct delay exceeds the lane bank ceiling.
  if (!lanes.empty() && clip_pdc_delay_q8_ > kMaxLaneClipDelayQ8) return refuse_from_state();
  std::shared_ptr<const std::vector<TrackLaneConfig>> snapshot;
  try {
    snapshot = std::make_shared<const std::vector<TrackLaneConfig>>(std::move(lanes));
    // All control-table growth happens before any semantic mutation.  The
    // commit below only appends into these reserved tables.
    owned_strips_.reserve(owned_strips_.size() + snapshot->size());
    track_strip_bindings_.reserve(track_strip_bindings_.size() + snapshot->size());
  } catch (const std::bad_alloc&) {
    return refuse_out_of_memory();
  } catch (...) {
    return false;
  }

  std::array<mixing::ChannelStrip*, kMaxTrackLanes> candidate_strips{};
  std::array<std::unique_ptr<mixing::ChannelStrip>, kMaxTrackLanes> staged_owned;
  std::array<mixing::ChannelStrip::PreparedSends, kMaxTrackLanes> staged_sends;
  std::array<bool, kMaxTrackLanes> replace_sends{};
  const std::vector<TrackLaneConfig>* previous_lanes = lanes_.control_current().get();
  size_t staged_owned_count = 0;
  // Owned strips of tracks leaving the snapshot are destroyed at commit, so only the rest count.
  size_t retained_owned_count = 0;
  for (const OwnedStrip& owned : owned_strips_) {
    for (const TrackLaneConfig& config : *snapshot) {
      if (config.track_id == owned.track_id) {
        ++retained_owned_count;
        break;
      }
    }
  }
  try {
    for (size_t lane_index = 0; lane_index < snapshot->size(); ++lane_index) {
      const TrackLaneConfig& config = (*snapshot)[lane_index];
      candidate_strips[lane_index] = bound_strip_for(config.track_id);
      if (candidate_strips[lane_index] == nullptr) {
        candidate_strips[lane_index] = owned_strip_for(config.track_id);
      }
      if (candidate_strips[lane_index] == nullptr && !config.sends.empty()) {
        if (retained_owned_count + staged_owned_count >= kMaxTrackLanes) {
          return refuse_from_state();
        }
        auto strip = make_seed_strip();
        candidate_strips[lane_index] = strip.get();
        staged_owned[lane_index] = std::move(strip);
        ++staged_owned_count;
      }
      if (candidate_strips[lane_index] == nullptr) continue;

      // Keep the retained send processor and its in-flight ramp when identity and config match.
      bool retain_sends = false;
      if (previous_lanes != nullptr && staged_owned[lane_index] == nullptr) {
        for (size_t previous_index = 0; previous_index < previous_lanes->size(); ++previous_index) {
          if ((*previous_lanes)[previous_index].track_id != (*snapshot)[lane_index].track_id) {
            continue;
          }
          if (previous_index < lane_states_.size() &&
              lane_states_[previous_index].track_id == (*snapshot)[lane_index].track_id &&
              lane_states_[previous_index].strip == candidate_strips[lane_index] &&
              candidate_strips[lane_index]->num_sends() == (*snapshot)[lane_index].sends.size() &&
              lane_sends_equal((*previous_lanes)[previous_index], (*snapshot)[lane_index])) {
            retain_sends = true;
          }
          break;
        }
      }
      if (retain_sends) continue;
      replace_sends[lane_index] = true;
      std::array<mixing::SendConfig, mixing::ChannelStrip::kMaxSends> sends{};
      for (size_t send_index = 0; send_index < config.sends.size(); ++send_index) {
        const TrackLaneConfig::Send& send = config.sends[send_index];
        sends[send_index] =
            mixing::SendConfig{send.enabled ? send.level_db : kFloorDb, send.timing, 5.0f};
      }
      if (!candidate_strips[lane_index]->prepare_sends(sends.data(), config.sends.size(),
                                                       staged_sends[lane_index])) {
        // The sends were validated with the list, so only an allocation fails here.
        return refuse_out_of_memory();
      }
    }
  } catch (const std::bad_alloc&) {
    return refuse_out_of_memory();
  } catch (...) {
    return false;
  }

  PdcPlan plan;
  const BusGraphView view = current_bus_graph_view();
  if (!plan_pdc(*snapshot, view, &plan, &candidate_strips)) return refuse_from_state();
  std::array<int, kMaxTrackLanes> lane_sources{};
  std::array<bool, kMaxTrackLanes> lane_reset{};
  make_lane_pdc_sources(*snapshot, &lane_sources, &lane_reset);
  PreparedPdc prepared_pdc;
  if (!prepare_pdc_updates(plan, lane_sources, lane_reset, &prepared_pdc)) {
    return refuse_pdc(prepared_pdc);
  }

  // From here on every vector has capacity and every processor is prepared, so nothing can fail.
  // Insert ramps stay keyed by track identity; only a track leaving the snapshot is retired.
  settle_insert_automations(*snapshot, true);
  remap_lane_insert_automations(*snapshot);
  for (size_t lane_index = 0; lane_index < snapshot->size(); ++lane_index) {
    if (staged_owned[lane_index] == nullptr) continue;
    mixing::ChannelStrip* raw = staged_owned[lane_index].get();
    owned_strips_.push_back(
        OwnedStrip{(*snapshot)[lane_index].track_id, std::move(staged_owned[lane_index]), {}});
    record_track_strip_binding((*snapshot)[lane_index].track_id, raw);
  }
  for (size_t lane_index = 0; lane_index < snapshot->size(); ++lane_index) {
    if (candidate_strips[lane_index] != nullptr && replace_sends[lane_index]) {
      candidate_strips[lane_index]->commit_sends(staged_sends[lane_index]);
    }
  }
  commit_pdc_updates(prepared_pdc);
  // snapshot is non-null and publish() has no other rejection path.  Treat
  // this as the publication point so a hypothetical future failure cannot
  // report false after the no-fail commit has already begun.
  (void)lanes_.publish(snapshot);
  lanes_.acquire_control_quiescent();
  sidechains_reader_.try_load_into(&audio_sidechains_);
  prepare_lanes_from_snapshot(*snapshot, &candidate_strips);
  for (size_t index = owned_strips_.size(); index > 0; --index) {
    const uint32_t track_id = owned_strips_[index - 1].track_id;
    const bool present =
        std::any_of(snapshot->begin(), snapshot->end(),
                    [track_id](const TrackLaneConfig& lane) { return lane.track_id == track_id; });
    if (present) continue;
    erase_owned_strip(index - 1);
    prune_lane_sidechains(track_id, 0);
  }
  return true;
}

bool TrackMixerRuntime::active() const noexcept {
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  return lanes && !lanes->empty();
}

size_t TrackMixerRuntime::lane_count() const noexcept {
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  return lanes ? lanes->size() : 0;
}

size_t TrackMixerRuntime::copy_lane_track_ids(uint32_t* out, size_t capacity) const noexcept {
  if (out == nullptr || capacity == 0) return 0;
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (lanes == nullptr) return 0;
  size_t count = 0;
  for (const TrackLaneConfig& lane : *lanes) {
    if (lane.track_id == 0) continue;
    bool seen = false;
    for (size_t i = 0; i < count; ++i) seen = seen || out[i] == lane.track_id;
    if (seen || count >= capacity) continue;
    out[count++] = lane.track_id;
  }
  return count;
}

bool TrackMixerRuntime::bind_track_strip(uint32_t track_id, mixing::ChannelStrip* strip) {
  if (track_id == 0) return false;
  if (strip != nullptr) {
    if (max_block_size_ > 0) strip->prepare(sample_rate_, max_block_size_);
    StagedTrackStrip staged;
    if (!stage_track_strip(track_id, *strip, &staged)) return false;
    commit_track_strip(track_id, strip, staged);
    return true;
  }
  acquire_lanes();
  // Control-side snapshot throughout: binding a strip is a control-thread
  // structural change, and current() is the audio thread's view.
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
    prepare_lanes_from_snapshot(*lanes);
  }
  const int lane_index = lane_slot_for(track_id);
  if (lane_index < 0) return false;
  clear_insert_automation_for_lane(static_cast<size_t>(lane_index));
  lane_states_[static_cast<size_t>(lane_index)].track_id = track_id;
  lane_states_[static_cast<size_t>(lane_index)].strip = nullptr;
  record_track_strip_binding(track_id, nullptr);
  // An unbound lane with sends seeds its own strip here.
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
    try {
      configure_lane_sends(*lanes, track_id);
    } catch (...) {
      return false;
    }
    if (!recompute_lane_pdc(*lanes)) return false;
  }
  return true;
}

void TrackMixerRuntime::apply_spec_solo(uint32_t track_id, bool soloed) noexcept {
  for (LaneState& lane : lane_states_) {
    if (lane.track_id == track_id) lane.solo = soloed;
  }
}

int TrackMixerRuntime::lane_slot_for(uint32_t track_id) const noexcept {
  for (size_t i = 0; i < lane_states_.size(); ++i) {
    if (lane_states_[i].track_id == track_id) return static_cast<int>(i);
  }
  for (size_t i = 0; i < lane_states_.size(); ++i) {
    if (lane_states_[i].track_id == 0) return static_cast<int>(i);
  }
  return -1;
}

bool TrackMixerRuntime::stage_track_strip(uint32_t track_id, mixing::ChannelStrip& strip,
                                          StagedTrackStrip* out) {
  acquire_lanes();
  const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get();
  if (lanes != nullptr) prepare_lanes_from_snapshot(*lanes);
  const int lane_index = lane_slot_for(track_id);
  if (lane_index < 0) return false;
  out->lane_index = static_cast<size_t>(lane_index);
  try {
    track_strip_bindings_.reserve(track_strip_bindings_.size() + 1);
    if (lanes != nullptr) {
      for (const TrackLaneConfig& config : *lanes) {
        if (config.track_id == track_id) configure_strip_sends(config, strip);
      }
    }
  } catch (const std::bad_alloc&) {
    return refuse_out_of_memory();
  } catch (...) {
    return false;
  }
  out->has_pdc = false;
  if (lanes != nullptr) {
    std::array<mixing::ChannelStrip*, kMaxTrackLanes> candidate{};
    for (size_t i = 0; i < kMaxTrackLanes; ++i) candidate[i] = lane_states_[i].strip;
    candidate[out->lane_index] = &strip;
    PdcPlan plan;
    if (!plan_pdc(*lanes, current_bus_graph_view(), &plan, &candidate)) return false;
    if (!prepare_identity_pdc(plan, &out->pdc)) {
      return out->pdc.out_of_memory ? refuse_out_of_memory() : false;
    }
    out->has_pdc = true;
  }
  return true;
}

void TrackMixerRuntime::commit_track_strip(uint32_t track_id, mixing::ChannelStrip* strip,
                                           StagedTrackStrip& staged) noexcept {
  clear_insert_automation_for_lane(staged.lane_index);
  LaneState& lane = lane_states_[staged.lane_index];
  lane.track_id = track_id;
  lane.strip = strip;
  // Storage was reserved while staging.
  record_track_strip_binding(track_id, strip);
  if (staged.has_pdc) commit_pdc_updates(staged.pdc);
}

bool TrackMixerRuntime::release_track_strip(uint32_t track_id) {
  if (track_id == 0) return false;
  acquire_lanes();
  const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get();
  if (lanes != nullptr) prepare_lanes_from_snapshot(*lanes);
  // Stage first: an unbound lane with sends gets a fresh strip, and the PDC is
  // planned without the released strip or its keys, before anything is destroyed.
  const SidechainTable next_sidechains =
      without_inserts_from(sidechains_, SidechainTargetKind::Lane, track_id, 0);
  std::unique_ptr<mixing::ChannelStrip> seed;
  try {
    if (lanes != nullptr) {
      for (const TrackLaneConfig& config : *lanes) {
        if (config.track_id != track_id || config.sends.empty()) continue;
        seed = make_seed_strip();
        configure_strip_sends(config, *seed);
        owned_strips_.reserve(owned_strips_.size() + 1);
        track_strip_bindings_.reserve(track_strip_bindings_.size() + 1);
      }
    }
  } catch (...) {
    return false;
  }
  PreparedPdc prepared;
  bool has_pdc = false;
  if (lanes != nullptr) {
    std::array<mixing::ChannelStrip*, kMaxTrackLanes> candidate{};
    for (size_t i = 0; i < kMaxTrackLanes; ++i) {
      candidate[i] = lane_states_[i].track_id == track_id ? seed.get() : lane_states_[i].strip;
    }
    BusGraphView view = current_bus_graph_view();
    build_routes(*view.buses, view.skip_binding, &view.routes, &next_sidechains);
    PdcPlan plan;
    if (!plan_pdc(*lanes, view, &plan, &candidate, nullptr, false, &next_sidechains) ||
        !prepare_identity_pdc(plan, &prepared)) {
      return false;
    }
    has_pdc = true;
  }

  // Commit; nothing below can fail.
  for (LaneState& lane : lane_states_) {
    if (lane.track_id != track_id) continue;
    clear_insert_automation_for_lane(static_cast<size_t>(&lane - lane_states_.data()));
    lane.strip = nullptr;
  }
  for (size_t index = 0; index < owned_strips_.size(); ++index) {
    if (owned_strips_[index].track_id != track_id) continue;
    erase_owned_strip(index);
    break;
  }
  track_strip_bindings_.erase(
      std::remove_if(track_strip_bindings_.begin(), track_strip_bindings_.end(),
                     [track_id](const TrackStripBinding& b) { return b.track_id == track_id; }),
      track_strip_bindings_.end());
  sidechains_ = next_sidechains;
  publish_sidechains();
  if (seed != nullptr) {
    mixing::ChannelStrip* raw = seed.get();
    owned_strips_.push_back(OwnedStrip{track_id, std::move(seed), {}});
    for (LaneState& lane : lane_states_) {
      if (lane.track_id == track_id) lane.strip = raw;
    }
    record_track_strip_binding(track_id, raw);
  }
  if (has_pdc) commit_pdc_updates(prepared);
  return true;
}

void TrackMixerRuntime::erase_owned_strip(size_t index) noexcept {
  const mixing::ChannelStrip* raw = owned_strips_[index].strip.get();
  for (LaneState& lane : lane_states_) {
    if (lane.strip == raw) lane.strip = nullptr;
  }
  track_strip_bindings_.erase(
      std::remove_if(track_strip_bindings_.begin(), track_strip_bindings_.end(),
                     [raw](const TrackStripBinding& b) { return b.strip == raw; }),
      track_strip_bindings_.end());
  owned_strips_.erase(owned_strips_.begin() + static_cast<std::ptrdiff_t>(index));
}

bool TrackMixerRuntime::set_track_strip(uint32_t track_id, const mixing::api::Strip& spec,
                                        StripAdmission admit, void* admit_context) {
  last_refusal_ = MixerRefusal::kArgument;
  if (track_id == 0 || !strip_eq_acceptable(spec.eq, sample_rate_)) return false;

  // In-place fast path: when a strip already exists for this track and only its
  // smoothable scalars changed (identical insert topology), retarget the existing
  // strip's parameters instead of rebuilding it. A rebuild constructs a fresh
  // strip whose fader/pan/trim smoothers settle straight to the new value, so a
  // live gain/pan edit would jump (an audible click); an in-place update keeps
  // the smoother state so the change ramps. PDC is recomputed in case the EQ
  // latency changed; the strip pointer is unchanged so the lane binding stays valid.
  for (OwnedStrip& owned : owned_strips_) {
    if (owned.track_id == track_id && owned.strip &&
        strip_inserts_equal(owned.spec.inserts, spec.inserts)) {
      mixing::api::Strip next_spec;
      try {
        next_spec = spec;
      } catch (const std::bad_alloc&) {
        return refuse_out_of_memory();
      } catch (...) {
        return false;
      }
      apply_strip_scalars(*owned.strip, spec, owned.spec);
      if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
        if (!recompute_lane_pdc(*lanes)) {
          // Refused: the previous scalars return, and the live PDC still fits them.
          apply_strip_scalars(*owned.strip, owned.spec, spec);
          return refuse_from_state();
        }
      }
      if (spec.soloed != owned.spec.soloed) apply_spec_solo(track_id, spec.soloed);
      owned.spec = std::move(next_spec);
      prune_lane_sidechains(track_id, spec.inserts.size());
      return true;
    }
  }

  std::unique_ptr<mixing::ChannelStrip> strip;
  try {
    strip = make_channel_strip_from_spec(spec, sample_rate_);
  } catch (const std::bad_alloc&) {
    return refuse_out_of_memory();
  } catch (...) {
    return false;
  }
  if (!strip) return false;
  if (max_block_size_ > 0) {
    strip->prepare(sample_rate_, max_block_size_);
  }
  if (admit != nullptr && !admit(admit_context, track_id, *strip)) return refuse_from_state();

  OwnedStrip* existing = nullptr;
  for (OwnedStrip& owned : owned_strips_) {
    if (owned.track_id == track_id) existing = &owned;
  }
  if (existing == nullptr && owned_strips_.size() >= kMaxTrackLanes) return refuse_from_state();
  mixing::ChannelStrip* raw = strip.get();
  mixing::api::Strip next_spec;
  try {
    next_spec = spec;
    if (existing == nullptr) owned_strips_.reserve(owned_strips_.size() + 1);
  } catch (const std::bad_alloc&) {
    return refuse_out_of_memory();
  } catch (...) {
    return false;
  }
  // Staging refuses only for a full lane table or a latency the other lanes cannot align to.
  StagedTrackStrip staged;
  if (!stage_track_strip(track_id, *raw, &staged)) return refuse_from_state();

  // Control thread, not concurrent with process(): the previous strip is
  // destroyed here, after the lane points away from it.
  commit_track_strip(track_id, raw, staged);
  const bool previous_soloed = existing != nullptr && existing->spec.soloed;
  if (spec.soloed != previous_soloed) apply_spec_solo(track_id, spec.soloed);
  if (existing != nullptr) {
    existing->strip = std::move(strip);
    existing->spec = std::move(next_spec);
  } else {
    owned_strips_.push_back(OwnedStrip{track_id, std::move(strip), std::move(next_spec)});
  }
  prune_lane_sidechains(track_id, spec.inserts.size());
  return true;
}

void TrackMixerRuntime::prepare(double sample_rate, int max_block_size) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : kDefaultDawSampleRate;
  max_block_size_ = std::max(max_block_size, 1);
  scratch_.assign(kMaxTrackLanes * kMaxLaneChannels * static_cast<size_t>(max_block_size_), 0.0f);
  bus_scratch_.assign(kMaxBusLanes * kMaxBusChannels * static_cast<size_t>(max_block_size_), 0.0f);
  key_scratch_.assign(kMaxTrackLanes * kMaxLaneChannels * static_cast<size_t>(max_block_size_),
                      0.0f);
  // No lane has snapshotted yet, so every key plane reads as silence until one
  // does rather than as whatever the previous prepare() left.
  key_frames_.fill(0);
  // Rests at unity so a lane whose gain ramp has not been advanced yet (a strip
  // rendered outside the finish_block sequence) contributes its dry signal
  // rather than silence.
  lane_gain_scratch_.assign(kMaxTrackLanes * static_cast<size_t>(max_block_size_), 1.0f);
  lane_gate_scratch_.assign(kMaxTrackLanes * static_cast<size_t>(max_block_size_), 1.0f);
  send_source_scratch_.assign(2u * kMaxLaneChannels * static_cast<size_t>(max_block_size_), 0.0f);
  // Lane-less sources keep the master's width; only lanes are stereo.
  direct_scratch_.assign(kMaxBusChannels * static_cast<size_t>(max_block_size_), 0.0f);
  // Edge scratch, pre-fader tap and downmix fold for the bus stage.
  bus_edge_scratch_.assign(3u * kMaxBusChannels * static_cast<size_t>(max_block_size_), 0.0f);
  bus_key_scratch_.assign(kMaxBusLanes * kMaxLaneChannels * static_cast<size_t>(max_block_size_),
                          0.0f);
  bus_key_frames_.fill(0);
  keyed_input_scratch_.assign(
      kMaxSidechainBindings * kMaxLaneChannels * static_cast<size_t>(max_block_size_), 0.0f);
  master_key_frames_.fill(0);
  for (LaneState& lane : lane_states_) {
    lane.fader_gain.prepare(sample_rate_, 5.0f);
    lane.pan.prepare(sample_rate_, 5.0f);
    lane.gate.prepare(sample_rate_, 10.0f);
    lane.clip_pdc_delay.set_prepared_channels(kMaxLaneChannels);
    lane.clip_pdc_delay.prepare(sample_rate_, max_block_size_);
    // Same time constant as the stereo pan smoother, so a surround placement
    // glides over the same interval a stereo pan does.
    lane.surround_glide.prepare(sample_rate_, 5.0f);
    lane.surround_primed_channels = -1;
    lane.fader_gain.reset(1.0f);
    lane.pan.reset(0.0f);
    lane.gate.reset(1.0f);
    lane.solo = false;
    lane.mute = false;
    lane.monitor_mode = TrackMonitorMode::kOff;
    if (lane.strip) {
      lane.strip->prepare(sample_rate_, max_block_size_);
    }
  }
  for (BusState& bus : bus_states_) {
    bus.gain.prepare(sample_rate_, 5.0f);
    bus.input_trim_gain.prepare(sample_rate_, 5.0f);
    bus.width.prepare(sample_rate_, max_block_size_);
    // Every slot, configured or not, so the render never meets an unprepared
    // stage and a surround bus finds state for all of its planes.
    bus.panner.prepare(sample_rate_, max_block_size_);
    bus.eq.prepare(sample_rate_, max_block_size_);
    bus.eq.prepare_channels(kMaxBusChannels);
    if (bus.bus) {
      bus.bus->prepare(sample_rate_, max_block_size_);
    }
  }
  for (mixing::AlignmentDelay& delay : lane_pdc_delays_) {
    delay.set_prepared_channels(kMaxLaneChannels);
    delay.prepare(sample_rate_, max_block_size_);
  }
  for (mixing::AlignmentDelay& delay : lane_pre_send_pdc_delays_) {
    delay.set_prepared_channels(kMaxLaneChannels);
    delay.prepare(sample_rate_, max_block_size_);
  }
  direct_pdc_delay_.set_prepared_channels(kMaxBusChannels);
  direct_pdc_delay_.prepare(sample_rate_, max_block_size_);
  for (mixing::AlignmentDelay& delay : lane_in_pdc_delays_) {
    delay.set_prepared_channels(kMaxLaneChannels);
    delay.prepare(sample_rate_, max_block_size_);
  }
  // The bus stage runs on bus and master buffers, which are as wide as the
  // widest layout the mixer renders, not the ≤2-wide lane buffers.
  for (mixing::AlignmentDelay& delay : bus_pdc_delays_) {
    delay.set_prepared_channels(kMaxBusChannels);
    delay.prepare(sample_rate_, max_block_size_);
  }
  master_pdc_delay_.set_prepared_channels(kMaxBusChannels);
  master_pdc_delay_.prepare(sample_rate_, max_block_size_);
  for (mixing::AlignmentDelay& delay : bus_edge_delays_) {
    delay.set_prepared_channels(kMaxBusChannels);
    delay.prepare(sample_rate_, max_block_size_);
  }
  for (mixing::AlignmentDelay& delay : key_edge_delays_) {
    delay.set_prepared_channels(kMaxLaneChannels);
    delay.prepare(sample_rate_, max_block_size_);
  }
  for (auto& sends : bus_sends_) {
    for (auto& send : sends) {
      if (send) send->prepare(sample_rate_, max_block_size_);
    }
  }
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    slot.smoother.prepare(sample_rate_, 5.0f);
    slot.smoother.reset(0.0f);
    slot.active = false;
    slot.assigned = false;
    slot.is_bus = false;
    slot.index = 0;
    slot.insert_index = 0;
    slot.param_id = 0;
  }
  insert_automation_overflow_count_ = 0;
  // Control-side snapshot: prepare() is control-thread only, and current() is
  // the audio thread's view.
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
    prepare_lanes_from_snapshot(*lanes);
    try {
      configure_lane_sends(*lanes);
    } catch (...) {
    }
    if (!recompute_lane_pdc(*lanes)) {
      throw SonareException(ErrorCode::InvalidState, "track mixer master PDC preparation failed");
    }
  }
}

void TrackMixerRuntime::process(float* const* channels, int num_channels, int num_samples) {
  (void)channels;
  (void)num_channels;
  (void)num_samples;
}

void TrackMixerRuntime::reset() {
  for (LaneState& lane : lane_states_) {
    lane.fader_gain.reset(1.0f);
    lane.pan.reset(0.0f);
    lane.gate.reset(1.0f);
    lane.solo = false;
    lane.mute = false;
  }
  flush_pdc_delays();
  flush_clip_pdc_delays();
}

bool TrackMixerRuntime::set_clip_pdc_delay_q8(int delay_samples_q8) noexcept {
  ClipPdcPlan prepared;
  if (!prepare_clip_pdc_delay_q8(delay_samples_q8, prepared)) return false;
  commit_clip_pdc_delay(prepared);
  return true;
}

bool TrackMixerRuntime::prepare_clip_pdc_delay_q8(int delay_samples_q8,
                                                  ClipPdcPlan& out) const noexcept {
  constexpr int kMaxDelayQ8 = mixing::kMaxAlignmentDelaySamples << 8;
  if (delay_samples_q8 < 0) return false;
  // With no lanes there is no clip bank, so a direct-only latency above the lane ceiling is valid.
  const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get();
  if ((lanes == nullptr || lanes->empty()) && delay_samples_q8 > kMaxDelayQ8) {
    try {
      for (size_t lane_index = 0; lane_index < lane_states_.size(); ++lane_index) {
        out.banks[lane_index] = lane_states_[lane_index].clip_pdc_delay;
      }
      out.delay_samples_q8 = delay_samples_q8;
      return true;
    } catch (...) {
      return false;
    }
  }
  // Never clamp only the lane side: lane clips would sit on a different timebase than direct clips.
  if (delay_samples_q8 > kMaxDelayQ8) return false;
  const int bounded = delay_samples_q8;
  // Stage on copies of the live banks so a failure leaves lanes untouched and history survives.
  try {
    for (size_t lane_index = 0; lane_index < lane_states_.size(); ++lane_index) {
      out.banks[lane_index] = lane_states_[lane_index].clip_pdc_delay;
      // Keep the bank wide enough for the lane scratch buffers even when this
      // plan is prepared before the first runtime prepare() call.
      if (out.banks[lane_index].prepared_channels() < kMaxLaneChannels) {
        out.banks[lane_index].set_prepared_channels(kMaxLaneChannels);
      }
      if (!out.banks[lane_index].try_set_delay_samples_q8(bounded)) return false;
    }
    out.delay_samples_q8 = bounded;
    return true;
  } catch (...) {
    return false;
  }
}

void TrackMixerRuntime::commit_clip_pdc_delay(ClipPdcPlan& prepared) noexcept {
  for (size_t lane_index = 0; lane_index < lane_states_.size(); ++lane_index) {
    using std::swap;
    swap(lane_states_[lane_index].clip_pdc_delay, prepared.banks[lane_index]);
  }
  clip_pdc_delay_q8_ = prepared.delay_samples_q8;
}

void TrackMixerRuntime::flush_clip_pdc_delays() noexcept {
  for (LaneState& lane : lane_states_) lane.clip_pdc_delay.reset();
}

void TrackMixerRuntime::settle_smoothers() noexcept {
  for (LaneState& lane : lane_states_) {
    lane.fader_gain.reset(lane.fader_gain.target());
    lane.pan.reset(lane.pan.target());
    lane.gate.reset(lane.gate.target());
    // The surround placement glides too, so a pre-roll settle quiesces it for
    // the same reason it quiesces the fader: otherwise the first audible block
    // glides into placement instead of opening at it.
    lane.surround_glide.settle();
    // Quiesce the lane's channel-strip gain stages too so the first rendered
    // block opens without an insert/fader ramp-in.
    if (lane.strip != nullptr) lane.strip->settle();
  }
  for (BusState& bus : bus_states_) {
    bus.gain.reset(bus.gain.target());
    bus.input_trim_gain.reset(bus.input_trim_gain.target());
    // Seed the width smoother from its target too; set_width() only stores the
    // target, so without this an offline pre-roll glides width from 1.0 over the
    // first audible block instead of opening at the configured width.
    bus.width.reset();
    bus.panner.reset();
  }
  for (auto& sends : bus_sends_) {
    for (auto& send : sends) {
      if (send) send->reset();
    }
  }
  settle_insert_automations();
}

void TrackMixerRuntime::settle_insert_automations() noexcept {
  // Structural replay restores insert targets without changing unrelated
  // fader and pan ramps in flight.
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (!slot.active) continue;
    const float target = slot.smoother.target();
    slot.smoother.reset(target);
    if (slot.is_bus) {
      const int bus_index = configured_bus_index(slot.bus_id);
      if (bus_index < 0) {
        slot.active = false;
        slot.assigned = false;
        continue;
      }
      slot.index = static_cast<size_t>(bus_index);
      apply_bus_insert_parameter(slot.index, slot.insert_index, slot.param_id, target);
    } else {
      apply_lane_insert_parameter(slot.index, slot.insert_index, slot.param_id, target);
    }
    slot.active = false;
  }
}

void TrackMixerRuntime::settle_insert_automations(const std::vector<TrackLaneConfig>& next_lanes,
                                                  bool preserve_surviving_lanes) noexcept {
  if (!preserve_surviving_lanes) {
    settle_insert_automations();
    return;
  }
  // Surviving lane ramps are identity-keyed and stay unsettled; bus slots are never settled here.
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (!slot.active || slot.is_bus) continue;
    const uint32_t track_id =
        slot.index < lane_states_.size() ? lane_states_[slot.index].track_id : 0;
    const bool survives = track_id != 0 && std::any_of(next_lanes.begin(), next_lanes.end(),
                                                       [track_id](const TrackLaneConfig& lane) {
                                                         return lane.track_id == track_id;
                                                       });
    if (survives) continue;
    // The strip is leaving the published graph. Do not apply its pending
    // target to an object that no longer renders; retire only this slot.
    slot.active = false;
    slot.assigned = false;
  }
}

bool TrackMixerRuntime::lane_config_valid(
    const std::vector<TrackLaneConfig>& lanes) const noexcept {
  if (lanes.size() > kMaxTrackLanes) return false;
  for (size_t i = 0; i < lanes.size(); ++i) {
    if (lanes[i].track_id == 0) return false;
    if (lanes[i].source_layout != ChannelLayout::Stereo) return false;
    if (lanes[i].output_bus_id != 0 && bus_state_for(lanes[i].output_bus_id) == nullptr) {
      return false;
    }
    if (lanes[i].sends.size() > mixing::ChannelStrip::kMaxSends) return false;
    for (size_t send_index = 0; send_index < lanes[i].sends.size(); ++send_index) {
      const TrackLaneConfig::Send& send = lanes[i].sends[send_index];
      if (send.bus_id == 0 || !gain_db_in_range(send.level_db) ||
          bus_state_for(send.bus_id) == nullptr) {
        return false;
      }
      for (size_t other = send_index + 1; other < lanes[i].sends.size(); ++other) {
        if (send.bus_id == lanes[i].sends[other].bus_id) return false;
      }
    }
    for (size_t j = i + 1; j < lanes.size(); ++j) {
      if (lanes[i].track_id == lanes[j].track_id) return false;
    }
  }
  return true;
}

mixing::ChannelStrip* TrackMixerRuntime::owned_strip_for(uint32_t track_id) noexcept {
  for (OwnedStrip& owned : owned_strips_) {
    if (owned.track_id == track_id) {
      return owned.strip.get();
    }
  }
  return nullptr;
}

mixing::ChannelStrip* TrackMixerRuntime::bound_strip_for(uint32_t track_id) const noexcept {
  for (const TrackStripBinding& binding : track_strip_bindings_) {
    if (binding.track_id == track_id) {
      return binding.strip;
    }
  }
  return nullptr;
}

bool TrackMixerRuntime::track_insert_processor_name(uint32_t track_id, unsigned int insert_index,
                                                    std::string* out_name) const noexcept {
  if (out_name == nullptr || track_insert_count(track_id) == 0) return false;
  mixing::ChannelStrip* strip = bound_strip_for(track_id);
  if (strip == nullptr) return false;
  for (const OwnedStrip& owned : owned_strips_) {
    if (owned.track_id != track_id || owned.strip.get() != strip) continue;
    const std::string* name = strip_insert_processor_name_at(owned.spec, insert_index);
    if (name == nullptr) return false;
    *out_name = *name;
    return true;
  }
  // No matching owned entry: the bound strip is externally bound
  // (bind_track_strip), which carries no retained spec to read a name from.
  return false;
}

bool TrackMixerRuntime::track_lane_position(uint32_t track_id,
                                            uint32_t* out_position) const noexcept {
  if (track_id == 0 || out_position == nullptr) return false;
  const std::shared_ptr<const std::vector<TrackLaneConfig>> lanes = lanes_.control_current();
  if (lanes == nullptr) return false;
  for (size_t index = 0; index < lanes->size() && index < kMaxTrackLanes; ++index) {
    if ((*lanes)[index].track_id != track_id) continue;
    *out_position = static_cast<uint32_t>(index);
    return true;
  }
  return false;
}

size_t TrackMixerRuntime::copy_control_lane_track_ids(uint32_t* out,
                                                      size_t capacity) const noexcept {
  const std::shared_ptr<const std::vector<TrackLaneConfig>> lanes = lanes_.control_current();
  if (out == nullptr || lanes == nullptr) return 0;
  size_t count = 0;
  for (const TrackLaneConfig& lane : *lanes) {
    if (count == capacity) break;
    out[count++] = lane.track_id;
  }
  return count;
}

size_t TrackMixerRuntime::bound_track_insert_count(uint32_t track_id) const noexcept {
  const mixing::ChannelStrip* strip = bound_strip_for(track_id);
  return strip != nullptr ? strip->num_pre_inserts() + strip->num_post_inserts() : 0;
}

size_t TrackMixerRuntime::track_insert_count(uint32_t track_id) const noexcept {
  uint32_t position = 0;
  return track_lane_position(track_id, &position) ? bound_track_insert_count(track_id) : 0;
}

const rt::ProcessorBase* TrackMixerRuntime::track_insert_processor(
    uint32_t track_id, unsigned int insert_index) const noexcept {
  if (insert_index >= track_insert_count(track_id)) return nullptr;
  return bound_strip_for(track_id)->insert_processor(insert_index);
}

bool TrackMixerRuntime::lane_insert_holds(uint32_t track_id, unsigned int insert_index,
                                          const InsertProcessorLayout& processor) const noexcept {
  const int lane = lane_index_for_track(track_id);
  if (lane < 0) return false;
  const mixing::ChannelStrip* strip = lane_states_[static_cast<size_t>(lane)].strip;
  return strip != nullptr &&
         insert_processor_layout(strip->insert_processor(insert_index)) == processor;
}

void TrackMixerRuntime::record_track_strip_binding(uint32_t track_id, mixing::ChannelStrip* strip) {
  if (track_id == 0) return;
  for (TrackStripBinding& binding : track_strip_bindings_) {
    if (binding.track_id == track_id) {
      binding.strip = strip;
      return;
    }
  }
  track_strip_bindings_.push_back(TrackStripBinding{track_id, strip});
}

std::unique_ptr<mixing::ChannelStrip> TrackMixerRuntime::make_seed_strip() const {
  mixing::ChannelStripConfig config{
      0.0f, 0.0f, mixing::PanLaw::Linear0dB, 5.0f, mixing::EqPosition::PreFader, 0.0f, false};
  config.mono_pan_centre_unity = true;
  auto strip = std::make_unique<mixing::ChannelStrip>(config);
  if (max_block_size_ > 0) {
    strip->prepare(sample_rate_, max_block_size_);
  }
  return strip;
}

mixing::ChannelStrip* TrackMixerRuntime::ensure_owned_strip_for(uint32_t track_id) {
  if (mixing::ChannelStrip* strip = owned_strip_for(track_id)) {
    return strip;
  }
  if (owned_strips_.size() >= kMaxTrackLanes) {
    return nullptr;
  }
  std::unique_ptr<mixing::ChannelStrip> strip = make_seed_strip();
  mixing::ChannelStrip* raw = strip.get();
  // Automation-seeded strip: no spec applied yet, so leave spec default (a later
  // set_track_strip will see a differing insert topology and rebuild).
  owned_strips_.push_back(OwnedStrip{track_id, std::move(strip), {}});
  return raw;
}

void TrackMixerRuntime::prepare_lanes_from_snapshot(
    const std::vector<TrackLaneConfig>& lanes) noexcept {
  prepare_lanes_from_snapshot(lanes, nullptr);
}

void TrackMixerRuntime::prepare_lanes_from_snapshot(
    const std::vector<TrackLaneConfig>& lanes,
    const std::array<mixing::ChannelStrip*, kMaxTrackLanes>* candidate_strips) noexcept {
  // Audio thread, noexcept: LaneState holds AlignmentDelay vectors, so move banks, never copy.
  // Slots are staged in a fixed array and moved by identity; new identities reset delay history.
  std::array<LaneState, kMaxTrackLanes> previous = std::move(lane_states_);
  std::array<LaneState, kMaxTrackLanes> next;
  std::array<int, kMaxTrackLanes> matching_previous{};
  std::array<bool, kMaxTrackLanes> reserved_previous{};
  matching_previous.fill(-1);
  std::array<bool, kMaxTrackLanes> moved_previous{};
  const size_t previous_active_count =
      std::min(applied_lane_count_, static_cast<size_t>(kMaxTrackLanes));

  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    const uint32_t track_id = lanes[lane_index].track_id;
    for (size_t previous_index = 0; previous_index < previous.size(); ++previous_index) {
      if (!reserved_previous[previous_index] && previous[previous_index].track_id == track_id) {
        matching_previous[lane_index] = static_cast<int>(previous_index);
        reserved_previous[previous_index] = true;
        break;
      }
    }
  }

  const auto reset_state = [this](LaneState& lane, uint32_t track_id) noexcept {
    lane.track_id = track_id;
    lane.fader_gain.prepare(sample_rate_, 5.0f);
    lane.pan.prepare(sample_rate_, 5.0f);
    lane.gate.prepare(sample_rate_, 10.0f);
    lane.surround_glide.prepare(sample_rate_, 5.0f);
    lane.fader_gain.reset(1.0f);
    lane.pan.reset(0.0f);
    lane.gate.reset(1.0f);
    lane.solo = false;
    lane.mute = false;
    lane.monitor_mode = TrackMonitorMode::kOff;
    lane.strip = nullptr;
    // A new track identity must not inherit the previous occupant's clip delay audio.
    lane.clip_pdc_delay.reset();
    lane.surround_primed_channels = -1;
  };

  // Move all surviving identities first. Reserving their old slots before the
  // fallback pass prevents a new lane at an earlier position from stealing a
  // bank that a later lane still owns (old [A,B] -> new [C,A]).
  for (size_t next_index = 0; next_index < lanes.size(); ++next_index) {
    const int source_index = matching_previous[next_index];
    if (source_index < 0) continue;
    const size_t source = static_cast<size_t>(source_index);
    next[next_index] = std::move(previous[source]);
    moved_previous[source] = true;
    // A matching id in the inactive tail is a re-add. Keep its control state
    // (solo/mute/fader/strip) for compatibility, but never resurrect delayed
    // audio that was in flight when the track was removed.
    if (source >= previous_active_count) next[next_index].clip_pdc_delay.reset();
  }

  // Fill new configured slots from blank old banks first, preserving retired
  // control metadata in the inactive tail whenever capacity permits. If all
  // banks are occupied, a retired state is evicted for the genuinely new id.
  for (size_t next_index = 0; next_index < lanes.size(); ++next_index) {
    if (matching_previous[next_index] >= 0) continue;
    int source_index = -1;
    for (size_t previous_index = 0; previous_index < previous.size(); ++previous_index) {
      if (!moved_previous[previous_index] && previous[previous_index].track_id == 0) {
        source_index = static_cast<int>(previous_index);
        break;
      }
    }
    if (source_index < 0) {
      for (size_t previous_index = 0; previous_index < previous.size(); ++previous_index) {
        if (!moved_previous[previous_index]) {
          source_index = static_cast<int>(previous_index);
          break;
        }
      }
    }
    if (source_index >= 0) {
      const size_t source = static_cast<size_t>(source_index);
      next[next_index] = std::move(previous[source]);
      moved_previous[source] = true;
    }
    reset_state(next[next_index], lanes[next_index].track_id);
  }

  // Retain retired identities in the inactive tail so their public mixer state
  // follows a temporary removal. Their raw clip bank is explicitly flushed.
  size_t next_index = lanes.size();
  for (; next_index < next.size(); ++next_index) {
    int source_index = -1;
    for (size_t previous_index = 0; previous_index < previous.size(); ++previous_index) {
      if (!moved_previous[previous_index] && previous[previous_index].track_id != 0) {
        source_index = static_cast<int>(previous_index);
        break;
      }
    }
    if (source_index >= 0) {
      const size_t source = static_cast<size_t>(source_index);
      next[next_index] = std::move(previous[source]);
      moved_previous[source] = true;
      next[next_index].clip_pdc_delay.reset();
      continue;
    }
    for (size_t previous_index = 0; previous_index < previous.size(); ++previous_index) {
      if (!moved_previous[previous_index]) {
        source_index = static_cast<int>(previous_index);
        break;
      }
    }
    if (source_index >= 0) {
      const size_t source = static_cast<size_t>(source_index);
      next[next_index] = std::move(previous[source]);
      moved_previous[source] = true;
    }
    reset_state(next[next_index], 0);
  }

  lane_states_ = std::move(next);
  if (candidate_strips != nullptr) {
    for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
      lane_states_[lane_index].strip = (*candidate_strips)[lane_index];
    }
  }
  applied_lane_snapshot_ = &lanes;
  applied_lane_count_ = lanes.size();
  for (size_t lane_index = 0; lane_index < kMaxTrackLanes; ++lane_index) {
    LaneRoute& route = lane_routes_[lane_index];
    route = LaneRoute{};
    if (lane_index >= lanes.size()) continue;
    const TrackLaneConfig& lane = lanes[lane_index];
    route.output_bus_id = lane.output_bus_id;
    route.send_count = std::min(lane.sends.size(), route.send_bus_ids.size());
    for (size_t send = 0; send < route.send_count; ++send) {
      route.send_bus_ids[send] = lane.sends[send].bus_id;
    }
  }
  resolve_lane_routes();
  // Slot identities were restaged, so no published reading still belongs to its slot.
  for (InsertGainReductionBoard& board : lane_insert_gr_boards_) board.clear();
}

void TrackMixerRuntime::remap_lane_insert_automations(
    const std::vector<TrackLaneConfig>& lanes) noexcept {
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (!slot.assigned || slot.is_bus) continue;
    const uint32_t old_track_id =
        slot.index < lane_states_.size() ? lane_states_[slot.index].track_id : 0;
    size_t next_index = 0;
    while (next_index < lanes.size() && lanes[next_index].track_id != old_track_id) {
      ++next_index;
    }
    if (old_track_id == 0 || next_index == lanes.size()) {
      slot.active = false;
      slot.assigned = false;
      continue;
    }
    slot.index = next_index;
    // The current/target pair and active state belong to the track identity,
    // so leave them intact across a lane reorder or republish. An explicit
    // settle_smoothers() call is the opt-in path for snapping to target.
  }
}

void TrackMixerRuntime::configure_lane_sends(const std::vector<TrackLaneConfig>& lanes,
                                             uint32_t only_track_id) {
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    const TrackLaneConfig& config = lanes[lane_index];
    if (only_track_id != 0 && config.track_id != only_track_id) continue;
    mixing::ChannelStrip* strip = lane_states_[lane_index].strip;
    if (!strip && !config.sends.empty()) {
      strip = ensure_owned_strip_for(config.track_id);
      lane_states_[lane_index].strip = strip;
      if (strip != nullptr) {
        // Send-seeded strips bypass bind_track_strip, so mirror them into the
        // control-thread binding table here.
        record_track_strip_binding(config.track_id, strip);
      }
    }
    if (strip) configure_strip_sends(config, *strip);
  }
}

void TrackMixerRuntime::configure_strip_sends(const TrackLaneConfig& config,
                                              mixing::ChannelStrip& strip) const {
  if (config.sends.size() > mixing::ChannelStrip::kMaxSends) {
    throw std::invalid_argument("track send count exceeds the strip send cap");
  }
  std::array<mixing::SendConfig, mixing::ChannelStrip::kMaxSends> configs{};
  for (size_t send_index = 0; send_index < config.sends.size(); ++send_index) {
    const TrackLaneConfig::Send& send = config.sends[send_index];
    if (bus_state_for(send.bus_id) == nullptr) {
      throw std::invalid_argument("track send references an unknown bus");
    }
    configs[send_index] =
        mixing::SendConfig{send.enabled ? send.level_db : kFloorDb, send.timing, 5.0f};
  }
  mixing::ChannelStrip::PreparedSends prepared;
  if (!strip.prepare_sends(configs.data(), config.sends.size(), prepared)) {
    throw std::bad_alloc();
  }
  strip.commit_sends(prepared);
}

void TrackMixerRuntime::flush_meters() noexcept {
  for (LaneState& lane : lane_states_) {
    if (lane.strip != nullptr) lane.strip->flush_meters();
  }
  for (BusState& bus : bus_states_) {
    if (bus.bus) bus.bus->flush_meters();
  }
}

void TrackMixerRuntime::reset_processing() noexcept {
  // Adopt the published lanes first so a strip bound since the last block is reset too.
  acquire_lanes();
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (lanes != nullptr && !scratch_.empty() && lanes != applied_lane_snapshot_) {
    prepare_lanes_from_snapshot(*lanes);
  }
  for (LaneState& lane : lane_states_) {
    if (lane.strip != nullptr) lane.strip->reset_processing();
  }
  for (BusState& bus : bus_states_) {
    bus.eq.reset();
    if (bus.bus) bus.bus->reset_processing();
  }
  for (auto& sends : bus_sends_) {
    for (auto& send : sends) {
      if (send) send->reset();
    }
  }
  // Also clears key_edge_delays_ and every lane/bus alignment bank.
  flush_pdc_delays();
  flush_clip_pdc_delays();
}

}  // namespace sonare::engine
