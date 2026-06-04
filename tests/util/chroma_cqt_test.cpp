/// @file chroma_cqt_test.cpp
/// @brief Smoke + default-normalization tests for chroma_cqt / chroma_cens.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "feature/chroma.h"
#include "util/constants.h"

using namespace sonare;
using namespace sonare::constants;

namespace {

Audio make_c_major_chord(int sr, float duration) {
  const size_t n = static_cast<size_t>(duration * sr);
  std::vector<float> y(n);
  const double tp = static_cast<double>(constants::kTwoPi);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(sr);
    y[i] = static_cast<float>(
        (std::sin(tp * 261.63 * t) + std::sin(tp * 329.63 * t) + std::sin(tp * 392.0 * t)) / 3.0);
  }
  return Audio::from_vector(std::move(y), sr);
}

}  // namespace

TEST_CASE("chroma_cqt produces 12-bin output", "[chroma_cqt][unit][smoke]") {
  Audio audio = make_c_major_chord(22050, 0.5f);
  ChromaCqtConfig cfg;
  Chroma c = chroma_cqt(audio, cfg);
  REQUIRE(c.n_chroma() == 12);
  REQUIRE(c.n_frames() > 0);
}

TEST_CASE("chroma_cqt normalized values are in [0, 1]", "[chroma_cqt][unit][smoke]") {
  Audio audio = make_c_major_chord(22050, 0.5f);
  ChromaCqtConfig cfg;
  cfg.normalize_frames = true;
  Chroma c = chroma_cqt(audio, cfg);
  const float* data = c.data();
  const size_t n = static_cast<size_t>(c.n_chroma() * c.n_frames());
  for (size_t i = 0; i < n; ++i) {
    REQUIRE(data[i] >= 0.0f);
    REQUIRE(data[i] <= 1.0f + 1e-5f);
  }
}

TEST_CASE("chroma_cens produces 12-bin output", "[chroma_cqt][unit][smoke]") {
  Audio audio = make_c_major_chord(22050, 0.5f);
  ChromaCensConfig cfg;
  Chroma c = chroma_cens(audio, cfg);
  REQUIRE(c.n_chroma() == 12);
  REQUIRE(c.n_frames() > 0);
}

TEST_CASE("chroma_cqt frames are L-inf-normalized by default",
          "[chroma_cqt][unit][normalize][librosa-parity]") {
  // librosa.feature.chroma_cqt defaults to norm=np.inf — each frame's
  // max magnitude must be 1 (or the frame is all-zero). Cf. CLAUDE.md
  // librosa-parity rule for defaults.
  Audio audio = make_c_major_chord(22050, 0.5f);
  ChromaCqtConfig cfg;
  cfg.normalize_frames = true;
  Chroma c = chroma_cqt(audio, cfg);

  REQUIRE(c.n_chroma() == 12);
  REQUIRE(c.n_frames() > 0);

  const float* data = c.data();
  const int n_chroma = c.n_chroma();
  const int n_frames = c.n_frames();

  for (int t = 0; t < n_frames; ++t) {
    float max_abs = 0.0f;
    for (int cidx = 0; cidx < n_chroma; ++cidx) {
      max_abs = std::max(max_abs, std::abs(data[cidx * n_frames + t]));
    }
    // Either unit L-inf norm (active frame) or zero (silent frame).
    REQUIRE((max_abs < 1e-6f || std::abs(max_abs - 1.0f) < 1e-4f));
  }
}
