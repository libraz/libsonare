#include <algorithm>
#include <cmath>

#include "analysis/progression_patterns.h"
#include "filters/chroma.h"
#include "streaming/stream_analyzer.h"
#include "streaming/stream_analyzer_publication.h"
#include "streaming/stream_analyzer_utils.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare {

using namespace streaming_detail;

namespace {

void validate_quantize_config(const QuantizeConfig& config) {
  if (!numeric::finite_ordered_range(config.mel_db_min, config.mel_db_max) ||
      !numeric::finite_positive(config.onset_max) || !numeric::finite_positive(config.rms_max) ||
      !numeric::finite_positive(config.centroid_max)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid stream quantization range");
  }
}

uint32_t output_feature_flags(const StreamConfig& config) noexcept {
  uint32_t flags = 0;
  if (config.compute_mel) flags |= kStreamFeatureMel;
  if (config.compute_chroma) flags |= kStreamFeatureChroma;
  if (config.compute_onset) flags |= kStreamFeatureOnset;
  if (config.compute_spectral) flags |= kStreamFeatureSpectral;
  return flags;
}

void copy_progressive_scalars(const ProgressiveEstimate& source, ProgressiveEstimate& target) {
  target.bpm = source.bpm;
  target.bpm_confidence = source.bpm_confidence;
  target.bpm_candidate_count = source.bpm_candidate_count;
  target.key = source.key;
  target.key_minor = source.key_minor;
  target.key_confidence = source.key_confidence;
  target.chord_root = source.chord_root;
  target.chord_quality = source.chord_quality;
  target.chord_confidence = source.chord_confidence;
  target.chord_start_time = source.chord_start_time;
  target.current_bar = source.current_bar;
  target.bar_duration = source.bar_duration;
  target.pattern_length = source.pattern_length;
  target.detected_pattern_score = source.detected_pattern_score;
  target.accumulated_seconds = source.accumulated_seconds;
  target.used_frames = source.used_frames;
  target.updated = source.updated;
}

template <typename T>
void copy_append_only_history(const std::vector<T>& source, std::vector<T>& storage,
                              size_t& stored_size, size_t source_drops,
                              size_t previous_source_drops) noexcept {
  size_t begin = stored_size;
  if (source_drops != previous_source_drops || stored_size > source.size()) begin = 0;
  for (size_t i = begin; i < source.size(); ++i) storage[i] = source[i];
  stored_size = source.size();
}

}  // namespace

void StreamAnalyzer::prepare_output_frame(StreamFrame& frame) const {
  if (config_.compute_magnitude) {
    frame.magnitude.reserve(static_cast<size_t>(config_.n_bins() / config_.magnitude_downsample));
  }
  if (config_.compute_mel) {
    frame.mel.reserve(static_cast<size_t>(config_.n_mels));
  }
  if (config_.compute_chroma) {
    frame.chroma.reserve(12);
  }
}

void StreamAnalyzer::prepare_progressive_estimate() {
  current_estimate_.chord_progression.reserve(config_.max_progression_entries);
  current_estimate_.bar_chord_progression.reserve(config_.max_progression_entries);
  current_estimate_.voted_pattern.reserve(4);

  // Bounds for everything the progression path touches on the audio thread,
  // taken from the pattern table itself so they stay correct when it changes.
  // A correction is recorded at most once per position, and only for a pattern
  // whose length equals the voted pattern's, so the longest pattern bounds both
  // correction lists; the longest name bounds every name buffer.
  const auto& patterns = known_progression_patterns();
  size_t max_pattern_length = 0;
  size_t max_pattern_name_length = 0;
  for (const auto& pattern : patterns) {
    max_pattern_length = std::max(max_pattern_length, pattern.chords.size());
    max_pattern_name_length = std::max(max_pattern_name_length, pattern.name.size());
  }
  pattern_corrections_.reserve(max_pattern_length);
  best_pattern_correction_.reserve(max_pattern_length);
  correction_pattern_name_.reserve(max_pattern_name_length);
  detected_pattern_scratch_name_.reserve(max_pattern_name_length);

  // Pre-size the score entries and their names: detect_progression_pattern()
  // rewrites them in place every call rather than clearing, which would release
  // the name buffers it is about to need again.
  current_estimate_.all_pattern_scores.resize(patterns.size());
  for (auto& score : current_estimate_.all_pattern_scores) {
    score.first.reserve(max_pattern_name_length);
  }
  current_estimate_.detected_pattern_name.reserve(std::max<size_t>(64, max_pattern_name_length));
}

