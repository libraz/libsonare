#include "mixing/channel_strip.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include "mixing/tail_utils.h"
#include "util/exception.h"
#include "util/insertion_sort.h"
#include "util/numeric_validation.h"

namespace sonare::mixing {

namespace {

void zero_taps(std::vector<std::vector<float>>& taps, int num_channels, int num_samples) {
  const int rows = std::min<int>(num_channels, static_cast<int>(taps.size()));
  for (int ch = 0; ch < rows; ++ch) {
    const int n = std::min<int>(num_samples, static_cast<int>(taps[ch].size()));
    std::fill(taps[ch].begin(), taps[ch].begin() + n, 0.0f);
  }
}

void zero_taps(std::vector<std::vector<float>>& taps, int num_channels, int start,
               int num_samples) {
  const int rows = std::min<int>(num_channels, static_cast<int>(taps.size()));
  for (int ch = 0; ch < rows; ++ch) {
    const int begin = std::min<int>(start, static_cast<int>(taps[ch].size()));
    const int end = std::min<int>(start + num_samples, static_cast<int>(taps[ch].size()));
    if (begin < end) {
      std::fill(taps[ch].begin() + begin, taps[ch].begin() + end, 0.0f);
    }
  }
}

void copy_to_taps(float* const* channels, std::vector<std::vector<float>>& taps, int num_channels,
                  int num_samples, int tap_offset = 0) {
  if (channels == nullptr || num_samples <= 0) return;
  // Visit every prepared row, not just the rows present in this block. A
  // shorter layout or a null source plane must publish silence into the tap;
  // leaving the old row in place makes the next aux send replay stale audio.
  const int rows = static_cast<int>(taps.size());
  for (int ch = 0; ch < rows; ++ch) {
    const int begin = std::min<int>(tap_offset, static_cast<int>(taps[ch].size()));
    const int end = std::min<int>(tap_offset + num_samples, static_cast<int>(taps[ch].size()));
    if (begin >= end) continue;
    if (ch >= num_channels || channels[ch] == nullptr) {
      std::fill(taps[ch].begin() + begin, taps[ch].begin() + end, 0.0f);
    } else {
      std::copy(channels[ch], channels[ch] + (end - begin), taps[ch].begin() + begin);
    }
  }
}

int total_latency_q8(const std::vector<std::unique_ptr<rt::ProcessorBase>>& inserts) noexcept {
  int total = 0;
  for (const auto& insert : inserts) {
    total += insert->latency_samples_q8();
  }
  return total;
}

// The meter reports the reduction the block actually took, so a bypassed insert
// contributes nothing. The hidden processor runs on scratch to keep its state
// current, but its gain reduction is not applied to the audible bypass path.
// Exclude that hidden reading from the strip snapshot, as the mute path also
// forces inaudible reduction to 0 dB.
void fold_insert_gain_reduction_db(const std::vector<std::unique_ptr<rt::ProcessorBase>>& inserts,
                                   float* out) noexcept {
  for (size_t i = 0; i < inserts.size(); ++i) {
    if (inserts[i]->bypassed()) continue;
    out[i] = std::min(out[i], inserts[i]->last_gain_reduction_db());
  }
}

float deepest_gain_reduction_db(const float* values, size_t count) noexcept {
  float reduction_db = 0.0f;
  for (size_t i = 0; i < count; ++i) reduction_db = std::min(reduction_db, values[i]);
  return reduction_db;
}

// Stages @p lane's events for [range_start, range_start + num_samples) of the
// block at @p block_start, with offsets relative to that block.
template <typename Lane, typename Staging>
void stage_events(Lane& lane, int64_t block_start, int range_start, int num_samples,
                  Staging& dest) {
  lane.consume_block(
      numeric::saturating_add(block_start, static_cast<int64_t>(range_start)), num_samples,
      [&](const AutomationBlockEvent& event) {
        AutomationBlockEvent shifted = event;
        shifted.offset += range_start;
        // Simultaneous points of one target resolve to the last.
        dest.push_coalescing(
            shifted, [](const AutomationBlockEvent& last, const AutomationBlockEvent& next) {
              return last.offset == next.offset && last.event.target == next.event.target;
            });
      });
}

template <typename Staging>
void sort_events_by_offset(Staging& events) {
  insertion_sort(events.begin(), events.end(),
                 [](const AutomationBlockEvent& lhs, const AutomationBlockEvent& rhs) {
                   if (lhs.offset != rhs.offset) return lhs.offset < rhs.offset;
                   return static_cast<int>(lhs.event.target.kind) <
                          static_cast<int>(rhs.event.target.kind);
                 });
}

// Offset of the staged event at @p index, or @p fallback past the end.
template <typename Staging>
int offset_or(const Staging& events, size_t index, int fallback) {
  return index < events.size() ? events[index].offset : fallback;
}

}  // namespace

ChannelStrip::ChannelStrip(ChannelStripConfig config)
    : input_trim_({config.input_trim_db, config.smoothing_ms}),
      fader_({config.fader_db, config.smoothing_ms}),
      panner_({config.pan, config.pan_law, config.smoothing_ms}),
      width_(1.0f, config.smoothing_ms),
      metering_enabled_(config.enable_metering),
      meter_config_(config.meter),
      eq_position_(config.eq_position) {
  // Pre-reserve the vectors that the audio thread iterates while the control
  // thread may concurrently push_back into. The audio thread iterates
  // insert_automation_ in process_at() and insert_sidechains_ in
  // process_insert_chain(); a reallocation here would invalidate those
  // iterators / pointers (C++ UB). Caps are enforced in schedule_insert_
  // automation() and add_pre/post_insert(); see channel_strip.h.
  insert_automation_.reserve(kMaxInsertAutomationLanes);
  insert_sidechains_.reserve(kMaxInserts);
  stereo_pair_alignment_delays_.reserve(kMaxInserts);
  bypass_alignment_delays_.reserve(kMaxInserts);
  pre_inserts_.reserve(kMaxInserts);
  post_inserts_.reserve(kMaxInserts);
  pre_insert_spo_.reserve(kMaxInserts);
  post_insert_spo_.reserve(kMaxInserts);
  sends_.reserve(kMaxSends);
  send_automation_.reserve(kMaxSends);
}

void ChannelStrip::prepare(double sample_rate, int max_block_size) {
  sample_rate_ = sample_rate;
  max_block_size_ = std::max(0, max_block_size);

  input_trim_.prepare(sample_rate, max_block_size);
  fader_.prepare(sample_rate, max_block_size);
  panner_.prepare(sample_rate, max_block_size);
  width_.prepare(sample_rate, max_block_size);
  // The alignment delay must preallocate storage for every plane this strip
  // will be handed so the audio thread never allocates and never silently drops
  // the upper channels. process()/process_segment() cap at kMaxStackChannels,
  // but process_unsegmented() -- which every wider layout takes -- does not, so
  // the bank is sized from the width the host declared, not from the stack cap.
  alignment_delay_.set_prepared_channels(prepared_channels_);
  alignment_delay_.prepare(sample_rate, max_block_size);
  // Preallocates filter state for kRealtimePreparedChannels; process() never allocates.
  eq_.prepare(sample_rate, max_block_size);
  pre_meter_.reset();
  post_meter_.reset();
  if (metering_enabled_) {
    pre_meter_.emplace(meter_config_);
    post_meter_.emplace(meter_config_);
    pre_meter_->prepare(sample_rate, max_block_size);
    post_meter_->prepare(sample_rate, max_block_size);
  }
  for (auto& insert : pre_inserts_) {
    insert->prepare(sample_rate_, max_block_size_);
  }
  for (auto& insert : post_inserts_) {
    insert->prepare(sample_rate_, max_block_size_);
  }
  // After the inserts: an insert only reports its final latency once prepared.
  prepare_insert_alignment_delays();
  for (auto& send : sends_) {
    send->prepare(sample_rate, max_block_size);
  }

  const auto cols = static_cast<size_t>(max_block_size_);
  // The taps and the send scratch span every plane the strip may be handed: a
  // send is as wide as the caller asks (a surround return strip sends its whole
  // bed), and the pre-fader meter has to observe the planes the strip processes.
  const auto tap_rows = static_cast<size_t>(std::max(kPreparedChannels, prepared_channels_));
  pre_tap_.assign(tap_rows, std::vector<float>(cols, 0.0f));
  post_tap_.assign(tap_rows, std::vector<float>(cols, 0.0f));
  send_temp_.assign(tap_rows, std::vector<float>(cols, 0.0f));
  send_temp_channels_.resize(tap_rows);
  for (size_t row = 0; row < tap_rows; ++row) {
    send_temp_channels_[row] = send_temp_[row].data();
  }
  null_planes_.assign(tap_rows, std::vector<float>(cols, 0.0f));
  stage_channels_.assign(tap_rows, nullptr);
  bypass_scratch_.assign(tap_rows, std::vector<float>(cols, 0.0f));
  bypass_scratch_channels_.resize(tap_rows);
  for (size_t row = 0; row < tap_rows; ++row) {
    bypass_scratch_channels_[row] = bypass_scratch_[row].data();
  }
}

void ChannelStrip::process(float* const* channels, int num_channels, int num_samples) {
  process_at(channels, num_channels, num_samples, 0);
}

int ChannelStrip::automation_chunk_samples(int64_t range_start, int num_samples,
                                           size_t insert_lanes) noexcept {
  const auto fits = [&](int chunk) {
    constexpr size_t kCap = kMaxAutomationEventsPerBlock;
    if (fader_automation_.count_block_offsets(range_start, chunk) > kCap ||
        pan_automation_.count_block_offsets(range_start, chunk) > kCap ||
        width_automation_.count_block_offsets(range_start, chunk) > kCap) {
      return false;
    }
    size_t inserts = 0;
    for (size_t li = 0; li < insert_lanes; ++li) {
      if (insert_automation_[li].lane) {
        inserts += insert_automation_[li].lane->count_block_offsets(range_start, chunk);
      }
    }
    return inserts <= kCap;
  };
  // One sample holds at most one offset per lane, and the lane cap is below the
  // staging capacity, so the halving always ends.
  int chunk = num_samples;
  while (chunk > 1 && !fits(chunk)) chunk = (chunk + 1) / 2;
  return chunk;
}

void ChannelStrip::stage_automation(int64_t block_start, int range_start, int num_samples,
                                    size_t insert_lanes, AutomationStaging& fader,
                                    AutomationStaging& pan, AutomationStaging& width,
                                    AutomationStaging& inserts) {
  fader.clear();
  pan.clear();
  width.clear();
  inserts.clear();
  stage_events(fader_automation_, block_start, range_start, num_samples, fader);
  stage_events(pan_automation_, block_start, range_start, num_samples, pan);
  stage_events(width_automation_, block_start, range_start, num_samples, width);
  for (size_t li = 0; li < insert_lanes; ++li) {
    if (insert_automation_[li].lane) {
      stage_events(*insert_automation_[li].lane, block_start, range_start, num_samples, inserts);
    }
  }
  sort_events_by_offset(inserts);
}

void ChannelStrip::process_at(float* const* channels, int num_channels, int num_samples,
                              int64_t block_start) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  // The EQ, the inserts and the meters each run once per segment below, so the
  // deltas are read here and checked at whichever exit the block takes; either
  // way the block bumps at most once.
  const uint32_t eq_discards_before = eq_.non_finite_discard_count();
  const uint64_t member_discards_before = member_discard_sum();
  const auto note_member_discards = [this, eq_discards_before, member_discards_before]() noexcept {
    if (eq_.non_finite_discard_count() != eq_discards_before ||
        member_discard_sum() != member_discards_before) {
      note_non_finite_discard();
    }
  };
  // AUDIO-THREAD ONLY. discard_before() and consume_block() on an
  // AutomationLane are both consumer-side and mutate the lane's
  // active_event_/has_active_event_ state, so they must be serialized.
  // All process_at() call sites are audio-thread (RealtimeEngine,
  // MixingRuntime, MonitorRuntime, graph-runtime StripNode); the control
  // thread only ever calls push() via the schedule_*_automation() APIs, so
  // the SPSC contract documented on AutomationLane is preserved.
  for (auto& lane : send_automation_) {
    if (lane) lane->discard_before(block_start);
  }

