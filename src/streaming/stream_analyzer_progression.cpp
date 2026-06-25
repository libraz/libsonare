#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "analysis/chord_analyzer.h"
#include "analysis/key_profiles.h"
#include "analysis/progression_patterns.h"
#include "streaming/stream_analyzer.h"
#include "streaming/stream_analyzer_utils.h"
#include "util/constants.h"

namespace sonare {

using sonare::constants::kEpsilon;
using namespace streaming_detail;

/// Seconds over which progressive key/BPM confidence ramps to full.
constexpr float kConfidenceRampSeconds = 30.0f;

void StreamAnalyzer::update_progressive_estimate(float current_time) {
  current_estimate_.accumulated_seconds = current_time;
  current_estimate_.used_frames = frame_count_;
  current_estimate_.updated = false;

  /// Update key estimate using Krumhansl-Schmuckler correlation
  if (config_.compute_chroma && chroma_frame_count_ > 0) {
    float time_since_key_update = current_time - last_key_update_time_;
    if (time_since_key_update >= config_.key_update_interval_sec) {
      /// Normalize chroma_sum for key detection
      std::array<float, 12> mean_chroma;
      float sum = 0.0f;
      for (int c = 0; c < 12; ++c) {
        mean_chroma[c] = chroma_sum_[c] / chroma_frame_count_;
        sum += mean_chroma[c];
      }
      if (sum > kEpsilon) {
        for (int c = 0; c < 12; ++c) {
          mean_chroma[c] /= sum;
        }
      }

      const MajorMinorKeyMatch key_match = find_best_major_minor_key(mean_chroma);
      current_estimate_.key = key_match.root;
      current_estimate_.key_minor = key_match.minor;

      /// Confidence based on correlation strength and time
      float time_factor = std::min(1.0f, current_time / kConfidenceRampSeconds);
      float corr_factor = (key_match.correlation + 1.0f) / 2.0f;  // Normalize [-1, 1] to [0, 1]
      current_estimate_.key_confidence = corr_factor * time_factor;

      last_key_update_time_ = current_time;
      current_estimate_.updated = true;
    }

    /// Update chord estimate (every frame, using smoothed chroma)
    if (!chord_templates_.empty() && !chroma_history_.empty()) {
      /// Compute median-filtered chroma (more robust to noise than averaging)
      std::array<float, 12> smoothed_chroma = compute_median_chroma(chroma_history_);

      auto [best_chord, chord_corr] = find_best_chord(smoothed_chroma.data(), chord_templates_);
      int new_root = static_cast<int>(best_chord.root);
      int new_quality = static_cast<int>(best_chord.quality);
      float new_confidence = std::max(0.0f, chord_corr);

      /// Only update if confidence is above threshold
      if (new_confidence >= kChordConfidenceThreshold) {
        current_estimate_.chord_root = new_root;
        current_estimate_.chord_quality = new_quality;
        current_estimate_.chord_confidence = new_confidence;
      } else {
        /// Keep current estimate but update confidence
        current_estimate_.chord_confidence = new_confidence;
      }

      /// Track chord progression incrementally from the same smoothed chord
      /// stream used for per-frame output. This avoids recomputing the whole
      /// progression in a getter or periodically discarding this state.
      if (new_confidence >= kChordConfidenceThreshold) {
        float frame_duration =
            static_cast<float>(config_.hop_length) / static_cast<float>(internal_sample_rate_);

        if (new_root == prev_chord_root_ && new_quality == prev_chord_quality_) {
          /// Same chord - accumulate stable time and track the held chord's
          /// peak confidence so the eventual ChordChange reports this chord's
          /// own strength, not the next chord's.
          chord_stable_time_ += frame_duration;
          prev_chord_confidence_ = std::max(prev_chord_confidence_, new_confidence);
        } else if (prev_chord_root_ < 0) {
          /// First ever stable chord: seed its start time at the current frame.
          /// (The general "changed" branch below also seeds new chords, but it
          /// only runs once prev_chord_root_ is valid, so handle the very first
          /// chord here so current_chord_start_time_ is never left at 0 by
          /// default.)
          prev_chord_root_ = new_root;
          prev_chord_quality_ = new_quality;
          chord_stable_time_ = frame_duration;
          prev_chord_confidence_ = new_confidence;
          current_chord_start_time_ = current_time;
        } else {
          /// Chord changed - check if previous chord was stable long enough
          if (prev_chord_root_ >= 0 && chord_stable_time_ >= kChordMinDuration) {
            /// Find the start time of the previous chord
            float chord_start = current_time - chord_stable_time_;

            /// Only add if it's a new chord or first chord
            if (current_estimate_.chord_progression.empty() ||
                current_estimate_.chord_progression.back().root != prev_chord_root_ ||
                current_estimate_.chord_progression.back().quality != prev_chord_quality_) {
              ChordChange change;
              change.root = prev_chord_root_;
              change.quality = prev_chord_quality_;
              change.start_time = chord_start;
              /// Use the *completed* chord's accumulated peak confidence, not
              /// new_confidence (which belongs to the chord that triggered the
              /// transition).
              change.confidence = prev_chord_confidence_;
              current_estimate_.chord_progression.push_back(change);
            }
          }

          /// Reset for new chord, seeding its confidence with this first frame.
          prev_chord_root_ = new_root;
          prev_chord_quality_ = new_quality;
          chord_stable_time_ = frame_duration;
          prev_chord_confidence_ = new_confidence;
          current_chord_start_time_ = current_time;
        }
      }

      /// Expose when the current chord began. chord_start_time was declared in
      /// ProgressiveEstimate and surfaced through every binding but was never
      /// assigned, so it always read 0.0. current_chord_start_time_ is updated
      /// whenever the held chord changes (above), so this reflects the start of
      /// the chord currently reported in current_estimate_.chord_root/quality.
      current_estimate_.chord_start_time = current_chord_start_time_;
    }
  }

  /// Update BPM estimate
  if (config_.compute_onset) {
    int n_onset = static_cast<int>(onset_accumulator_.size());
    current_estimate_.bpm_candidate_count = n_onset;

    float time_since_bpm_update = current_time - last_bpm_update_time_;
    if (time_since_bpm_update >= config_.bpm_update_interval_sec && n_onset >= kMinOnsetFrames) {
      /// Compute max lag based on minimum BPM (use internal sample rate)
      int max_lag = bpm_to_lag(kBpmMin, internal_sample_rate_, config_.hop_length);
      max_lag = std::min(max_lag, n_onset - 1);

      if (max_lag > 2) {
        /// Compute autocorrelation over the bounded onset window. Copy the
        /// deque into a contiguous vector for the (vectorized) autocorrelation;
        /// this runs only on the throttled BPM-update cadence and over a
        /// window-bounded length, so it stays O(1) per stream rather than
        /// growing with total duration.
        std::vector<float> onset_window(onset_accumulator_.begin(), onset_accumulator_.end());
        auto autocorr = compute_autocorrelation_streaming(onset_window, max_lag);

        /// Find best tempo (use internal sample rate)
        auto [bpm, rel_confidence] =
            find_best_tempo(autocorr, internal_sample_rate_, config_.hop_length, kBpmMin, kBpmMax);

        current_estimate_.bpm = bpm;

        /// Combine relative confidence with time-based confidence
        /// Time factor: confidence increases as we get more data (up to the ramp)
        float time_factor = std::min(1.0f, current_time / kConfidenceRampSeconds);
        current_estimate_.bpm_confidence = rel_confidence * time_factor;

        last_bpm_update_time_ = current_time;
        current_estimate_.updated = true;
      }
    }
  }

  /// Update bar-synchronized chord tracking
  if (config_.compute_chroma) {
    update_bar_chord_tracking(current_time);
  }
}

void StreamAnalyzer::update_bar_chord_tracking(float current_time) {
  /// Check if BPM is stable enough to start bar tracking
  if (!bar_tracking_active_) {
    if (current_estimate_.bpm_confidence >= kBpmConfidenceThreshold &&
        current_estimate_.bpm > 0.0f) {
      /// Start bar tracking
      bar_tracking_active_ = true;

      bar_duration_ = static_cast<float>(kBeatsPerBar) * 60.0f / current_estimate_.bpm;
      current_bar_index_ = 0;
      bar_start_time_ = current_time;

      /// Compute retroactive bar chords from stored chroma history
      compute_retroactive_bar_chords();

      /// Reset for live bar tracking (state already set by retroactive computation)
      bar_chord_votes_.fill(0);
      bar_vote_count_ = 0;

      /// Update estimate with bar info
      current_estimate_.bar_duration = bar_duration_;
      current_estimate_.current_bar = current_bar_index_;
    }
    return;
  }

  /// Update bar duration if BPM changed significantly
  float new_bar_duration = static_cast<float>(kBeatsPerBar) * 60.0f / current_estimate_.bpm;
  if (std::abs(new_bar_duration - bar_duration_) > 0.1f) {
    bar_duration_ = new_bar_duration;
    current_estimate_.bar_duration = bar_duration_;
  }

  /// Vote for chord using current frame's smoothed chroma (from chroma_history_)
  /// This uses the same smoothed detection as per-frame chord output
  if (!chord_templates_.empty() && !chroma_history_.empty()) {
    /// Compute median-filtered chroma (more robust to noise than averaging)
    std::array<float, 12> smoothed_chroma = compute_median_chroma(chroma_history_);

    /// Detect chord for this frame
    auto [frame_chord, frame_corr] = find_best_chord(smoothed_chroma.data(), chord_templates_);

    /// Only vote if confidence is above threshold
    if (frame_corr >= kChordConfidenceThreshold) {
      int root = static_cast<int>(frame_chord.root);
      int quality = static_cast<int>(frame_chord.quality);

      /// Index: root * kNumChordQualities + quality.
      /// Sized to cover every ChordQuality enumerator so 7ths / sus / extended
      /// chords are no longer silently dropped when the underlying template
      /// set grows beyond simple triads.
      int vote_idx = root * kNumChordQualities + quality;
      if (vote_idx >= 0 && vote_idx < StreamAnalyzer::kBarVoteSlots) {
        ++bar_chord_votes_[vote_idx];
        ++bar_vote_count_;
      }
    }
  }
  /// Check if we've crossed a bar boundary
  if (current_time >= bar_start_time_ + bar_duration_) {
    /// Find chord with most votes
    if (bar_vote_count_ > 0) {
      int best_idx = 0;
      int best_votes = bar_chord_votes_[0];
      for (int i = 1; i < StreamAnalyzer::kBarVoteSlots; ++i) {
        if (bar_chord_votes_[i] > best_votes) {
          best_votes = bar_chord_votes_[i];
          best_idx = i;
        }
      }

      int best_root = best_idx / kNumChordQualities;
      int best_quality = best_idx % kNumChordQualities;
      float confidence = static_cast<float>(best_votes) / static_cast<float>(bar_vote_count_);

      /// Add to bar chord progression
      BarChord bar_chord;
      bar_chord.bar_index = current_bar_index_;
      bar_chord.root = best_root;
      bar_chord.quality = best_quality;
      bar_chord.start_time = bar_start_time_;
      bar_chord.confidence = confidence;
      current_estimate_.bar_chord_progression.push_back(bar_chord);

      /// Update voted pattern and detect progression periodically (every 4 bars)
      if ((current_bar_index_ + 1) % 4 == 0) {
        compute_voted_pattern(4);
        detect_progression_pattern();
      }
    }

    /// Move to next bar
    ++current_bar_index_;
    bar_start_time_ = current_time;
    bar_chord_votes_.fill(0);
    bar_vote_count_ = 0;

    /// Update estimate
    current_estimate_.current_bar = current_bar_index_;
  }
}

void StreamAnalyzer::compute_retroactive_bar_chords() {
  if (full_chroma_history_.empty() || bar_duration_ <= 0.0f) {
    return;
  }

  /// Calculate frames per bar (use internal sample rate)
  float seconds_per_frame = static_cast<float>(config_.hop_length) / internal_sample_rate_;
  int frames_per_bar = static_cast<int>(bar_duration_ / seconds_per_frame + 0.5f);

  if (frames_per_bar <= 0) {
    return;
  }

  /// Absolute time of full_chroma_history_.front(). Once the chroma history cap
  /// is hit the surviving history no longer begins at t=0; without this offset
  /// every retroactive bar start_time would be wrong by the dropped duration.
  const float history_start_sec =
      static_cast<float>(full_chroma_history_offset_) * seconds_per_frame;

  /// How many complete bars can we detect from full history?
  int retroactive_frames = static_cast<int>(full_chroma_history_.size());
  int retroactive_bars = retroactive_frames / frames_per_bar;

  /// Clear existing bar progression and recompute from start
  current_estimate_.bar_chord_progression.clear();

  for (int bar = 0; bar < retroactive_bars; ++bar) {
    int start_frame = bar * frames_per_bar;
    int end_frame = std::min(start_frame + frames_per_bar, retroactive_frames);

    /// Vote for chord using frames in this bar.
    /// Sized to (12 pitch classes * kNumChordQualities) — see kBarVoteSlots
    /// in stream_analyzer.h. Must stay in lockstep with the live-tracking
    /// table above; the static_assert near the top of this file enforces it.
    std::array<int, StreamAnalyzer::kBarVoteSlots> votes = {};
    int vote_count = 0;

    for (int f = start_frame; f < end_frame; ++f) {
      /// Median-smooth chroma with the same per-bin median as the live tracking
      /// path (compute_median_chroma), but over a window *centered* on this
      /// frame. The retroactive pass owns the full history, so it can look ahead
      /// for a symmetric, more accurate estimate; the live path is causal and
      /// can only trail. The two therefore agree in the steady interior of a bar
      /// but may differ by a frame near bar boundaries — this look-ahead is
      /// intentional, not a bit-exact match of the live estimate.
      int smooth_start = std::max(0, f - kChordSmoothingFrames / 2);
      int smooth_end = std::min(retroactive_frames, f + kChordSmoothingFrames / 2);
      std::deque<std::array<float, 12>> window(full_chroma_history_.begin() + smooth_start,
                                               full_chroma_history_.begin() + smooth_end);
      std::array<float, 12> smoothed = compute_median_chroma(window);

      /// Detect chord
      auto [chord, corr] = find_best_chord(smoothed.data(), chord_templates_);
      if (corr >= kChordConfidenceThreshold) {
        int idx =
            static_cast<int>(chord.root) * kNumChordQualities + static_cast<int>(chord.quality);
        if (idx >= 0 && idx < StreamAnalyzer::kBarVoteSlots) {
          ++votes[idx];
          ++vote_count;
        }
      }
    }

    /// Find best chord
    int best_idx = 0;
    int best_votes = votes[0];
    for (int i = 1; i < StreamAnalyzer::kBarVoteSlots; ++i) {
      if (votes[i] > best_votes) {
        best_votes = votes[i];
        best_idx = i;
      }
    }

    int best_root = best_idx / kNumChordQualities;
    int best_quality = best_idx % kNumChordQualities;
    float confidence = (vote_count > 0) ? static_cast<float>(best_votes) / vote_count : 0.0f;

    /// Create bar chord entry
    BarChord bc;
    bc.bar_index = bar;
    bc.root = best_root;
    bc.quality = best_quality;
    bc.start_time = history_start_sec + bar * bar_duration_;
    bc.confidence = confidence;
    current_estimate_.bar_chord_progression.push_back(bc);
  }

  /// Update bar index to continue from where retroactive detection ended
  current_bar_index_ = retroactive_bars;
  bar_start_time_ = history_start_sec + retroactive_bars * bar_duration_;

  /// Compute voted pattern from all detected bars
  compute_voted_pattern(4);

  /// Detect best matching progression pattern
  detect_progression_pattern();
}

void StreamAnalyzer::compute_voted_pattern(int pattern_length) {
  // Skip if pattern is already locked (detected with high confidence)
  if (pattern_locked_) {
    return;
  }

  const auto& bars = current_estimate_.bar_chord_progression;
  if (bars.empty() || pattern_length <= 0) {
    return;
  }

  current_estimate_.pattern_length = pattern_length;
  current_estimate_.voted_pattern.clear();
  current_estimate_.voted_pattern.resize(pattern_length);

  /// For each position in the pattern, vote across all repetitions
  for (int pos = 0; pos < pattern_length; ++pos) {
    /// Collect votes: chord index -> (total confidence, count).
    /// Sized to (12 pitch classes * kNumChordQualities); see kBarVoteSlots.
    std::array<float, StreamAnalyzer::kBarVoteSlots> confidence_sum = {};
    std::array<int, StreamAnalyzer::kBarVoteSlots> vote_count = {};

    /// Go through all bars at this pattern position
    for (size_t bar_idx = pos; bar_idx < bars.size(); bar_idx += pattern_length) {
      const auto& bar = bars[bar_idx];
      int chord_idx = bar.root * kNumChordQualities + bar.quality;
      if (chord_idx >= 0 && chord_idx < StreamAnalyzer::kBarVoteSlots) {
        confidence_sum[chord_idx] += bar.confidence;
        ++vote_count[chord_idx];
      }
    }

    /// Find chord with highest weighted vote (confidence-weighted)
    /// Apply diatonic chord bonus based on detected key.
    int detected_key = current_estimate_.key;
    const auto diatonic_chords = diatonic_triads(current_estimate_.key_minor);

    int best_idx = 0;
    float best_score = 0.0f;
    int total_votes = 0;

    for (int i = 0; i < StreamAnalyzer::kBarVoteSlots; ++i) {
      total_votes += vote_count[i];

      float score = confidence_sum[i];
      if (score < 0.01f) continue;

      /// Apply diatonic bonus: +15% if chord is diatonic to detected key
      int chord_root = i / kNumChordQualities;
      int chord_quality = i % kNumChordQualities;
      int relative_root = (chord_root - detected_key + 12) % 12;

      for (const auto& [diatonic_degree, diatonic_quality] : diatonic_chords) {
        if (relative_root == diatonic_degree && chord_quality == diatonic_quality) {
          score *= 1.15f;  // 15% bonus for diatonic chords
          break;
        }
      }

      if (score > best_score) {
        best_score = score;
        best_idx = i;
      }
    }

    /// Create voted pattern entry
    BarChord& voted = current_estimate_.voted_pattern[pos];
    voted.bar_index = pos;
    voted.root = best_idx / kNumChordQualities;
    voted.quality = best_idx % kNumChordQualities;
    voted.start_time = 0.0f;  ///< Not meaningful for pattern

    /// Confidence = ratio of votes for this chord
    int votes_for_best = vote_count[best_idx];
    voted.confidence = (total_votes > 0) ? static_cast<float>(votes_for_best) / total_votes : 0.0f;
  }

  /// Try to correct voted pattern using known progression patterns
  correct_voted_pattern_by_known_patterns();
}

void StreamAnalyzer::correct_voted_pattern_by_known_patterns() {
  auto& voted = current_estimate_.voted_pattern;
  if (voted.size() < 4) return;

  // Calculate minimum bars needed before locking
  // If expected duration is known, use 25% of expected total bars
  // Otherwise, require at least 2 full repetitions (8 bars for 4-bar pattern)
  const auto& bars = current_estimate_.bar_chord_progression;
  int pattern_len = static_cast<int>(voted.size());
  int min_bars_for_lock;

  if (expected_duration_ > 0.0f && bar_duration_ > 0.0f) {
    int expected_total_bars = static_cast<int>(expected_duration_ / bar_duration_);
    // Lock after 25% of song, but at least 2 repetitions
    min_bars_for_lock = std::max(pattern_len * 2, expected_total_bars / 4);
  } else {
    // Default: 2 full repetitions
    min_bars_for_lock = pattern_len * 2;
  }

  bool can_lock = (static_cast<int>(bars.size()) >= min_bars_for_lock);

  const auto& patterns = known_progression_patterns();
  int detected_key = current_estimate_.key;
  int pattern_length = static_cast<int>(voted.size());

  /// Find best matching known pattern
  std::string best_pattern_name;
  float best_match_score = 0.0f;
  std::vector<std::pair<int, int>> best_correction;  // position -> (new_root, new_quality)

  for (const auto& pattern : patterns) {
    int known_len = static_cast<int>(pattern.chords.size());
    if (known_len != pattern_length) continue;  // Only match same-length patterns

    int exact_matches = 0;
    int confusable_matches = 0;
    std::vector<std::pair<int, int>> corrections;

    for (int pos = 0; pos < pattern_length; ++pos) {
      int voted_root = voted[pos].root;
      int voted_quality = voted[pos].quality;

      // Expected chord from known pattern (relative to detected key)
      int expected_degree = pattern.chords[pos].first;
      int expected_quality = pattern.chords[pos].second;
      int expected_root = (detected_key + expected_degree) % 12;

      if (voted_root == expected_root && voted_quality == expected_quality) {
        ++exact_matches;
      } else if (are_chords_confusable(voted_root, voted_quality, expected_root,
                                       expected_quality)) {
        ++confusable_matches;
        corrections.emplace_back(pos, expected_root * kNumChordQualities + expected_quality);
      }
    }

    // Score: exact matches count fully, confusable matches count partially
    float score = (exact_matches + confusable_matches * 0.7f) / pattern_length;

    // Require at least (n-1) positions to match (exact or confusable)
    // For 4-chord pattern: need 3+ matches
    int total_matches = exact_matches + confusable_matches;
    if (total_matches >= pattern_length - 1 && score > best_match_score) {
      best_match_score = score;
      best_pattern_name = pattern.name;
      best_correction = corrections;
    }
  }

  // Apply correction if we found a good match (at least 75% match score)
  if (best_match_score >= 0.75f && !best_correction.empty()) {
    for (const auto& [pos, chord_idx] : best_correction) {
      /// chord_idx was encoded as expected_root * kNumChordQualities +
      /// expected_quality (see the corrections.emplace_back above), so it MUST
      /// be decoded with kNumChordQualities — not a hardcoded 4. Using /4 / %4
      /// corrupted every pattern-corrected chord whose encoding spanned more
      /// than four qualities.
      voted[pos].root = chord_idx / kNumChordQualities;
      voted[pos].quality = chord_idx % kNumChordQualities;
      // Keep original confidence but mark as corrected
      // (confidence remains from voting, but chord is corrected)
    }

    // Also set the detected pattern name based on the correction
    current_estimate_.detected_pattern_name = best_pattern_name;
    current_estimate_.detected_pattern_score = best_match_score;

    // Lock the pattern only if we have enough data (4+ repetitions)
    if (can_lock) {
      pattern_locked_ = true;
    }
  }
}

void StreamAnalyzer::detect_progression_pattern() {
  // Skip if pattern is already locked
  if (pattern_locked_) {
    return;
  }

  const auto& bars = current_estimate_.bar_chord_progression;
  if (bars.size() < 4) {
    return;
  }

  const auto& patterns = known_progression_patterns();
  int detected_key = current_estimate_.key;

  std::string best_pattern_name;
  float best_score = 0.0f;
  current_estimate_.all_pattern_scores.clear();

  for (const auto& pattern : patterns) {
    int pattern_len = static_cast<int>(pattern.chords.size());
    if (pattern_len == 0) continue;

    /// Accumulate score across all repetitions of this pattern
    float total_score = 0.0f;
    float max_possible = 0.0f;

    for (size_t bar_idx = 0; bar_idx < bars.size(); ++bar_idx) {
      int pos = bar_idx % pattern_len;
      const auto& expected = pattern.chords[pos];

      /// Expected chord (relative to detected key)
      int expected_root = (detected_key + expected.first) % 12;
      int expected_quality = expected.second;

      /// Get detected bar chord
      const auto& bar = bars[bar_idx];
      float bar_conf = bar.confidence;

      const float similarity =
          chord_similarity_score(bar.root, bar.quality, expected_root, expected_quality);

      /// Weight by detection confidence
      total_score += similarity * bar_conf;
      max_possible += bar_conf;
    }

    float score = (max_possible > 0.0f) ? total_score / max_possible : 0.0f;

    /// Store all pattern scores
    current_estimate_.all_pattern_scores.emplace_back(pattern.name, score);

    if (score > best_score) {
      best_score = score;
      best_pattern_name = pattern.name;
    }
  }

  /// Sort by score descending
  std::sort(current_estimate_.all_pattern_scores.begin(),
            current_estimate_.all_pattern_scores.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  /// Only report pattern if score meets minimum threshold (75%)
  /// A low score means the pattern is a poor match, even if it's the "best" among patterns
  constexpr float kMinPatternScore = 0.75f;
  if (best_score >= kMinPatternScore) {
    current_estimate_.detected_pattern_name = best_pattern_name;
    current_estimate_.detected_pattern_score = best_score;
  } else if (current_estimate_.detected_pattern_name.empty()) {
    // Only clear if not already set by pattern-based correction
    current_estimate_.detected_pattern_score = best_score;
  }
}

}  // namespace sonare
