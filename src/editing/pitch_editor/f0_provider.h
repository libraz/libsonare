#pragma once

/// @file f0_provider.h
/// @brief F0 provider abstraction for monophonic pitch editing.
///
/// Core-internal: the F0 provider (and its pYIN implementation) backs the
/// pitch-editing pipeline and is not exposed on any public surface (C ABI /
/// Node / Python / WASM). Callers reach it indirectly through the pitch-shift /
/// pitch-correction entry points.

#include <cmath>
#include <vector>

#include "core/audio.h"
#include "feature/pitch.h"

namespace sonare::editing::pitch_editor {

struct F0Track {
  std::vector<float> f0_hz;
  /// Per-frame voiced probability in [0, 1], as pYIN defines it: the frame's
  /// voiced observation mass. It depends on how many periods of the pitch fit
  /// in the analysis frame, so for a fixed frame_length it rises with F0 even
  /// when the signal quality is unchanged. It is therefore NOT a signal-quality
  /// confidence and NOT a correction weight; consumers that need a voicing
  /// decision must read `voiced`. Optional: an empty vector is allowed
  /// everywhere `voiced` is populated.
  std::vector<float> voiced_prob;
  /// Per-frame voicing decision. This is the authoritative voiced/unvoiced
  /// flag for every consumer (pitch correction, note segmentation).
  std::vector<bool> voiced;
  int hop_length = 512;
  int sample_rate = 48000;
  /// Optional direct frame cadence for host-supplied F0 tracks. Zero keeps the
  /// legacy sample_rate / hop_length derivation used by internal providers.
  /// Read it through @ref frame_rate / @ref samples_per_frame rather than
  /// directly: those two are the single cadence rule, and a consumer that
  /// reaches past them converts one track's frames at a different rate than its
  /// neighbour does.
  float frame_rate_hz = 0.0f;

  int n_frames() const noexcept { return static_cast<int>(f0_hz.size()); }

  /// Frame cadence in Hz. Returns 0 for a track that carries neither an explicit
  /// cadence nor a usable sample_rate / hop_length pair.
  float frame_rate() const noexcept {
    if (frame_rate_hz > 0.0f && std::isfinite(frame_rate_hz)) return frame_rate_hz;
    if (sample_rate <= 0 || hop_length <= 0) return 0.0f;
    return static_cast<float>(sample_rate) / static_cast<float>(hop_length);
  }

  /// Samples per analysis frame under that cadence -- the frame-to-sample half
  /// of the same rule. With no explicit cadence this is exactly hop_length, so
  /// an internally produced track converts identically to before. Double, not
  /// float: a hop near INT_MAX/2 does not survive a float round trip, and the
  /// sample offsets derived from it are compared against the hop exactly.
  double samples_per_frame() const noexcept {
    if (frame_rate_hz > 0.0f && std::isfinite(frame_rate_hz) && sample_rate > 0) {
      return static_cast<double>(sample_rate) / static_cast<double>(frame_rate_hz);
    }
    return static_cast<double>(hop_length > 0 ? hop_length : 0);
  }
};

class F0Provider {
 public:
  virtual ~F0Provider() = default;
  virtual F0Track detect(const Audio& audio) = 0;
};

class PyinF0Provider final : public F0Provider {
 public:
  explicit PyinF0Provider(PitchConfig config = {});
  F0Track detect(const Audio& audio) override;
  const PitchConfig& config() const noexcept { return config_; }

 private:
  PitchConfig config_{};
};

}  // namespace sonare::editing::pitch_editor