void StreamAnalyzer::initialize_stats_publication() {
  const size_t pattern_count = known_progression_patterns().size();
  for (auto& slot : publication_->stats_slots) {
    auto& estimate = slot.storage.estimate;
    estimate.chord_progression.resize(config_.max_progression_entries);
    estimate.bar_chord_progression.resize(config_.max_progression_entries);
    estimate.voted_pattern.resize(4);
    estimate.all_pattern_scores.resize(pattern_count);
    estimate.detected_pattern_name.reserve(64);
    for (auto& score : estimate.all_pattern_scores) score.first.reserve(64);
  }
}

bool StreamAnalyzer::try_begin_output_write(size_t* write_index) noexcept {
  if (write_index == nullptr || output_buffer_.empty()) return false;
  const uint64_t read = publication_->output_read_sequence.load(std::memory_order_acquire);
  const uint64_t write = publication_->producer_write_sequence;
  if (write - read >= output_buffer_.size()) {
    ++publication_->producer_dropped_frames;
    return false;
  }
  *write_index = static_cast<size_t>(write % output_buffer_.size());
  return true;
}

void StreamAnalyzer::publish_output_write() noexcept {
  ++publication_->producer_write_sequence;
  publication_->output_write_sequence.store(publication_->producer_write_sequence,
                                            std::memory_order_release);
}

void StreamAnalyzer::reset_publication() noexcept {
  publication_->output_read_sequence.store(0, std::memory_order_relaxed);
  publication_->output_write_sequence.store(0, std::memory_order_relaxed);
  publication_->producer_write_sequence = 0;
  publication_->producer_dropped_frames = 0;
  publication_->published_stats_slot.store(0, std::memory_order_relaxed);
  for (unsigned i = 0; i < StreamAnalyzerPublication::kStatsSlotCount; ++i) {
    publication_->stats_slot_states[i].store(
        i == 0 ? StreamAnalyzerPublication::StatsSlotState::kPublished
               : StreamAnalyzerPublication::StatsSlotState::kFree,
        std::memory_order_relaxed);
    auto& slot = publication_->stats_slots[i];
    slot.chord_progression_size = 0;
    slot.bar_chord_progression_size = 0;
    slot.voted_pattern_size = 0;
    slot.all_pattern_scores_size = 0;
    slot.storage.dropped_chord_progression_entries = 0;
    slot.storage.dropped_bar_progression_entries = 0;
  }
}

void StreamAnalyzer::publish_stats_snapshot() noexcept {
  const unsigned current = publication_->published_stats_slot.load(std::memory_order_relaxed);
  unsigned target = StreamAnalyzerPublication::kStatsSlotCount;
  for (unsigned i = 0; i < StreamAnalyzerPublication::kStatsSlotCount; ++i) {
    if (i == current) continue;
    auto expected = StreamAnalyzerPublication::StatsSlotState::kFree;
    if (publication_->stats_slot_states[i].compare_exchange_strong(
            expected, StreamAnalyzerPublication::StatsSlotState::kWriting,
            std::memory_order_acquire, std::memory_order_relaxed)) {
      target = i;
      break;
    }
  }
  // Under the documented one-producer/one-consumer contract, three slots
  // guarantee a free target. Keep the RT path non-blocking if the contract is
  // violated: retain the previous coherent snapshot instead of waiting.
  if (target == StreamAnalyzerPublication::kStatsSlotCount) return;

  auto& slot = publication_->stats_slots[target];
  AnalyzerStats& snapshot = slot.storage;
  snapshot.total_frames = frame_count_;
  snapshot.total_samples = cumulative_samples_;
  snapshot.duration_seconds = static_cast<float>(cumulative_samples_) / config_.sample_rate;
  snapshot.pending_frames = available_frames();
  snapshot.dropped_output_frames = publication_->producer_dropped_frames;

  copy_progressive_scalars(current_estimate_, snapshot.estimate);
  const size_t previous_chord_drops = snapshot.dropped_chord_progression_entries;
  const size_t previous_bar_drops = snapshot.dropped_bar_progression_entries;
  copy_append_only_history(current_estimate_.chord_progression, snapshot.estimate.chord_progression,
                           slot.chord_progression_size, dropped_chord_progression_entries_,
                           previous_chord_drops);
  copy_append_only_history(current_estimate_.bar_chord_progression,
                           snapshot.estimate.bar_chord_progression, slot.bar_chord_progression_size,
                           dropped_bar_progression_entries_, previous_bar_drops);
  snapshot.dropped_chord_progression_entries = dropped_chord_progression_entries_;
  snapshot.dropped_bar_progression_entries = dropped_bar_progression_entries_;

  slot.voted_pattern_size = current_estimate_.voted_pattern.size();
  for (size_t i = 0; i < slot.voted_pattern_size; ++i) {
    snapshot.estimate.voted_pattern[i] = current_estimate_.voted_pattern[i];
  }
  snapshot.estimate.detected_pattern_name = current_estimate_.detected_pattern_name;
  // The estimate's score vector is storage kept at full size so its name
  // buffers survive between passes; the meaningful prefix is the count the last
  // scoring pass wrote, mirroring how the slots already separate the two.
  slot.all_pattern_scores_size = all_pattern_scores_count_;
  for (size_t i = 0; i < slot.all_pattern_scores_size; ++i) {
    snapshot.estimate.all_pattern_scores[i] = current_estimate_.all_pattern_scores[i];
  }

  publication_->stats_slot_states[target].store(
      StreamAnalyzerPublication::StatsSlotState::kPublished, std::memory_order_release);
  publication_->published_stats_slot.store(target, std::memory_order_release);
  auto expected = StreamAnalyzerPublication::StatsSlotState::kPublished;
  publication_->stats_slot_states[current].compare_exchange_strong(
      expected, StreamAnalyzerPublication::StatsSlotState::kFree, std::memory_order_release,
      std::memory_order_relaxed);
}

