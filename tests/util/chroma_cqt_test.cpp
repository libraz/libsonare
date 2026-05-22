/// @file chroma_cqt_test.cpp
/// @brief Smoke tests for chroma_cqt / chroma_cens (no librosa parity).

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "feature/chroma.h"
#include "util/constants.h"

using namespace sonare;

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
  Audio audio = make_c_major_chord(22050, 1.0f);
  ChromaCqtConfig cfg;
  Chroma c = chroma_cqt(audio, cfg);
  REQUIRE(c.n_chroma() == 12);
  REQUIRE(c.n_frames() > 0);
}

TEST_CASE("chroma_cqt normalized values are in [0, 1]", "[chroma_cqt][unit][smoke]") {
  Audio audio = make_c_major_chord(22050, 1.0f);
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
  Audio audio = make_c_major_chord(22050, 1.0f);
  ChromaCensConfig cfg;
  Chroma c = chroma_cens(audio, cfg);
  REQUIRE(c.n_chroma() == 12);
  REQUIRE(c.n_frames() > 0);
}
