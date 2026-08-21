#pragma once

/// @file mix_eval.h
/// @brief Objective evaluation helpers for the mixing assistant's suggestions.
///
/// @warning **Test-only.** This header must not be promoted to `tools/`, to a
///          public header, or to any shipped surface, and nothing here may be
///          given a `sonare_mixing_assistant_*` name.
///
///          It deliberately does the one thing the shipped API refuses to do:
///          run analysis, suggestion, application and rendering in a single
///          call. Keeping suggestion and application separate is a contract of
///          the public API — a mix has no single right answer, so nothing is
///          applied on the user's behalf. Measuring whether a suggestion
///          actually improved anything requires closing that loop, so this
///          helper closes it *inside the test binary only*. It being convenient
///          is not a reason to export it.
///
/// @details What is measured here is deliberately narrow. Whether a mix sounds
///          good is subjective and has no place in a regression suite. Only
///          quantities with an unambiguous direction are computed, and even
///          those are recorded rather than gated except where they are true
///          invariants.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "core/audio.h"
#include "mastering/common/loudness_measure.h"
#include "mixing/api/scene.h"
#include "mixing/assistant/masking.h"
#include "mixing/assistant/mix_profile.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"
#include "util/constants.h"

namespace sonare::mixing::assistant::test {

/// @brief One synthetic multi-track fixture.
/// @details Synthetic rather than recorded: a real multitrack stem set would be
///          a licensing and repository-size problem, and a signal whose band
///          placement is known by construction lets the expected interference be
///          computed rather than eyeballed.
struct SyntheticTracks {
  std::vector<std::vector<float>> left;
  std::vector<std::vector<float>> right;  ///< Empty vector for a mono track.
  std::vector<std::string> ids;
  std::vector<std::string> names;
  int sample_rate = 48000;

  std::vector<TrackInput> inputs() const {
    std::vector<TrackInput> tracks;
    tracks.reserve(left.size());
    for (std::size_t index = 0; index < left.size(); ++index) {
      TrackInput track;
      track.id = ids[index];
      track.name = index < names.size() ? names[index] : std::string();
      track.left = left[index].data();
      track.right = right[index].empty() ? nullptr : right[index].data();
      track.frame_count = left[index].size();
      track.sample_rate = sample_rate;
      tracks.push_back(track);
    }
    return tracks;
  }
};

/// @brief Builds a small multi-track fixture with known band placement.
/// @details Each part occupies a band the assistant's taxonomy recognises, and
///          the levels are deliberately spread so gain staging has something to
///          correct. One deliberately silent track is included: the exclusion
///          path is the thing most likely to break, and a fixture that never
///          exercises it would pass while broken.
/// @param sample_rate Shared sample rate.
/// @param duration_sec Length of every non-silent part.
inline SyntheticTracks make_demo_tracks(int sample_rate = 48000, float duration_sec = 1.2f) {
  const std::size_t frames =
      static_cast<std::size_t>(static_cast<float>(sample_rate) * duration_sec);
  SyntheticTracks fixture;
  fixture.sample_rate = sample_rate;

  // Deterministic pseudo-noise. Not std::rand: the fixture has to produce the
  // same bytes on every run for a golden to mean anything.
  std::uint32_t state = 0x13572468u;
  auto next_noise = [&state]() {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<double>(state) / 4294967295.0) * 2.0f - 1.0f;
  };

  auto add_mono = [&](const std::string& id, const std::string& name, std::vector<float> samples) {
    fixture.ids.push_back(id);
    fixture.names.push_back(name);
    fixture.left.push_back(std::move(samples));
    fixture.right.emplace_back();
  };

  auto seconds = [&](std::size_t frame) {
    return static_cast<float>(frame) / static_cast<float>(sample_rate);
  };