const StreamFrame& StreamAnalyzer::output_front() const {
  const uint64_t sequence = publication_->output_read_sequence.load(std::memory_order_relaxed);
  return output_buffer_[static_cast<size_t>(sequence % output_buffer_.size())];
}

void StreamAnalyzer::pop_output_front() {
  const uint64_t read = publication_->output_read_sequence.load(std::memory_order_relaxed);
  publication_->output_read_sequence.store(read + 1, std::memory_order_release);
}

size_t StreamAnalyzer::available_frames() const {
  const uint64_t write = publication_->output_write_sequence.load(std::memory_order_acquire);
  const uint64_t read = publication_->output_read_sequence.load(std::memory_order_relaxed);
  return static_cast<size_t>(write - read);
}

std::vector<StreamFrame> StreamAnalyzer::read_frames(size_t max_frames) {
  size_t count = std::min(max_frames, available_frames());
  std::vector<StreamFrame> result;
  result.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    result.push_back(output_front());
    pop_output_front();
  }

  return result;
}

void StreamAnalyzer::read_frames_soa(size_t max_frames, FrameBuffer& buffer) {
  buffer.clear();

  size_t count = std::min(max_frames, available_frames());
  buffer.n_frames = count;
  buffer.reserve(count, config_.compute_mel ? config_.n_mels : 0, config_.compute_chroma ? 12 : 0,
                 output_feature_flags(config_));

  if (count == 0) {
    return;
  }

  for (size_t i = 0; i < count; ++i) {
    const StreamFrame& frame = output_front();

    buffer.timestamps.push_back(frame.timestamp);
    if (config_.compute_onset) buffer.onset_strength.push_back(frame.onset_strength);
    buffer.rms_energy.push_back(frame.rms_energy);
    if (config_.compute_spectral) {
      buffer.spectral_centroid.push_back(frame.spectral_centroid);
      buffer.spectral_flatness.push_back(frame.spectral_flatness);
    }
    if (config_.compute_chroma) {
      buffer.chord_root.push_back(frame.chord_root);
      buffer.chord_quality.push_back(frame.chord_quality);
      buffer.chord_confidence.push_back(frame.chord_confidence);
    }

    buffer.mel.insert(buffer.mel.end(), frame.mel.begin(), frame.mel.end());
    buffer.chroma.insert(buffer.chroma.end(), frame.chroma.begin(), frame.chroma.end());

    pop_output_front();
  }
}

void StreamAnalyzer::read_frames_quantized_u8(size_t max_frames, QuantizedFrameBufferU8& buffer,
                                              const QuantizeConfig& qconfig) {
  validate_quantize_config(qconfig);
  buffer.clear();

  size_t count = std::min(max_frames, available_frames());
  buffer.n_frames = count;
  buffer.reserve(count, config_.compute_mel ? config_.n_mels : 0, config_.compute_chroma ? 12 : 0,
                 output_feature_flags(config_));

  if (count == 0) {
    return;
  }

  for (size_t i = 0; i < count; ++i) {
    const StreamFrame& frame = output_front();

    buffer.timestamps.push_back(frame.timestamp);

    for (float mel_power : frame.mel) {
      float db = single_power_to_db(mel_power);
      buffer.mel.push_back(quantize_to_u8(db, qconfig.mel_db_min, qconfig.mel_db_max));
    }

    for (float c : frame.chroma) {
      buffer.chroma.push_back(quantize_to_u8(c, 0.0f, 1.0f));
    }

    if (config_.compute_onset) {
      buffer.onset_strength.push_back(
          quantize_to_u8(frame.onset_strength, 0.0f, qconfig.onset_max));
    }
    buffer.rms_energy.push_back(quantize_to_u8(frame.rms_energy, 0.0f, qconfig.rms_max));
    if (config_.compute_spectral) {
      buffer.spectral_centroid.push_back(
          quantize_to_u8(frame.spectral_centroid, 0.0f, qconfig.centroid_max));
      buffer.spectral_flatness.push_back(quantize_to_u8(frame.spectral_flatness, 0.0f, 1.0f));
    }

    pop_output_front();
  }
}

