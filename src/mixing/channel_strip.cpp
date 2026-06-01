#include "mixing/channel_strip.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

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
  const int rows = std::min<int>(num_channels, static_cast<int>(taps.size()));
  for (int ch = 0; ch < rows; ++ch) {
    if (channels[ch] == nullptr) {
      continue;
    }
    const int begin = std::min<int>(tap_offset, static_cast<int>(taps[ch].size()));
    const int end = std::min<int>(tap_offset + num_samples, static_cast<int>(taps[ch].size()));
    if (begin < end) {
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

float aggregate_gain_reduction_db(
    const std::vector<std::unique_ptr<rt::ProcessorBase>>& inserts) noexcept {
  float reduction_db = 0.0f;
  for (const auto& insert : inserts) {
    reduction_db = std::min(reduction_db, insert->last_gain_reduction_db());
  }
  return reduction_db;
}

template <typename Lane, size_t Capacity>
size_t consume_events(Lane& lane, int64_t block_start, int num_samples,
                      std::array<AutomationBlockEvent, Capacity>& dest) {
  size_t count = 0;
  lane.consume_block(block_start, num_samples, [&](const AutomationBlockEvent& event) {
    if (count < dest.size()) {
      dest[count++] = event;
    }
  });
  return count;
}

template <size_t Capacity>
void sort_events_by_offset(std::array<AutomationBlockEvent, Capacity>& events, size_t count) {
  std::sort(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(count),
            [](const AutomationBlockEvent& lhs, const AutomationBlockEvent& rhs) {
              if (lhs.offset != rhs.offset) return lhs.offset < rhs.offset;
              return static_cast<int>(lhs.event.target.kind) <
                     static_cast<int>(rhs.event.target.kind);
            });
}

template <size_t Capacity>
int next_event_offset(const std::array<AutomationBlockEvent, Capacity>& events, size_t count,
                      size_t index, int fallback) {
  return index < count ? events[index].offset : fallback;
}

}  // namespace

ChannelStrip::ChannelStrip(ChannelStripConfig config)
    : input_trim_({config.input_trim_db, config.smoothing_ms}),
      fader_({config.fader_db, config.smoothing_ms}),
      panner_({config.pan, config.pan_law, config.smoothing_ms}),
      width_(1.0f, config.smoothing_ms),
      eq_position_(config.eq_position) {
  // Pre-reserve the vectors that the audio thread iterates while the control
  // thread may concurrently push_back into. The audio thread iterates
  // insert_automation_ in process_at() and insert_sidechains_ in
  // process_insert_chain(); a reallocation here would invalidate those
  // iterators / pointers (C++ UB). Caps are enforced in schedule_insert_
  // automation() and add_pre/post_insert(); see channel_strip.h.
  insert_automation_.reserve(kMaxInsertAutomationLanes);
  insert_sidechains_.reserve(kMaxInserts);
  pre_inserts_.reserve(kMaxInserts);
  post_inserts_.reserve(kMaxInserts);
}

void ChannelStrip::prepare(double sample_rate, int max_block_size) {
  sample_rate_ = sample_rate;
  max_block_size_ = std::max(0, max_block_size);

  input_trim_.prepare(sample_rate, max_block_size);
  fader_.prepare(sample_rate, max_block_size);
  panner_.prepare(sample_rate, max_block_size);
  width_.prepare(sample_rate, max_block_size);
  alignment_delay_.prepare(sample_rate, max_block_size);
  eq_.prepare(sample_rate, max_block_size);
  pre_meter_.prepare(sample_rate, max_block_size);
  post_meter_.prepare(sample_rate, max_block_size);
  for (auto& insert : pre_inserts_) {
    insert->prepare(sample_rate_, max_block_size_);
  }
  for (auto& insert : post_inserts_) {
    insert->prepare(sample_rate_, max_block_size_);
  }
  for (auto& send : sends_) {
    send->prepare(sample_rate, max_block_size);
  }

  const auto rows = static_cast<size_t>(kPreparedChannels);
  const auto cols = static_cast<size_t>(max_block_size_);
  pre_tap_.assign(rows, std::vector<float>(cols, 0.0f));
  post_tap_.assign(rows, std::vector<float>(cols, 0.0f));
  send_temp_.assign(rows, std::vector<float>(cols, 0.0f));

  // ParametricEq allocates its per-channel filter state lazily on the first process() with a
  // given channel count. Warm it up here (off the audio thread) so process() stays RT-safe.
  if (max_block_size_ > 0) {
    float* warm[kPreparedChannels];
    for (int ch = 0; ch < kPreparedChannels; ++ch) {
      warm[ch] = post_tap_[static_cast<size_t>(ch)].data();
    }
    eq_.process(warm, kPreparedChannels, max_block_size_);
    eq_.reset();
    zero_taps(post_tap_, kPreparedChannels, max_block_size_);
  }
}

void ChannelStrip::process(float* const* channels, int num_channels, int num_samples) {
  process_at(channels, num_channels, num_samples, 0);
}

void ChannelStrip::process_at(float* const* channels, int num_channels, int num_samples,
                              int64_t block_start) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
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
  if (num_channels > kMaxStackChannels) {
    process_unsegmented(channels, num_channels, num_samples);
    return;
  }

  std::array<AutomationBlockEvent, kMaxAutomationEventsPerBlock> fader_events{};
  std::array<AutomationBlockEvent, kMaxAutomationEventsPerBlock> pan_events{};
  std::array<AutomationBlockEvent, kMaxAutomationEventsPerBlock> width_events{};
  std::array<AutomationBlockEvent, kMaxAutomationEventsPerBlock> insert_events{};
  const size_t fader_count =
      consume_events(fader_automation_, block_start, num_samples, fader_events);
  const size_t pan_count = consume_events(pan_automation_, block_start, num_samples, pan_events);
  const size_t width_count =
      consume_events(width_automation_, block_start, num_samples, width_events);
  size_t insert_count = 0;
  for (auto& lane : insert_automation_) {
    if (!lane.lane) continue;
    lane.lane->consume_block(block_start, num_samples, [&](const AutomationBlockEvent& event) {
      if (insert_count < insert_events.size()) {
        insert_events[insert_count++] = event;
      }
    });
  }
  sort_events_by_offset(insert_events, insert_count);

  if (fader_count == 0 && pan_count == 0 && width_count == 0 && insert_count == 0) {
    process_unsegmented(channels, num_channels, num_samples);
    return;
  }

  if (effectively_muted()) {
    // The events were already drained from the SPSC lanes above; discarding them
    // would leave parameter state stale on unmute. Apply them (advancing fader /
    // pan / width / insert parameters to their block-final values) before the
    // muted passthrough so the strip resumes with correct parameters.
    for (size_t i = 0; i < fader_count; ++i) apply_automation_event(fader_events[i].event);
    for (size_t i = 0; i < pan_count; ++i) apply_automation_event(pan_events[i].event);
    for (size_t i = 0; i < width_count; ++i) apply_automation_event(width_events[i].event);
    for (size_t i = 0; i < insert_count; ++i) apply_automation_event(insert_events[i].event);
    process_unsegmented(channels, num_channels, num_samples);
    return;
  }

  const int clamped_samples = std::min(num_samples, max_block_size_);
  zero_taps(pre_tap_, num_channels, 0, clamped_samples);
  zero_taps(post_tap_, num_channels, 0, clamped_samples);

  size_t fader_index = 0;
  size_t pan_index = 0;
  size_t width_index = 0;
  size_t insert_index = 0;
  int cursor = 0;
  while (cursor < num_samples) {
    while (fader_index < fader_count && fader_events[fader_index].offset == cursor) {
      apply_automation_event(fader_events[fader_index++].event);
    }
    while (pan_index < pan_count && pan_events[pan_index].offset == cursor) {
      apply_automation_event(pan_events[pan_index++].event);
    }
    while (width_index < width_count && width_events[width_index].offset == cursor) {
      apply_automation_event(width_events[width_index++].event);
    }
    while (insert_index < insert_count && insert_events[insert_index].offset == cursor) {
      apply_automation_event(insert_events[insert_index++].event);
    }

    const int next_offset = std::min(
        {num_samples, next_event_offset(fader_events, fader_count, fader_index, num_samples),
         next_event_offset(pan_events, pan_count, pan_index, num_samples),
         next_event_offset(width_events, width_count, width_index, num_samples),
         next_event_offset(insert_events, insert_count, insert_index, num_samples)});
    const int segment_samples = std::max(0, next_offset - cursor);
    if (segment_samples > 0) {
      process_segment(channels, num_channels, cursor, segment_samples, cursor);
      cursor += segment_samples;
    } else {
      // Defensive guard for duplicate or unsorted offsets; consume matching events next loop.
      ++cursor;
    }
  }

  const float pre_gain_reduction_db = aggregate_gain_reduction_db(pre_inserts_);
  const float post_gain_reduction_db =
      std::min(pre_gain_reduction_db, aggregate_gain_reduction_db(post_inserts_));

  float* pre_meter_channels[kPreparedChannels]{};
  const int meter_rows = std::min<int>(num_channels, kPreparedChannels);
  for (int ch = 0; ch < meter_rows; ++ch) {
    pre_meter_channels[ch] = pre_tap_[static_cast<size_t>(ch)].data();
  }
  pre_meter_.set_gain_reduction_db(pre_gain_reduction_db);
  pre_meter_.process(pre_meter_channels, meter_rows, clamped_samples);
  post_meter_.set_gain_reduction_db(post_gain_reduction_db);
  post_meter_.process(channels, num_channels, num_samples);
}