  // Kick: everything under 130 Hz, gone almost as soon as it arrives.
  std::vector<float> kick(frames, 0.0f);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const float t = seconds(frame);
    const float phase = std::fmod(t, 0.5f);
    const float envelope = std::exp(-28.0f * phase);
    kick[frame] = 0.8f * envelope * std::sin(constants::kTwoPi * 55.0f * t);
  }
  add_mono("kick", "Kick In", kick);

  // Bass: the same register held rather than struck, and tonal. Overlaps the
  // kick's band on purpose so the dominance measure has a real pair to report.
  std::vector<float> bass(frames, 0.0f);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const float t = seconds(frame);
    bass[frame] = 0.25f * (std::sin(constants::kTwoPi * 82.0f * t) +
                           0.3f * std::sin(constants::kTwoPi * 164.0f * t));
  }
  add_mono("bass", "Bass DI", bass);

  // Voice: a held harmonic series with a weak fundamental and its energy in the
  // 500-2000 Hz band, rolling off before the cymbal region. The partial weights
  // are what put the centroid and the rolloff where a voice's are; they are not
  // an attempt to sound like one.
  const float kVoicePartials[] = {0.30f, 0.70f, 0.90f, 0.85f, 0.80f, 0.70f, 0.60f,
                                  0.55f, 0.50f, 0.45f, 0.40f, 0.35f, 0.30f, 0.25f};
  std::vector<float> vox(frames, 0.0f);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const float t = seconds(frame);
    float sample = 0.0f;
    for (std::size_t partial = 0; partial < std::size(kVoicePartials); ++partial) {
      const float hz = 180.0f * static_cast<float>(partial + 1);
      sample += kVoicePartials[partial] * std::sin(constants::kTwoPi * hz * t);
    }
    vox[frame] = 0.045f * sample;
  }
  add_mono("vox", "Lead Vox", vox);

  // Guitar pair: plucked, so partly decayed rather than held, with enough
  // high-mid content for the pick attack to register. Double-tracked left and
  // right at slightly different pitches, which is both how the part is really
  // recorded and what gives the mono-compatibility path a subject.
  auto pluck = [&](float root_hz, float phase_offset) {
    std::vector<float> out(frames, 0.0f);
    for (std::size_t frame = 0; frame < frames; ++frame) {
      const float t = seconds(frame);
      const float envelope = std::exp(-16.0f * std::fmod(t, 0.25f));
      float sample = 0.0f;
      for (int partial = 1; partial <= 12; ++partial) {
        const float hz = root_hz * static_cast<float>(partial);
        // 1/sqrt(n) rather than 1/n: a plucked string keeps enough upper
        // partials for the pick attack to show as high-mid content, which is
        // the feature that separates it from a held note.
        sample += (1.0f / std::sqrt(static_cast<float>(partial))) *
                  std::sin(constants::kTwoPi * hz * t + phase_offset);
      }
      out[frame] = 0.07f * envelope * sample;
    }
    return out;
  };
  fixture.ids.push_back("gtr");
  fixture.names.push_back("Gtr Double");
  fixture.left.push_back(pluck(330.0f, 0.0f));
  fixture.right.push_back(pluck(331.7f, 1.1f));

  // Hi-hat: bright noise, short, and repeated often. The decay is tight enough
  // that the burst is over well before the next one, which is the whole
  // difference from a cymbal.
  std::vector<float> hats(frames, 0.0f);
  // First-difference high-pass, twice, so the burst sits above 6 kHz. The
  // filter state is a local, not a static: a static would carry over between
  // calls and the fixture would stop being reproducible.
  float previous_noise = 0.0f;
  float previous_filtered = 0.0f;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const float t = seconds(frame);
    const float envelope = std::exp(-900.0f * std::fmod(t, 0.25f));
    const float sample = next_noise();
    const float once = sample - previous_noise;
    previous_noise = sample;
    const float twice = once - previous_filtered;
    previous_filtered = once;
    hats[frame] = 0.20f * envelope * twice;
  }
  add_mono("hats", "HiHat", hats);

  // Digital silence. Must be excluded rather than driven to the loudness target.
  add_mono("silent", "Unused", std::vector<float>(frames, 0.0f));

  return fixture;
}