void StreamAnalyzer::read_frames_quantized_i16(size_t max_frames, QuantizedFrameBufferI16& buffer,
                                               const QuantizeConfig& qconfig) {
  validate_quantize_config(qconfig);
  buffer.clear();

  size_t count = std::min(max_frames, available_frames());
  buffer.n_frames = count;
  buffer.reserve(count, config_.compute_mel ? config_.n_mels : 0, config_.compute_chroma ? 12 : 0,
                 output_feature_flags(config_));

  if (count == 0) {
    return;
  }

  for (size_t i = 0; i < count; ++i) {
    const StreamFrame& frame = output_front();

    buffer.timestamps.push_back(frame.timestamp);

    for (float mel_power : frame.mel) {
      float db = single_power_to_db(mel_power);
      buffer.mel.push_back(quantize_to_i16(db, qconfig.mel_db_min, qconfig.mel_db_max));
    }

    for (float c : frame.chroma) {
      buffer.chroma.push_back(quantize_to_i16(c, 0.0f, 1.0f));
    }

    if (config_.compute_onset) {
      buffer.onset_strength.push_back(
          quantize_to_i16(frame.onset_strength, 0.0f, qconfig.onset_max));
    }
    buffer.rms_energy.push_back(quantize_to_i16(frame.rms_energy, 0.0f, qconfig.rms_max));
    if (config_.compute_spectral) {
      buffer.spectral_centroid.push_back(
          quantize_to_i16(frame.spectral_centroid, 0.0f, qconfig.centroid_max));
      buffer.spectral_flatness.push_back(quantize_to_i16(frame.spectral_flatness, 0.0f, 1.0f));
    }

    pop_output_front();
  }
}

void StreamAnalyzer::reset(size_t base_sample_offset) {
  offset_tracking_mode_ = OffsetTrackingMode::Unset;
  next_external_sample_offset_ = 0;
  cumulative_samples_ = base_sample_offset;
  cumulative_samples_exact_ = static_cast<double>(base_sample_offset);
  frame_count_ = 0;
  emitted_frame_count_ = 0;
  finalized_ = false;

  overlap_buffer_.clear();
  overlap_read_pos_ = 0;
  reset_publication();

  if (needs_mel_analysis_) {
    std::fill(prev_mel_log_.begin(), prev_mel_log_.end(), 0.0f);
  }
  has_prev_frame_ = false;

  onset_accumulator_start_ = 0;
  onset_accumulator_size_ = 0;
  chroma_sum_.fill(0.0f);
  chroma_frame_count_ = 0;
  last_key_update_time_ = 0.0f;
  last_bpm_update_time_ = 0.0f;
  current_estimate_ = ProgressiveEstimate();
  prepare_progressive_estimate();
  all_pattern_scores_count_ = 0;
  dropped_chord_progression_entries_ = 0;
  dropped_bar_progression_entries_ = 0;

  prev_chord_root_ = -1;
  prev_chord_quality_ = -1;
  chord_stable_time_ = 0.0f;
  current_chord_start_time_ = 0.0f;
  prev_chord_confidence_ = 0.0f;
  chroma_history_start_ = 0;
  chroma_history_size_ = 0;
  full_chroma_history_start_ = 0;
  full_chroma_history_size_ = 0;
  full_chroma_history_offset_ = 0;
  if (stream_resampler_) {
    stream_resampler_->reset();
  }

  bar_tracking_active_ = false;
  bar_duration_ = 0.0f;
  current_bar_index_ = -1;
  bar_start_time_ = 0.0f;
  bar_chord_votes_.fill(0);
  bar_vote_count_ = 0;

  pattern_locked_ = false;
  publish_stats_snapshot();
}