  // Drain the parameter-automation SPSC lanes UNCONDITIONALLY, before the
  // wide-channel fast path. If we early-returned here without consuming, the
  // control thread would keep pushing events the audio thread never pulls, so
  // the bounded ring would overflow and sample-accurate automation would be
  // permanently lost on > kMaxStackChannels layouts (e.g. 7.1.4). We consume
  // first, then (for wide layouts) apply the events to advance parameter state
  // and fall back to the unsegmented path.
  AutomationStaging fader_events;
  AutomationStaging pan_events;
  AutomationStaging width_events;
  AutomationStaging insert_events;
  // Audio thread: read the published lane count with acquire ordering and
  // iterate by index over [0, lanes_size). Range-for would read the vector's
  // non-atomic size_ member, which races with the control thread's push_back.
  const size_t lanes_size = insert_automation_size_.load(std::memory_order_acquire);
  // Automation denser than the staging holds is consumed and applied in shorter
  // chunks, so no accepted breakpoint is dropped or moved.
  const auto stage_chunk = [&](int chunk_start) {
    const int chunk = automation_chunk_samples(
        numeric::saturating_add(block_start, static_cast<int64_t>(chunk_start)),
        num_samples - chunk_start, lanes_size);
    stage_automation(block_start, chunk_start, chunk, lanes_size, fader_events, pan_events,
                     width_events, insert_events);
    return chunk_start + chunk;
  };
  const auto staged_refusals = [&]() {
    return fader_events.refused() + pan_events.refused() + width_events.refused() +
           insert_events.refused();
  };
  int chunk_end = stage_chunk(0);