/// @brief Objective metrics for a set of tracks under a proposed scene.
struct MixEvaluation {
  /// @brief Spread of per-track integrated loudness after the suggested trim,
  ///        in dB. Expected to shrink.
  /// @details Nearly tautological once gain staging normalises to an absolute
  ///          target, so this is a break detector — a silent track that was not
  ///          excluded, a clamp that never engaged — rather than evidence of
  ///          improvement.
  float track_loudness_spread_db = 0.0f;

  /// @brief Total band dominance above parity, summed over ordered track pairs.
  /// @details Uses the same energy-ratio measure the assistant itself uses. A
  ///          loudness-difference measure is not substituted here on the grounds
  ///          that this is only a test: test code ships in the repository and
  ///          carries the same constraints as the implementation.
  float total_dominance = 0.0f;

  /// @brief Number of tracks flagged as at risk under a mono fold.
  int mono_risk_count = 0;

  /// @brief True peak of the rendered master, in dBTP.
  float master_true_peak_dbtp = constants::kFloorDb;

  /// @brief Whether the scene survives a JSON round trip unchanged.
  bool scene_round_trips = false;

  /// @brief Whether the scene could be instantiated and rendered at all.
  bool rendered = false;
};

/// @brief Sums the tracks straight to a stereo bus with no processing.
/// @details The reference the suggested mix is compared against.
inline void sum_tracks(const std::vector<TrackInput>& tracks, std::vector<float>& out_left,
                       std::vector<float>& out_right) {
  std::size_t longest = 0;
  for (const auto& track : tracks) longest = std::max(longest, track.frame_count);
  out_left.assign(longest, 0.0f);
  out_right.assign(longest, 0.0f);
  for (const auto& track : tracks) {
    if (track.left == nullptr) continue;
    for (std::size_t frame = 0; frame < track.frame_count; ++frame) {
      out_left[frame] += track.left[frame];
      out_right[frame] += track.right != nullptr ? track.right[frame] : track.left[frame];
    }
  }
}

/// @brief Renders the tracks through the scene and returns the master output.
/// @details Goes through the public C ABI on purpose, so the harness exercises
///          the same path a caller would: serialise the scene, hand it to the
///          mixer, feed the tracks. Returns false when the scene cannot be
///          instantiated.
inline bool render_scene(const std::vector<TrackInput>& tracks, const api::Scene& scene,
                         std::vector<float>& out_left, std::vector<float>& out_right,
                         int max_block_size = 1024) {
  if (tracks.empty()) return false;
  const std::string json = api::scene_to_json(scene);
  SonareMixer* mixer =
      sonare_mixer_from_scene_json(json.c_str(), tracks.front().sample_rate, max_block_size);
  if (mixer == nullptr) return false;

  std::size_t longest = 0;
  for (const auto& track : tracks) longest = std::max(longest, track.frame_count);

  // Every strip needs a channel pointer for the whole render, so short tracks
  // are padded with silence rather than dropped part way through.
  std::vector<std::vector<float>> padded_left(tracks.size());
  std::vector<std::vector<float>> padded_right(tracks.size());
  std::vector<const float*> left_ptrs(tracks.size(), nullptr);
  std::vector<const float*> right_ptrs(tracks.size(), nullptr);
  for (std::size_t index = 0; index < tracks.size(); ++index) {
    padded_left[index].assign(longest, 0.0f);
    padded_right[index].assign(longest, 0.0f);
    const TrackInput& track = tracks[index];
    for (std::size_t frame = 0; frame < track.frame_count; ++frame) {
      const float sample = track.left != nullptr ? track.left[frame] : 0.0f;
      padded_left[index][frame] = sample;
      padded_right[index][frame] = track.right != nullptr ? track.right[frame] : sample;
    }
  }

  out_left.assign(longest, 0.0f);
  out_right.assign(longest, 0.0f);
  bool ok = true;
  for (std::size_t offset = 0; offset < longest;
       offset += static_cast<std::size_t>(max_block_size)) {
    const std::size_t block = std::min(static_cast<std::size_t>(max_block_size), longest - offset);
    for (std::size_t index = 0; index < tracks.size(); ++index) {
      left_ptrs[index] = padded_left[index].data() + offset;
      right_ptrs[index] = padded_right[index].data() + offset;
    }
    const SonareError err =
        sonare_mixer_process_stereo(mixer, left_ptrs.data(), right_ptrs.data(), tracks.size(),
                                    out_left.data() + offset, out_right.data() + offset, block);
    if (err != SONARE_OK) {
      ok = false;
      break;
    }
  }
  sonare_mixer_destroy(mixer);
  return ok;
}