void StreamAnalyzer::set_expected_duration(float duration_seconds) {
  if (!numeric::finite_non_negative(duration_seconds)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "expected duration must be finite and non-negative");
  }
  expected_duration_ = duration_seconds;
}

void StreamAnalyzer::set_normalization_gain(float gain) {
  if (!numeric::finite_positive(gain)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "normalization gain must be finite and positive");
  }
  normalization_gain_ = std::clamp(gain, 0.01f, 100.0f);
}

void StreamAnalyzer::set_tuning_ref_hz(float ref_hz) {
  // Rejected, not clamped, and over the same range StreamConfig accepts at
  // construction: clamping here would silently turn a 1000 Hz request into
  // 880 Hz while the same value handed to the constructor was refused, so the
  // chromagram would depend on which entry point the host used.
  if (!numeric::finite_positive(ref_hz) || ref_hz < kMinTuningRefHz || ref_hz > kMaxTuningRefHz) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "tuning reference must be finite and within 220..880 Hz");
  }
  config_.tuning_ref_hz = ref_hz;

  if (config_.compute_chroma) {
    ChromaFilterConfig chroma_config;
    chroma_config.n_chroma = 12;
    chroma_config.tuning = constants::kSemitonesPerOctave * std::log2(ref_hz / constants::kA4Hz);
    /// Minimum frequency ~C2; see kStreamingChromaFminHz.
    chroma_config.fmin = kStreamingChromaFminHz;
    chroma_filterbank_ =
        create_chroma_filterbank(internal_sample_rate_, config_.n_fft, chroma_config);
  }
}

AnalyzerStats StreamAnalyzer::stats() const {
  unsigned index = 0;
  for (;;) {
    index = publication_->published_stats_slot.load(std::memory_order_acquire);
    auto expected = StreamAnalyzerPublication::StatsSlotState::kPublished;
    if (publication_->stats_slot_states[index].compare_exchange_strong(
            expected, StreamAnalyzerPublication::StatsSlotState::kReading,
            std::memory_order_acquire, std::memory_order_relaxed)) {
      break;
    }
  }

  struct ReaderPin {
    StreamAnalyzerPublication* publication;
    unsigned index;
    ~ReaderPin() {
      publication->stats_slot_states[index].store(
          StreamAnalyzerPublication::StatsSlotState::kPublished, std::memory_order_release);
      if (publication->published_stats_slot.load(std::memory_order_acquire) != index) {
        auto expected = StreamAnalyzerPublication::StatsSlotState::kPublished;
        publication->stats_slot_states[index].compare_exchange_strong(
            expected, StreamAnalyzerPublication::StatsSlotState::kFree, std::memory_order_release,
            std::memory_order_relaxed);
      }
    }
  } pin{publication_.get(), index};

  const auto& slot = publication_->stats_slots[index];
  const AnalyzerStats& snapshot = slot.storage;
  AnalyzerStats result;
  result.total_frames = snapshot.total_frames;
  result.total_samples = snapshot.total_samples;
  result.duration_seconds = snapshot.duration_seconds;
  result.pending_frames = available_frames();
  result.dropped_output_frames = snapshot.dropped_output_frames;
  result.dropped_chord_progression_entries = snapshot.dropped_chord_progression_entries;
  result.dropped_bar_progression_entries = snapshot.dropped_bar_progression_entries;
  copy_progressive_scalars(snapshot.estimate, result.estimate);
  result.estimate.chord_progression.assign(
      snapshot.estimate.chord_progression.begin(),
      snapshot.estimate.chord_progression.begin() +
          static_cast<std::ptrdiff_t>(slot.chord_progression_size));
  result.estimate.bar_chord_progression.assign(
      snapshot.estimate.bar_chord_progression.begin(),
      snapshot.estimate.bar_chord_progression.begin() +
          static_cast<std::ptrdiff_t>(slot.bar_chord_progression_size));
  result.estimate.voted_pattern.assign(snapshot.estimate.voted_pattern.begin(),
                                       snapshot.estimate.voted_pattern.begin() +
                                           static_cast<std::ptrdiff_t>(slot.voted_pattern_size));
  result.estimate.detected_pattern_name = snapshot.estimate.detected_pattern_name;
  result.estimate.all_pattern_scores.assign(
      snapshot.estimate.all_pattern_scores.begin(),
      snapshot.estimate.all_pattern_scores.begin() +
          static_cast<std::ptrdiff_t>(slot.all_pattern_scores_size));
  return result;
}

int StreamAnalyzer::frame_count() const { return stats().total_frames; }

float StreamAnalyzer::current_time() const { return stats().duration_seconds; }

}  // namespace sonare