  const bool muted = effectively_muted();
  if (num_channels > kMaxStackChannels) {
    // Wide layouts cannot use the segmented stack-array path. Apply the drained
    // events in order to advance fader / pan / width / insert parameters to their
    // block-final values, then process unsegmented.
    for (;;) {
      for (const auto* staging : {&fader_events, &pan_events, &width_events, &insert_events}) {
        for (size_t i = 0; i < staging->size(); ++i) apply_automation_event((*staging)[i].event);
      }
      if (chunk_end >= num_samples) break;
      chunk_end = stage_chunk(chunk_end);
    }
    automation_staging_refusals_.add(static_cast<uint32_t>(staged_refusals()));
    process_unsegmented(channels, num_channels, num_samples);
    note_member_discards();
    return;
  }

  if (chunk_end >= num_samples && fader_events.empty() && pan_events.empty() &&
      width_events.empty() && insert_events.empty()) {
    process_unsegmented(channels, num_channels, num_samples);
    note_member_discards();
    return;
  }

  const int clamped_samples = std::min(num_samples, max_block_size_);
  if (muted) {
    // Run the segmented path on silence so automation lands at its sample offsets.
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] != nullptr) {
        std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
      }
    }
  }
  zero_taps(pre_tap_, num_channels, 0, clamped_samples);
  zero_taps(post_tap_, num_channels, 0, clamped_samples);

  int cursor = 0;
  // Track the block-representative (most-negative) gain reduction across every
  // segment. last_gain_reduction_db() reflects only the most recently processed
  // segment, so sampling it once after the loop would report the final
  // segment's GR rather than the block maximum. Accumulate the per-segment max
  // so the segmented path agrees with the unsegmented path's block-level value.
  insert_gain_reduction_count_ = pre_inserts_.size() + post_inserts_.size();
  std::fill_n(insert_gain_reduction_db_.begin(), insert_gain_reduction_count_, 0.0f);
  for (;;) {
    size_t fader_index = 0;
    size_t pan_index = 0;
    size_t width_index = 0;
    size_t insert_index = 0;
    while (cursor < chunk_end) {
      while (fader_index < fader_events.size() && fader_events[fader_index].offset == cursor) {
        apply_automation_event(fader_events[fader_index++].event);
      }
      while (pan_index < pan_events.size() && pan_events[pan_index].offset == cursor) {
        apply_automation_event(pan_events[pan_index++].event);
      }
      while (width_index < width_events.size() && width_events[width_index].offset == cursor) {
        apply_automation_event(width_events[width_index++].event);
      }
      while (insert_index < insert_events.size() && insert_events[insert_index].offset == cursor) {
        apply_automation_event(insert_events[insert_index++].event);
      }

      const int next_offset = std::min({chunk_end, offset_or(fader_events, fader_index, chunk_end),
                                        offset_or(pan_events, pan_index, chunk_end),
                                        offset_or(width_events, width_index, chunk_end),
                                        offset_or(insert_events, insert_index, chunk_end)});
      const int segment_samples = std::max(0, next_offset - cursor);
      if (segment_samples > 0) {
        process_segment(channels, num_channels, cursor, segment_samples, cursor);
        if (!muted) {
          fold_insert_gain_reduction_db(pre_inserts_, insert_gain_reduction_db_.data());
          fold_insert_gain_reduction_db(post_inserts_,
                                        insert_gain_reduction_db_.data() + pre_inserts_.size());
        }
        cursor += segment_samples;
      } else {
        // Defensive guard for duplicate or unsorted offsets; consume matching events next loop.
        ++cursor;
      }
    }
    if (chunk_end >= num_samples) break;
    chunk_end = stage_chunk(chunk_end);
  }
  automation_staging_refusals_.add(static_cast<uint32_t>(staged_refusals()));

  if (muted) {
    // Keep the inserts' state advance but publish exact silence.
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] != nullptr) {
        std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
      }
    }
    zero_taps(pre_tap_, num_channels, 0, clamped_samples);
    zero_taps(post_tap_, num_channels, 0, clamped_samples);
  }

  // post GR is clamped to be no less aggressive than pre GR for snapshot
  // consistency, matching the unsegmented path.
  const float pre_gain_reduction_db =
      deepest_gain_reduction_db(insert_gain_reduction_db_.data(), pre_inserts_.size());
  const float post_gain_reduction_db =
      std::min(pre_gain_reduction_db,
               deepest_gain_reduction_db(insert_gain_reduction_db_.data() + pre_inserts_.size(),
                                         post_inserts_.size()));

  // Meter every plane the strip just processed, not just the front pair. The
  // unsegmented path already drives the pre meter from the full-width buffer;
  // this path has to agree, or the observed channel_count would depend on
  // whether an automation event happened to land in the block. That mattered
  // beyond the per-plane readings: the integrated-LUFS histogram is
  // instance-lifetime state, so energies computed over two different channel
  // counts mixed into one histogram and permanently skewed integrated_lufs.
  std::array<float*, kMaxStackChannels> pre_meter_channels{};
  const int meter_rows =
      std::min<int>({num_channels, kMaxStackChannels, static_cast<int>(pre_tap_.size())});
  for (int ch = 0; ch < meter_rows; ++ch) {
    pre_meter_channels[static_cast<size_t>(ch)] = pre_tap_[static_cast<size_t>(ch)].data();
  }
  if (pre_meter_) {
    pre_meter_->set_gain_reduction_db(pre_gain_reduction_db);
    pre_meter_->process(pre_meter_channels.data(), meter_rows, clamped_samples);
  }
  last_gain_reduction_db_ = post_gain_reduction_db;
  if (post_meter_) post_meter_->set_gain_reduction_db(post_gain_reduction_db);
  // Drive the post-fader meter over the SAME window length as the pre-fader
  // meter. The pre-fader meter reads pre_tap_, which is only max_block_size_
  // wide, so it can integrate at most clamped_samples. Feeding the post meter
  // the full num_samples when num_samples > max_block_size_ would make the two
  // meters integrate different lengths, so their RMS/LUFS readings would
  // disagree for the same block. Clamp the post meter to match.
  if (post_meter_) post_meter_->process(channels, num_channels, clamped_samples);
  note_member_discards();
}