void ChannelStrip::process_unsegmented(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  const int clamped_samples = std::min(num_samples, max_block_size_);

  if (effectively_muted()) {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] != nullptr) {
        std::fill(channels[ch], channels[ch] + num_samples, 0.0f);
      }
    }
    zero_taps(pre_tap_, num_channels, clamped_samples);
    zero_taps(post_tap_, num_channels, clamped_samples);
    pre_meter_.set_gain_reduction_db(0.0f);
    post_meter_.set_gain_reduction_db(0.0f);
    pre_meter_.process(channels, num_channels, num_samples);
    post_meter_.process(channels, num_channels, num_samples);
    return;
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

  if (eq_position_.load(std::memory_order_relaxed) == EqPosition::PreFader) {
    eq_.process(channels, num_channels, num_samples);
  }
  process_insert_chain(pre_inserts_, channels, num_channels, num_samples, 0, 0);
  const float pre_gain_reduction_db = aggregate_gain_reduction_db(pre_inserts_);

  // Pre-fader tap (after trim, polarity, delay, EQ-if-pre, and pre inserts) feeds pre-fader aux.
  copy_to_taps(channels, pre_tap_, num_channels, clamped_samples);
  pre_meter_.set_gain_reduction_db(pre_gain_reduction_db);
  pre_meter_.process(channels, num_channels, num_samples);

  fader_.process(channels, num_channels, num_samples);
  panner_.process(channels, num_channels, num_samples);

  if (eq_position_.load(std::memory_order_relaxed) == EqPosition::PostFader) {
    eq_.process(channels, num_channels, num_samples);
  }
  process_insert_chain(post_inserts_, channels, num_channels, num_samples, pre_inserts_.size(), 0);
  const float post_gain_reduction_db =
      std::min(pre_gain_reduction_db, aggregate_gain_reduction_db(post_inserts_));
  post_meter_.set_gain_reduction_db(post_gain_reduction_db);
  width_.process(channels, num_channels, num_samples);

  if (num_channels >= 2 && channels[0] != nullptr && channels[1] != nullptr) {
    for (int i = 0; i < num_samples; ++i) {
      goniometer_.push(channels[0][i], channels[1][i]);
    }
  }

  // Post-fader tap is the final output, used by post-fader sends and the output meter.
  copy_to_taps(channels, post_tap_, num_channels, clamped_samples);

  post_meter_.process(channels, num_channels, num_samples);
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

  if (eq_position_.load(std::memory_order_relaxed) == EqPosition::PreFader) {
    eq_.process(segment, num_channels, num_samples);
  }
  process_insert_chain(pre_inserts_, segment, num_channels, num_samples, 0, start);
  copy_to_taps(segment, pre_tap_, num_channels, num_samples, tap_offset);

  fader_.process(segment, num_channels, num_samples);
  panner_.process(segment, num_channels, num_samples);

  if (eq_position_.load(std::memory_order_relaxed) == EqPosition::PostFader) {
    eq_.process(segment, num_channels, num_samples);
  }
  process_insert_chain(post_inserts_, segment, num_channels, num_samples, pre_inserts_.size(),
                       start);
  width_.process(segment, num_channels, num_samples);

  if (num_channels >= 2 && segment[0] != nullptr && segment[1] != nullptr) {
    for (int i = 0; i < num_samples; ++i) {
      goniometer_.push(segment[0][i], segment[1][i]);
    }
  }
  copy_to_taps(segment, post_tap_, num_channels, num_samples, tap_offset);
}