/// @brief Population standard deviation, or 0 for fewer than two values.
inline float spread_db(const std::vector<float>& values) {
  if (values.size() < 2) return 0.0f;
  double mean = 0.0;
  for (float value : values) mean += value;
  mean /= static_cast<double>(values.size());
  double variance = 0.0;
  for (float value : values) {
    const double delta = static_cast<double>(value) - mean;
    variance += delta * delta;
  }
  variance /= static_cast<double>(values.size());
  return static_cast<float>(std::sqrt(variance));
}

/// @brief Total dominance above parity across every ordered pair and band.
inline float total_dominance(const MixProfile& mix) {
  float total = 0.0f;
  for (int masker = 0; masker < mix.track_count; ++masker) {
    for (int maskee = 0; maskee < mix.track_count; ++maskee) {
      if (masker == maskee) continue;
      for (int band = 0; band < kBandCount; ++band) {
        const BandDominance entry = mix.dominance_at(masker, maskee, band);
        if (entry.valid_frames == 0) continue;
        total += std::max(0.0f, entry.ratio - 0.5f);
      }
    }
  }
  return total;
}

/// @brief Evaluates a suggestion end to end.
/// @param tracks Input tracks.
/// @param result What the assistant returned for them.
inline MixEvaluation evaluate(const std::vector<TrackInput>& tracks,
                              const MixAssistantResult& result) {
  MixEvaluation evaluation;

  // Post-trim loudness: the measured value plus the suggested trim and fader.
  // Re-rendering each strip in isolation would measure the same thing far more
  // slowly, and the trim is a static gain by construction.
  std::vector<float> staged;
  for (const auto& profile : result.tracks) {
    if (!profile.usable) continue;
    float offset = 0.0f;
    for (const auto& strip : result.scene.strips) {
      if (strip.id != profile.strip_id) continue;
      offset = strip.input_trim_db + strip.fader_db;
      break;
    }
    staged.push_back(profile.base.loudness.integrated_lufs + offset);
  }
  evaluation.track_loudness_spread_db = spread_db(staged);
  evaluation.total_dominance = total_dominance(result.mix);
  evaluation.mono_risk_count = static_cast<int>(result.mix.mono_risks.size());

  const api::Scene round_tripped = api::scene_from_json(api::scene_to_json(result.scene));
  evaluation.scene_round_trips =
      api::scene_to_json(round_tripped) == api::scene_to_json(result.scene);

  std::vector<float> left;
  std::vector<float> right;
  evaluation.rendered = render_scene(tracks, result.scene, left, right);
  if (evaluation.rendered && !left.empty()) {
    std::vector<float> interleaved(left.size() * 2, 0.0f);
    for (std::size_t frame = 0; frame < left.size(); ++frame) {
      interleaved[frame * 2] = left[frame];
      interleaved[frame * 2 + 1] = right[frame];
    }
    const auto summary = mastering::common::measure_loudness_summary_interleaved(
        interleaved.data(), left.size(), 2, tracks.front().sample_rate);
    evaluation.master_true_peak_dbtp = summary.true_peak_dbtp;
  }
  return evaluation;
}

}  // namespace sonare::mixing::assistant::test