void ChannelStrip::process_unsegmented(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  const int clamped_samples = std::min(num_samples, max_block_size_);
  const bool muted = effectively_muted();

  if (muted) {
    // Feed silence through so stateful stages decay their tails while muted.
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] != nullptr) {
        std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
      }
    }
  }

  input_trim_.process(channels, num_channels, num_samples);

  const float polarity_l = polarity_left_.load(std::memory_order_relaxed);
  const float polarity_r = polarity_right_.load(std::memory_order_relaxed);
  if (channels[0] != nullptr) {
    for (int i = 0; i < num_samples; ++i) {
      channels[0][i] *= polarity_l;
    }
  }
  if (num_channels > 1 && channels[1] != nullptr) {
    for (int i = 0; i < num_samples; ++i) {
      channels[1][i] *= polarity_r;
    }
  }

  alignment_delay_.process(channels, num_channels, num_samples);

  const bool eq_enabled = eq_enabled_.load(std::memory_order_relaxed);
  const EqPosition eq_at = eq_position_.load(std::memory_order_relaxed);
  float* const* staged = stage_channels(channels, num_channels, num_samples);
  // The EQ re-enters from rest whenever it was off, moved, or skipped for a block.
  const bool eq_pre = eq_pre_gate_.admit(
      staged != nullptr && eq_enabled && eq_at == EqPosition::PreFader, [this] { eq_.reset(); });
  if (staged != nullptr) {
    if (eq_pre) eq_.process(staged, num_channels, num_samples);
    process_insert_chain(pre_inserts_, pre_insert_spo_, staged, num_channels, num_samples, 0, 0);
  }
  // A muted pre-insert tail stays out of the output and meters.
  if (muted) {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] != nullptr) {
        std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
      }
    }
  }
  insert_gain_reduction_count_ = pre_inserts_.size() + post_inserts_.size();
  std::fill_n(insert_gain_reduction_db_.begin(), insert_gain_reduction_count_, 0.0f);
  if (!muted) fold_insert_gain_reduction_db(pre_inserts_, insert_gain_reduction_db_.data());
  const float pre_gain_reduction_db =
      deepest_gain_reduction_db(insert_gain_reduction_db_.data(), pre_inserts_.size());

  // Pre-fader tap (after trim, polarity, delay, EQ-if-pre, and pre inserts) feeds pre-fader aux.
  copy_to_taps(channels, pre_tap_, num_channels, clamped_samples);
  if (pre_meter_) {
    pre_meter_->set_gain_reduction_db(pre_gain_reduction_db);
    pre_meter_->process(channels, num_channels, num_samples);
  }

  fader_.process(channels, num_channels, num_samples);
  // Stereo pan and width are undefined on a surround bed, so wider blocks skip them.
  // The at-rest skip is exact only on a stereo pair; a mono block still takes the pan law.
  if (num_channels == 1 || (num_channels == 2 && !panner_.at_rest_identity())) {
    panner_.process(channels, num_channels, num_samples);
  }
  if (num_channels == 2 && (width_.width() != 1.0f || width_.current_width() != 1.0f)) {
    width_.process(channels, num_channels, num_samples);
  }

  staged = stage_channels(channels, num_channels, num_samples);
  const bool eq_post = eq_post_gate_.admit(
      staged != nullptr && eq_enabled && eq_at == EqPosition::PostFader, [this] { eq_.reset(); });
  if (staged != nullptr) {
    if (eq_post) eq_.process(staged, num_channels, num_samples);
    process_insert_chain(post_inserts_, post_insert_spo_, staged, num_channels, num_samples,
                         pre_inserts_.size(), 0);
  }
  // Post inserts advanced on silence; discard their tail before publishing.
  if (muted) {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] != nullptr) {
        std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
      }
    }
  }
  if (!muted) {
    fold_insert_gain_reduction_db(post_inserts_,
                                  insert_gain_reduction_db_.data() + pre_inserts_.size());
  }
  const float post_gain_reduction_db =
      std::min(pre_gain_reduction_db,
               deepest_gain_reduction_db(insert_gain_reduction_db_.data() + pre_inserts_.size(),
                                         post_inserts_.size()));
  last_gain_reduction_db_ = post_gain_reduction_db;
  if (post_meter_) post_meter_->set_gain_reduction_db(post_gain_reduction_db);

  if (!muted && num_channels >= 2 && channels[0] != nullptr && channels[1] != nullptr) {
    for (int i = 0; i < num_samples; ++i) {
      goniometer_.push(channels[0][i], channels[1][i]);
    }
  }

  // Post-fader tap is the final output, used by post-fader sends and the output meter.
  copy_to_taps(channels, post_tap_, num_channels, clamped_samples);

  if (post_meter_) post_meter_->process(channels, num_channels, num_samples);
}

void ChannelStrip::process_segment(float* const* channels, int num_channels, int start,
                                   int num_samples, int tap_offset) {
  std::array<float*, kMaxStackChannels> segment_channels{};
  for (int ch = 0; ch < num_channels; ++ch) {
    segment_channels[static_cast<size_t>(ch)] =
        channels[ch] == nullptr ? nullptr : channels[ch] + start;
  }
  float* const* segment = segment_channels.data();

  input_trim_.process(segment, num_channels, num_samples);

  const float polarity_l = polarity_left_.load(std::memory_order_relaxed);
  const float polarity_r = polarity_right_.load(std::memory_order_relaxed);
  if (segment[0] != nullptr) {
    for (int i = 0; i < num_samples; ++i) {
      segment[0][i] *= polarity_l;
    }
  }
  if (num_channels > 1 && segment[1] != nullptr) {
    for (int i = 0; i < num_samples; ++i) {
      segment[1][i] *= polarity_r;
    }
  }

  alignment_delay_.process(segment, num_channels, num_samples);

  const bool eq_enabled = eq_enabled_.load(std::memory_order_relaxed);
  const EqPosition eq_at = eq_position_.load(std::memory_order_relaxed);
  float* const* staged = stage_channels(segment, num_channels, num_samples);
  // The EQ re-enters from rest whenever it was off, moved, or skipped for a block.
  const bool eq_pre = eq_pre_gate_.admit(
      staged != nullptr && eq_enabled && eq_at == EqPosition::PreFader, [this] { eq_.reset(); });
  if (staged != nullptr) {
    if (eq_pre) eq_.process(staged, num_channels, num_samples);
    process_insert_chain(pre_inserts_, pre_insert_spo_, staged, num_channels, num_samples, 0,
                         start);
  }
  const bool muted = effectively_muted();
  if (muted) {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (segment[ch]) std::fill(segment[ch], segment[ch] + num_samples, 0.0f);
    }
  }
  copy_to_taps(segment, pre_tap_, num_channels, num_samples, tap_offset);

  fader_.process(segment, num_channels, num_samples);
  // Stereo pan and width are undefined on a surround bed, so wider blocks skip them.
  // The at-rest skip is exact only on a stereo pair; a mono block still takes the pan law.
  if (num_channels == 1 || (num_channels == 2 && !panner_.at_rest_identity())) {
    panner_.process(segment, num_channels, num_samples);
  }
  if (num_channels == 2 && (width_.width() != 1.0f || width_.current_width() != 1.0f)) {
    width_.process(segment, num_channels, num_samples);
  }

  staged = stage_channels(segment, num_channels, num_samples);
  const bool eq_post = eq_post_gate_.admit(
      staged != nullptr && eq_enabled && eq_at == EqPosition::PostFader, [this] { eq_.reset(); });
  if (staged != nullptr) {
    if (eq_post) eq_.process(staged, num_channels, num_samples);
    process_insert_chain(post_inserts_, post_insert_spo_, staged, num_channels, num_samples,
                         pre_inserts_.size(), start);
  }
  if (muted) {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (segment[ch]) std::fill(segment[ch], segment[ch] + num_samples, 0.0f);
    }
  }

  if (!effectively_muted() && num_channels >= 2 && segment[0] != nullptr && segment[1] != nullptr) {
    for (int i = 0; i < num_samples; ++i) {
      goniometer_.push(segment[0][i], segment[1][i]);
    }
  }
  copy_to_taps(segment, post_tap_, num_channels, num_samples, tap_offset);
}