void ChannelStrip::process_insert_chain(std::vector<std::unique_ptr<rt::ProcessorBase>>& inserts,
                                        float* const* channels, int num_channels, int num_samples,
                                        size_t first_insert_index, int sidechain_offset) {
  std::array<const float*, kPreparedChannels> shifted_sidechain{};
  for (size_t local = 0; local < inserts.size(); ++local) {
    const size_t index = first_insert_index + local;
    const InsertSidechain* key =
        index < insert_sidechains_.size() ? &insert_sidechains_[index] : nullptr;
    if (key != nullptr && key->num_channels > 0 && key->num_samples > sidechain_offset) {
      const int rows = std::min<int>(key->num_channels, kPreparedChannels);
      const int remaining = key->num_samples - sidechain_offset;
      for (int ch = 0; ch < rows; ++ch) {
        shifted_sidechain[static_cast<size_t>(ch)] =
            key->channels[ch] == nullptr ? nullptr : key->channels[ch] + sidechain_offset;
      }
      inserts[local]->set_sidechain(shifted_sidechain.data(), rows,
                                    std::min(num_samples, remaining));
    } else if (key != nullptr && key->managed) {
      inserts[local]->clear_sidechain();
    } else {
      // Leave directly configured processor sidechains intact. Graph-managed
      // keys are marked through set_insert_sidechain().
    }
    inserts[local]->process(channels, num_channels, num_samples);
  }
}