float* const* ChannelStrip::stage_channels(float* const* channels, int num_channels,
                                           int num_samples) noexcept {
  if (std::all_of(channels, channels + num_channels, [](float* row) { return row != nullptr; })) {
    return channels;
  }
  if (num_channels > static_cast<int>(stage_channels_.size()) || num_samples > max_block_size_) {
    return nullptr;
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    const auto row = static_cast<size_t>(ch);
    if (channels[ch] != nullptr) {
      stage_channels_[row] = channels[ch];
      continue;
    }
    std::fill(null_planes_[row].begin(), null_planes_[row].begin() + num_samples, 0.0f);
    stage_channels_[row] = null_planes_[row].data();
  }
  return stage_channels_.data();
}

void ChannelStrip::process_insert_chain(std::vector<std::unique_ptr<rt::ProcessorBase>>& inserts,
                                        const std::vector<uint8_t>& stereo_pair_only,
                                        float* const* channels, int num_channels, int num_samples,
                                        size_t first_insert_index, int sidechain_offset) {
  // A strip insert sees only the front L/R pair, so it forwards at most
  // kPreparedChannels sidechain rows (the surround planes pass through dry).
  std::array<const float*, kPreparedChannels> shifted_sidechain{};
  // Both banks are addressed by the combined insert index while run_insert_chain
  // indexes them by chain-local position, so hand it the segment's base.
  auto segment_bank = [first_insert_index](std::vector<AlignmentDelay>& bank) -> AlignmentDelay* {
    return first_insert_index < bank.size()
               ? bank.data() + static_cast<std::ptrdiff_t>(first_insert_index)
               : nullptr;
  };
  AlignmentDelay* stereo_pair_delays = segment_bank(stereo_pair_alignment_delays_);
  AlignmentDelay* bypass_delays = segment_bank(bypass_alignment_delays_);
  float* const* bypass_scratch =
      bypass_scratch_channels_.empty() ? nullptr : bypass_scratch_channels_.data();
  run_insert_chain(inserts, stereo_pair_only, insert_sidechains_, channels, num_channels,
                   num_samples, first_insert_index, sidechain_offset, shifted_sidechain.data(),
                   kPreparedChannels, /*detector_excluded_channel=*/-1, stereo_pair_delays,
                   bypass_delays, bypass_scratch, static_cast<int>(bypass_scratch_channels_.size()),
                   max_block_size_);
}

void ChannelStrip::prepare_insert_alignment_delays() {
  const size_t total = pre_inserts_.size() + post_inserts_.size();
  // Capacity is reserved in the constructor and add_pre/post_insert enforce the
  // cap, so neither resize can reallocate under the audio thread.
  stereo_pair_alignment_delays_.resize(total);
  bypass_alignment_delays_.resize(total);

  for (size_t index = 0; index < total; ++index) {
    const bool is_pre = index < pre_inserts_.size();
    const size_t local = is_pre ? index : index - pre_inserts_.size();
    const rt::ProcessorBase* insert =
        is_pre ? pre_inserts_[local].get() : post_inserts_[local].get();
    const std::vector<uint8_t>& spo_flags = is_pre ? pre_insert_spo_ : post_insert_spo_;
    if (insert == nullptr) {
      continue;
    }
    const bool stereo_pair_only = local < spo_flags.size() && spo_flags[local] != 0;
    const int latency_q8 = insert->latency_samples_q8();

    // The front pair is delayed by the insert itself, so this bank only ever
    // covers the planes behind it.
    configure_insert_alignment_delay(stereo_pair_alignment_delays_[index], prepared_channels_ - 2,
                                     stereo_pair_only ? latency_q8 : 0);
    // The bypass substitute stands in for the whole insert, so it must cover
    // every plane the strip can hand it.
    configure_insert_alignment_delay(bypass_alignment_delays_[index], prepared_channels_,
                                     latency_q8);
  }
}

void ChannelStrip::reset() {
  reset_processing();
  if (pre_meter_) pre_meter_->reset();
  if (post_meter_) post_meter_->reset();
  fader_automation_.clear();
  pan_automation_.clear();
  width_automation_.clear();
  for (auto& lane : insert_automation_) {
    if (lane.lane) lane.lane->clear();
  }
  for (auto& lane : send_automation_) {
    if (lane) lane->clear();
  }
  goniometer_.reset();
}

void ChannelStrip::settle() noexcept {
  // Snap the gain stages to their steady-state targets so the first rendered
  // block does not ramp in from the previous gain. Unlike reset(), this does
  // not clear automation, meters, or insert state -- it only quiesces the
  // gain smoothers ahead of a deterministic offline render.
  input_trim_.settle();
  fader_.settle();
  // Seed the width smoother from its target as well; set_width() only stores the
  // target, so a non-default width would otherwise glide from 1.0 over the first
  // rendered block instead of opening settled.
  width_.reset();
  // Snap the pan smoothers to their steady-state gains too, so a non-default
  // static pan opens settled instead of gliding from center over the first block.
  panner_.reset();
}

int ChannelStrip::latency_samples() const noexcept { return latency_samples_q8() >> 8; }

int ChannelStrip::latency_samples_q8() const noexcept { return post_fader_latency_samples_q8(); }

int ChannelStrip::tail_samples() const noexcept {
  rt::TailBudget tail = rt::TailBudget::reported(pre_fader_tail_samples());
  if (eq_enabled() && eq_position() == EqPosition::PostFader) {
    tail.then(rt::TailBudget::reported(eq_.tail_samples()));
  }
  return tail.then(processor_chain_tail(post_inserts_)).samples();
}

int ChannelStrip::pre_fader_tail_samples() const noexcept {
  // The channel delay is not latency, so the audio it holds back is owed as tail.
  rt::TailBudget tail;
  tail.delay(alignment_delay_.delay_samples());
  if (eq_enabled() && eq_position() == EqPosition::PreFader) {
    tail.then(rt::TailBudget::reported(eq_.tail_samples()));
  }
  return tail.then(processor_chain_tail(pre_inserts_)).samples();
}

int ChannelStrip::pre_fader_latency_samples_q8() const noexcept {
  // The channel delay moves this strip relative to the others; PDC must not undo it.
  return total_latency_q8(pre_inserts_);
}

int ChannelStrip::post_fader_latency_samples_q8() const noexcept {
  return pre_fader_latency_samples_q8() + total_latency_q8(post_inserts_);
}

std::optional<int> ChannelStrip::insert_input_latency_samples_q8(
    unsigned int insert_index) const noexcept {
  const size_t index = insert_index;
  const size_t pre_count = pre_inserts_.size();
  const size_t total = pre_count + post_inserts_.size();
  if (index >= total) return std::nullopt;

  int64_t prefix = 0;
  for (size_t prior = 0; prior < index; ++prior) {
    const auto& chain = prior < pre_count ? pre_inserts_ : post_inserts_;
    const size_t local = prior < pre_count ? prior : prior - pre_count;
    if (local < chain.size() && chain[local] != nullptr) {
      prefix += static_cast<int64_t>(chain[local]->latency_samples_q8());
      if (prefix < std::numeric_limits<int>::min() || prefix > std::numeric_limits<int>::max()) {
        return std::nullopt;
      }
    }
  }
  return static_cast<int>(prefix);
}

void ChannelStrip::set_polarity_invert(bool left, bool right) noexcept {
  polarity_left_.store(left ? -1.0f : 1.0f, std::memory_order_relaxed);
  polarity_right_.store(right ? -1.0f : 1.0f, std::memory_order_relaxed);
}

bool ChannelStrip::polarity_invert_left() const noexcept {
  return polarity_left_.load(std::memory_order_relaxed) < 0.0f;
}

bool ChannelStrip::polarity_invert_right() const noexcept {
  return polarity_right_.load(std::memory_order_relaxed) < 0.0f;
}

void ChannelStrip::set_channel_delay_samples(int delay_samples) {
  alignment_delay_.set_delay_samples(delay_samples);
}

bool ChannelStrip::try_set_channel_delay_samples(int delay_samples) noexcept {
  const int bounded = std::clamp(delay_samples, 0, kMaxAlignmentDelaySamples);
  return alignment_delay_.try_set_delay_samples_q8(bounded << 8, FractionalDelayMode::None);
}

void ChannelStrip::set_prepared_channels(int num_channels) {
  const int next = std::max(1, num_channels);
  if (next == prepared_channels_) return;
  prepared_channels_ = next;
  // Resize the banks that already exist as well as the ones a later prepare()
  // will build: a host that widens an already-prepared strip must not have to
  // remember the ordering.
  alignment_delay_.set_prepared_channels(prepared_channels_);
  prepare_insert_alignment_delays();
  // The taps and the send scratch follow the declared layout too; otherwise a
  // widened strip would meter and send only the planes the narrower prepare()
  // happened to allocate.
  const auto tap_rows = static_cast<size_t>(std::max(kPreparedChannels, prepared_channels_));
  for (auto* taps : {&pre_tap_, &post_tap_, &send_temp_}) {
    if (!taps->empty() && taps->size() != tap_rows) {
      taps->resize(tap_rows, std::vector<float>((*taps)[0].size(), 0.0f));
    }
  }
  if (!null_planes_.empty()) {
    null_planes_.resize(tap_rows, std::vector<float>(null_planes_[0].size(), 0.0f));
    stage_channels_.resize(tap_rows, nullptr);
  }
  if (!send_temp_.empty()) {
    send_temp_channels_.resize(send_temp_.size());
    for (size_t row = 0; row < send_temp_.size(); ++row) {
      send_temp_channels_[row] = send_temp_[row].data();
    }
  }
  if (!bypass_scratch_.empty() && bypass_scratch_.size() != tap_rows) {
    bypass_scratch_.resize(tap_rows, std::vector<float>(bypass_scratch_[0].size(), 0.0f));
  }
  if (!bypass_scratch_.empty()) {
    bypass_scratch_channels_.resize(bypass_scratch_.size());
    for (size_t row = 0; row < bypass_scratch_.size(); ++row) {
      bypass_scratch_channels_[row] = bypass_scratch_[row].data();
    }
  }
}

int ChannelStrip::alignment_channel_overflow() const noexcept {
  int widest = alignment_delay_.channel_overflow_high_water();
  for (const AlignmentDelay& delay : stereo_pair_alignment_delays_) {
    widest = std::max(widest, delay.channel_overflow_high_water());
  }
  for (const AlignmentDelay& delay : bypass_alignment_delays_) {
    widest = std::max(widest, delay.channel_overflow_high_water());
  }
  return widest;
}

MeterSnapshot ChannelStrip::meter_snapshot(TapPoint tap) const noexcept {
  const std::optional<MeterProcessor>& meter = tap == TapPoint::PreFader ? pre_meter_ : post_meter_;
  return meter ? meter->snapshot() : MeterSnapshot{};
}

size_t ChannelStrip::read_goniometer_latest(GoniometerPoint* dest,
                                            size_t max_points) const noexcept {
  return goniometer_.read_latest(dest, max_points);
}

void ChannelStrip::add_pre_insert(std::unique_ptr<rt::ProcessorBase> processor,
                                  bool stereo_pair_only) {
  if (!processor) {
    throw SonareException(ErrorCode::InvalidParameter, "insert processor must not be null");
  }
  if (pre_inserts_.size() + post_inserts_.size() >= kMaxInserts) {
    throw SonareException(ErrorCode::InvalidState, "ChannelStrip insert cap exceeded");
  }
  if (max_block_size_ > 0) {
    processor->prepare(sample_rate_, max_block_size_);
  }
  pre_inserts_.push_back(std::move(processor));
  pre_insert_spo_.push_back(stereo_pair_only ? 1u : 0u);
  insert_sidechains_.resize(pre_inserts_.size() + post_inserts_.size());
  prepare_insert_alignment_delays();
}