void ChannelStrip::reset() {
  input_trim_.reset();
  alignment_delay_.reset();
  fader_.reset();
  panner_.reset();
  width_.reset();
  eq_.reset();
  pre_meter_.reset();
  post_meter_.reset();
  for (auto& insert : pre_inserts_) {
    insert->reset();
  }
  for (auto& insert : post_inserts_) {
    insert->reset();
  }
  for (auto& send : sends_) {
    send->reset();
  }
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
  zero_taps(pre_tap_, kPreparedChannels, max_block_size_);
  zero_taps(post_tap_, kPreparedChannels, max_block_size_);
  zero_taps(send_temp_, kPreparedChannels, max_block_size_);
}

int ChannelStrip::latency_samples() const noexcept { return latency_samples_q8() >> 8; }

int ChannelStrip::latency_samples_q8() const noexcept { return post_fader_latency_samples_q8(); }

int ChannelStrip::pre_fader_latency_samples_q8() const noexcept {
  return alignment_delay_.latency_samples_q8() + total_latency_q8(pre_inserts_);
}

int ChannelStrip::post_fader_latency_samples_q8() const noexcept {
  return pre_fader_latency_samples_q8() + total_latency_q8(post_inserts_);
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

bool ChannelStrip::schedule_width_automation(int64_t sample_pos, float width,
                                             AutomationCurveType curve) noexcept {
  AutomationEvent event;
  event.sample_pos = sample_pos;
  event.value = width;
  event.curve = curve;
  event.target.kind = AutomationTargetKind::Width;
  return width_automation_.push(event);
}

bool ChannelStrip::schedule_fader_automation(int64_t sample_pos, float fader_db,
                                             AutomationCurveType curve) noexcept {
  AutomationEvent event;
  event.sample_pos = sample_pos;
  event.value = fader_db;
  event.curve = curve;
  event.target.kind = AutomationTargetKind::Fader;
  return fader_automation_.push(event);
}

bool ChannelStrip::schedule_pan_automation(int64_t sample_pos, float pan,
                                           AutomationCurveType curve) noexcept {
  AutomationEvent event;
  event.sample_pos = sample_pos;
  event.value = pan;
  event.curve = curve;
  event.target.kind = AutomationTargetKind::Pan;
  return pan_automation_.push(event);
}

bool ChannelStrip::schedule_insert_automation(unsigned int insert_index, unsigned int param_id,
                                              int64_t sample_pos, float value,
                                              AutomationCurveType curve) noexcept {
  constexpr unsigned int kMaxReasonableParamId = 65535u;
  if (param_id > kMaxReasonableParamId) {
    return false;
  }
  const size_t idx = insert_index;
  const size_t pre_count = pre_inserts_.size();
  rt::ProcessorBase* insert = nullptr;
  if (idx < pre_count) {
    insert = pre_inserts_[idx].get();
  } else if (idx - pre_count < post_inserts_.size()) {
    insert = post_inserts_[idx - pre_count].get();
  }
  if (insert == nullptr || !insert->parameter_is_realtime_safe(param_id)) {
    return false;
  }

  AutomationEvent event;
  event.sample_pos = sample_pos;
  event.value = value;
  event.curve = curve;
  event.target.kind = AutomationTargetKind::InsertParameter;
  event.target.insert_index = insert_index;
  event.target.param_id = param_id;
  for (auto& lane : insert_automation_) {
    if (lane.target == event.target && lane.lane) {
      return lane.lane->push(event);
    }
  }
  // Hard cap: push_back below MUST NOT reallocate, because the audio thread
  // may concurrently iterate insert_automation_ in process_at(). Capacity is
  // reserved up-front in the constructor (kMaxInsertAutomationLanes).
  if (insert_automation_.size() >= kMaxInsertAutomationLanes) {
    return false;
  }
  insert_automation_.push_back({event.target, std::make_unique<AutomationLane>()});
  return insert_automation_.back().lane->push(event);
}

void ChannelStrip::apply_automation_event(const AutomationEvent& event) noexcept {
  switch (event.target.kind) {
    case AutomationTargetKind::Fader:
      set_fader_db(event.value);
      break;
    case AutomationTargetKind::Pan:
      set_pan(event.value);
      break;
    case AutomationTargetKind::Width:
      set_width(event.value);
      break;
    case AutomationTargetKind::InsertParameter: {
      // insert_index addresses the combined sequence [pre_inserts_ ... post_inserts_ ...].
      const size_t idx = event.target.insert_index;
      const size_t pre_count = pre_inserts_.size();
      rt::ProcessorBase* insert = nullptr;
      if (idx < pre_count) {
        insert = pre_inserts_[idx].get();
      } else if (idx - pre_count < post_inserts_.size()) {
        insert = post_inserts_[idx - pre_count].get();
      }
      if (insert != nullptr && insert->parameter_is_realtime_safe(event.target.param_id)) {
        // Ignore unrecognized ids / out-of-range; no-op on failure.
        insert->set_parameter(event.target.param_id, event.value);
      }
      break;
    }
    case AutomationTargetKind::Send:
      // Send automation is consumed separately from the send_automation_ lanes in
      // mix_send_at, not through this function, so this case is intentionally a no-op
      // (kept only for -Wswitch exhaustiveness).
      break;
  }
}

MeterSnapshot ChannelStrip::meter_snapshot(TapPoint tap) const noexcept {
  return tap == TapPoint::PreFader ? pre_meter_.snapshot() : post_meter_.snapshot();
}

size_t ChannelStrip::read_goniometer_latest(GoniometerPoint* dest,
                                            size_t max_points) const noexcept {
  return goniometer_.read_latest(dest, max_points);
}

void ChannelStrip::add_pre_insert(std::unique_ptr<rt::ProcessorBase> processor) {
  if (!processor) {
    throw std::invalid_argument("insert processor must not be null");
  }
  if (pre_inserts_.size() + post_inserts_.size() >= kMaxInserts) {
    throw std::length_error("ChannelStrip insert cap exceeded");
  }
  if (max_block_size_ > 0) {
    processor->prepare(sample_rate_, max_block_size_);
  }
  pre_inserts_.push_back(std::move(processor));
  insert_sidechains_.resize(pre_inserts_.size() + post_inserts_.size());
}

void ChannelStrip::add_post_insert(std::unique_ptr<rt::ProcessorBase> processor) {
  if (!processor) {
    throw std::invalid_argument("insert processor must not be null");
  }
  if (pre_inserts_.size() + post_inserts_.size() >= kMaxInserts) {
    throw std::length_error("ChannelStrip insert cap exceeded");
  }
  if (max_block_size_ > 0) {
    processor->prepare(sample_rate_, max_block_size_);
  }
  post_inserts_.push_back(std::move(processor));
  insert_sidechains_.resize(pre_inserts_.size() + post_inserts_.size());
}

void ChannelStrip::set_insert_sidechain(unsigned int insert_index, const float* const* channels,
                                        int num_channels, int num_samples) {
  const size_t index = insert_index;
  if (index >= pre_inserts_.size() + post_inserts_.size()) {
    return;
  }
  if (insert_sidechains_.size() < pre_inserts_.size() + post_inserts_.size()) {
    insert_sidechains_.resize(pre_inserts_.size() + post_inserts_.size());
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
  for (auto& sidechain : insert_sidechains_) {
    sidechain = {};
  }
}

size_t ChannelStrip::add_send(const SendConfig& cfg) {
  sends_.push_back(std::make_unique<SendProcessor>(cfg));
  send_automation_.push_back(std::make_unique<AutomationLane>());
  if (max_block_size_ > 0) {
    sends_.back()->prepare(sample_rate_, max_block_size_);
  }
  return sends_.size() - 1;
}

void ChannelStrip::set_send_db(size_t index, float db) {
  if (index >= sends_.size()) {
    return;
  }
  sends_[index]->set_send_db(db);
}

bool ChannelStrip::schedule_send_automation(size_t index, int64_t sample_pos, float db,
                                            AutomationCurveType curve) noexcept {
  if (index >= send_automation_.size()) {
    return false;
  }
  if (!send_automation_[index]) {
    return false;
  }
  AutomationEvent event;
  event.sample_pos = sample_pos;
  event.value = db;
  event.curve = curve;
  event.target.kind = AutomationTargetKind::Send;
  event.target.param_id = static_cast<uint32_t>(index);
  return send_automation_[index]->push(event);
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

void ChannelStrip::mix_send(size_t index, float* const* dest, int num_channels, int num_samples) {
  mix_send_at(index, dest, num_channels, num_samples, 0);
}

void ChannelStrip::mix_send_at(size_t index, float* const* dest, int num_channels, int num_samples,
                               int64_t block_start) {
  if (index >= sends_.size() || dest == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  SendProcessor& send = *sends_[index];
  auto& tap = (send.timing() == SendTiming::PreFader) ? pre_tap_ : post_tap_;

  const int rows =
      std::min<int>(std::min(num_channels, kPreparedChannels), static_cast<int>(tap.size()));
  const int n = std::min(num_samples, max_block_size_);

  float* temp[kPreparedChannels];
  for (int ch = 0; ch < rows; ++ch) {
    std::copy(tap[ch].begin(), tap[ch].begin() + n, send_temp_[ch].begin());
    temp[ch] = send_temp_[ch].data();
  }

  std::array<AutomationBlockEvent, kMaxAutomationEventsPerBlock> send_events{};
  const size_t send_count =
      index < send_automation_.size() && send_automation_[index]
          ? consume_events(*send_automation_[index], block_start, n, send_events)
          : 0;

  if (send_count == 0) {
    // Applies the smoothed send gain in place on the copied tap, leaving dest untouched.
    send.process(temp, rows, n);
  } else {
    size_t send_event_index = 0;
    int cursor = 0;
    while (cursor < n) {
      while (send_event_index < send_count && send_events[send_event_index].offset == cursor) {
        send.set_send_db(send_events[send_event_index++].event.value);
      }
      const int next_offset = next_event_offset(send_events, send_count, send_event_index, n);
      const int segment_samples = std::max(0, next_offset - cursor);
      if (segment_samples > 0) {
        float* segment[kPreparedChannels]{};
        for (int ch = 0; ch < rows; ++ch) {
          segment[ch] = send_temp_[ch].data() + cursor;
        }
        send.process(segment, rows, segment_samples);
        cursor += segment_samples;
      } else {
        ++cursor;
      }
    }
  }

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

}  // namespace sonare::mixing