void ChannelStrip::add_post_insert(std::unique_ptr<rt::ProcessorBase> processor,
                                   bool stereo_pair_only) {
  if (!processor) {
    throw SonareException(ErrorCode::InvalidParameter, "insert processor must not be null");
  }
  if (pre_inserts_.size() + post_inserts_.size() >= kMaxInserts) {
    throw SonareException(ErrorCode::InvalidState, "ChannelStrip insert cap exceeded");
  }
  if (max_block_size_ > 0) {
    processor->prepare(sample_rate_, max_block_size_);
  }
  post_inserts_.push_back(std::move(processor));
  post_insert_spo_.push_back(stereo_pair_only ? 1u : 0u);
  insert_sidechains_.resize(pre_inserts_.size() + post_inserts_.size());
  prepare_insert_alignment_delays();
}

void ChannelStrip::set_insert_sidechain(unsigned int insert_index, const float* const* channels,
                                        int num_channels, int num_samples) {
  const size_t index = insert_index;
  // insert_sidechains_ is sized by add_pre_insert / add_post_insert (control
  // thread). Never resize here: the audio thread iterates insert_sidechains_ in
  // process_insert_chain(), and a resize could reallocate or grow it under the
  // reader. An index past the current sidechain count is treated as a no-op.
  if (index >= insert_sidechains_.size()) {
    return;
  }
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    insert_sidechains_[index] = {{}, 0, 0, true};
    return;
  }
  const int n = std::min(num_channels, kMaxStackChannels);
  InsertSidechain entry;
  entry.channels = {};
  for (int ch = 0; ch < n; ++ch) {
    entry.channels[static_cast<size_t>(ch)] = channels[ch];
  }
  entry.num_channels = n;
  entry.num_samples = num_samples;
  entry.managed = true;
  insert_sidechains_[index] = entry;
}

void ChannelStrip::clear_insert_sidechains() noexcept {
  // A slot that was ever keyed stays managed, so its processor drops the
  // previous block's borrowed key instead of reading it again.
  for (auto& sidechain : insert_sidechains_) {
    sidechain = {{}, 0, 0, sidechain.managed};
  }
}

size_t ChannelStrip::add_send(const SendConfig& cfg) {
  if (sends_.size() >= kMaxSends) {
    throw SonareException(ErrorCode::InvalidState, "ChannelStrip send cap exceeded");
  }
  sends_.push_back(std::make_unique<SendProcessor>(cfg));
  send_automation_.push_back(std::make_unique<AutomationLane>());
  if (max_block_size_ > 0) {
    sends_.back()->prepare(sample_rate_, max_block_size_);
  }
  return sends_.size() - 1;
}

void ChannelStrip::clear_sends() {
  sends_.clear();
  send_automation_.clear();
}

bool ChannelStrip::prepare_sends(const SendConfig* configs, size_t count,
                                 PreparedSends& out) const noexcept {
  if (count > kMaxSends || (count != 0 && configs == nullptr)) {
    return false;
  }
  PreparedSends next;
  try {
    next.sends.reserve(count);
    next.automation.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      next.sends.push_back(std::make_unique<SendProcessor>(configs[index]));
      if (max_block_size_ > 0) {
        next.sends.back()->prepare(sample_rate_, max_block_size_);
      }
      next.automation.push_back(std::make_unique<AutomationLane>());
    }
  } catch (...) {
    return false;
  }
  out = std::move(next);
  return true;
}

void ChannelStrip::commit_sends(PreparedSends& prepared) noexcept {
  sends_.swap(prepared.sends);
  send_automation_.swap(prepared.automation);
}

void ChannelStrip::remove_send(size_t index) {
  if (index >= sends_.size()) {
    return;
  }
  // sends_ and send_automation_ are kept index-parallel by add_send, so erase
  // the same slot from both. Erasing shifts higher sends down by one index,
  // which the caller (the C-ABI remove_send) mirrors into the scene strip and
  // forces a graph recompile to re-derive the send node port layout.
  sends_.erase(sends_.begin() + static_cast<std::ptrdiff_t>(index));
  if (index < send_automation_.size()) {
    send_automation_.erase(send_automation_.begin() + static_cast<std::ptrdiff_t>(index));
  }
}

void ChannelStrip::set_send_db(size_t index, float db) {
  if (index >= sends_.size()) {
    return;
  }
  sends_[index]->set_send_db(db);
}

float ChannelStrip::send_db(size_t index) const noexcept {
  return index < sends_.size() ? sends_[index]->send_db() : 0.0f;
}

bool ChannelStrip::schedule_send_automation(size_t index, int64_t sample_pos, float db,
                                            AutomationCurveType curve) noexcept {
  return schedule_send_automation_result(index, sample_pos, db, curve) ==
         AutomationPushResult::Success;
}

AutomationPushResult ChannelStrip::schedule_send_automation_result(
    size_t index, int64_t sample_pos, float db, AutomationCurveType curve) noexcept {
  if (index >= send_automation_.size() || !std::isfinite(db)) {
    return AutomationPushResult::NonMonotonic;
  }
  if (!send_automation_[index]) {
    return AutomationPushResult::NonMonotonic;
  }
  AutomationEvent event;
  event.sample_pos = sample_pos;
  event.value = db;
  event.curve = curve;
  // The lane is the send's identity. A slot index here would go stale when an
  // earlier send is removed, and points straddling the removal would stop
  // interpolating because their targets would differ.
  event.target.kind = AutomationTargetKind::Send;
  return send_automation_[index]->try_push(event);
}

SendTiming ChannelStrip::send_timing(size_t index) const {
  if (index >= sends_.size()) {
    return SendTiming::PostFader;
  }
  return sends_[index]->timing();
}

int ChannelStrip::send_latency_samples_q8(size_t index) const noexcept {
  return send_timing(index) == SendTiming::PreFader ? pre_fader_latency_samples_q8()
                                                    : post_fader_latency_samples_q8();
}

int ChannelStrip::send_tail_samples(size_t index) const noexcept {
  return send_timing(index) == SendTiming::PreFader ? pre_fader_tail_samples() : tail_samples();
}

void ChannelStrip::mix_send(size_t index, float* const* dest, int num_channels, int num_samples) {
  mix_send_at(index, dest, num_channels, num_samples, 0);
}

void ChannelStrip::mix_send_at(size_t index, float* const* dest, int num_channels, int num_samples,
                               int64_t block_start) {
  if (index >= sends_.size() || dest == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  const auto& tap = (sends_[index]->timing() == SendTiming::PreFader) ? pre_tap_ : post_tap_;

  const int rows = std::min<int>(
      {num_channels, static_cast<int>(tap.size()), static_cast<int>(send_temp_.size())});
  const int n = std::min(num_samples, max_block_size_);

  for (int ch = 0; ch < rows; ++ch) {
    std::copy(tap[ch].begin(), tap[ch].begin() + n, send_temp_[ch].begin());
  }
  apply_send_from_temp(index, dest, rows, n, block_start);
}

void ChannelStrip::mix_send_from_at(size_t index, const float* const* source, float* const* dest,
                                    int num_channels, int num_samples, int64_t block_start) {
  if (index >= sends_.size() || source == nullptr || dest == nullptr || num_channels <= 0 ||
      num_samples <= 0) {
    return;
  }

  const int rows = std::min<int>(num_channels, static_cast<int>(send_temp_.size()));
  const int n = std::min(num_samples, max_block_size_);

  for (int ch = 0; ch < rows; ++ch) {
    if (source[ch] == nullptr) {
      std::fill(send_temp_[ch].begin(), send_temp_[ch].begin() + n, 0.0f);
      continue;
    }
    std::copy(source[ch], source[ch] + n, send_temp_[ch].begin());
  }
  apply_send_from_temp(index, dest, rows, n, block_start);
}

int ChannelStrip::copy_pre_fader_tap(float* const* dest, int num_channels,
                                     int num_samples) const noexcept {
  if (dest == nullptr || num_channels <= 0 || num_samples <= 0) return 0;
  const int rows = std::min<int>(num_channels, static_cast<int>(pre_tap_.size()));
  const int n = std::min(num_samples, max_block_size_);
  int written = 0;
  for (int ch = 0; ch < rows; ++ch) {
    if (dest[ch] == nullptr) break;
    std::copy(pre_tap_[static_cast<size_t>(ch)].begin(),
              pre_tap_[static_cast<size_t>(ch)].begin() + n, dest[ch]);
    // The tap is only as long as the prepared block, so a caller asking for more
    // gets silence rather than whatever its own scratch was holding.
    std::fill(dest[ch] + n, dest[ch] + num_samples, 0.0f);
    ++written;
  }
  return written;
}

void ChannelStrip::apply_send_from_temp(size_t index, float* const* dest, int rows, int n,
                                        int64_t block_start) {
  SendProcessor& send = *sends_[index];

  // send_temp_channels_ is sized in prepare() / set_prepared_channels(), so no allocation here.
  for (int ch = 0; ch < rows; ++ch) {
    send_temp_channels_[static_cast<size_t>(ch)] = send_temp_[static_cast<size_t>(ch)].data();
  }

  AutomationLane* lane = index < send_automation_.size() ? send_automation_[index].get() : nullptr;
  AutomationStaging send_events;
  // Same chunking as process_at(): a dense lane is staged in ranges it fits.
  const auto stage_chunk = [&](int chunk_start) {
    int chunk = n - chunk_start;
    while (chunk > 1 && lane->count_block_offsets(
                            numeric::saturating_add(block_start, static_cast<int64_t>(chunk_start)),
                            chunk) > kMaxAutomationEventsPerBlock) {
      chunk = (chunk + 1) / 2;
    }
    send_events.clear();
    stage_events(*lane, block_start, chunk_start, chunk, send_events);
    return chunk_start + chunk;
  };
  int chunk_end = lane != nullptr ? stage_chunk(0) : n;

  if (chunk_end >= n && send_events.empty()) {
    // Applies the smoothed send gain in place on the copied tap, leaving dest untouched.
    send.process(send_temp_channels_.data(), rows, n);
  } else {
    int cursor = 0;
    for (;;) {
      size_t send_event_index = 0;
      while (cursor < chunk_end) {
        while (send_event_index < send_events.size() &&
               send_events[send_event_index].offset == cursor) {
          send.set_send_db(send_events[send_event_index++].event.value);
        }
        const int next_offset = offset_or(send_events, send_event_index, chunk_end);
        const int segment_samples = std::max(0, next_offset - cursor);
        if (segment_samples > 0) {
          for (int ch = 0; ch < rows; ++ch) {
            send_temp_channels_[static_cast<size_t>(ch)] =
                send_temp_[static_cast<size_t>(ch)].data() + cursor;
          }
          send.process(send_temp_channels_.data(), rows, segment_samples);
          cursor += segment_samples;
        } else {
          ++cursor;
        }
      }
      if (chunk_end >= n) break;
      chunk_end = stage_chunk(chunk_end);
    }
  }
  automation_staging_refusals_.add(static_cast<uint32_t>(send_events.refused()));

  for (int ch = 0; ch < rows; ++ch) {
    if (dest[ch] == nullptr) {
      continue;
    }
    for (int i = 0; i < n; ++i) {
      dest[ch][i] += send_temp_[ch][i];
    }
  }
}

void ChannelStrip::set_muted(bool muted) noexcept {
  muted_.store(muted, std::memory_order_relaxed);
}

bool ChannelStrip::muted() const noexcept { return muted_.load(std::memory_order_relaxed); }

bool ChannelStrip::effectively_muted() const noexcept {
  return muted() || (implied_mute() && !solo_safe());
}

void ChannelStrip::set_soloed(bool soloed) noexcept {
  soloed_.store(soloed, std::memory_order_relaxed);
}

bool ChannelStrip::soloed() const noexcept { return soloed_.load(std::memory_order_relaxed); }

void ChannelStrip::set_solo_safe(bool solo_safe) noexcept {
  solo_safe_.store(solo_safe, std::memory_order_relaxed);
}

bool ChannelStrip::solo_safe() const noexcept { return solo_safe_.load(std::memory_order_relaxed); }

void ChannelStrip::set_implied_mute(bool implied_mute) noexcept {
  implied_mute_.store(implied_mute, std::memory_order_relaxed);
}

bool ChannelStrip::implied_mute() const noexcept {
  return implied_mute_.load(std::memory_order_relaxed);
}

void ChannelStrip::reset_processing() noexcept {
  last_gain_reduction_db_ = 0.0f;
  std::fill_n(insert_gain_reduction_db_.begin(), insert_gain_reduction_count_, 0.0f);
  // Gain, pan and width processors reset by snapping their smoothers to target.
  input_trim_.reset();
  alignment_delay_.reset();
  fader_.reset();
  panner_.reset();
  width_.reset();
  eq_.reset();
  for (auto& insert : pre_inserts_) {
    insert->reset();
  }
  for (auto& insert : post_inserts_) {
    insert->reset();
  }
  for (auto& delay : stereo_pair_alignment_delays_) {
    delay.reset();
  }
  for (auto& delay : bypass_alignment_delays_) {
    delay.reset();
  }
  for (auto& send : sends_) {
    send->reset();
  }
  for (auto* taps : {&pre_tap_, &post_tap_, &send_temp_}) {
    zero_taps(*taps, static_cast<int>(taps->size()), max_block_size_);
  }
}

}  // namespace sonare::mixing
