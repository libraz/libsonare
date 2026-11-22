/// @file tempo_from_audio_test.cpp
/// @brief Tempo candidates estimated from audio read the beats, meter and downbeats the full music
///        analysis reports for the same buffer, at input rates above the analysis rate.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "analysis/analysis_rate.h"
#include "analysis/music_analyzer.h"
#include "core/audio.h"
#include "mir/tempo_estimator_bridge.h"
#include "util/constants.h"

using namespace sonare;

namespace {

constexpr double kDurationS = 12.0;
constexpr double kClickHz = 1000.0;
constexpr double kClickDecayS = 0.0027;
constexpr float kClickAmp = 0.5f;

/// @brief Equal decaying 1 kHz clicks every beat from @p first_s: no accent, so the meter rests on
///        the analysis geometry rather than on the signal.
Audio unaccented_clicks(int sr, double bpm, double first_s) {
  std::vector<float> x(static_cast<size_t>(kDurationS * sr), 0.0f);
  const double period = 60.0 / bpm;
  const int click_len = sr / 50;
  for (double t = first_s; t < kDurationS - period; t += period) {
    const size_t s = static_cast<size_t>(std::llround(t * sr));
    for (int i = 0; i < click_len && s + i < x.size(); ++i) {
      x[s + i] += kClickAmp * static_cast<float>(std::exp(-i / (kClickDecayS * sr)) *
                                                 std::sin(constants::kTwoPiD * kClickHz * i / sr));
    }
  }
  return Audio::from_vector(std::move(x), sr);
}

}  // namespace

TEST_CASE("Tempo from audio and the full analysis agree on beats and meter above 22050 Hz",
          "[mir][tempo_from_audio]") {
  for (int sr : {44100, 48000}) {
    for (double bpm : {100.0, 128.0}) {
      INFO("sr = " << sr << ", bpm = " << bpm);
      const Audio audio = unaccented_clicks(sr, bpm, 0.13);

      MusicAnalyzer music(audio);
      const BeatAnalyzer& full = music.beat_analyzer();
      const BeatAnalyzer direct(analysis_rate_audio(audio));
      REQUIRE(direct.beats().size() == full.beats().size());
      CHECK(direct.time_signature().numerator == full.time_signature().numerator);
      CHECK(direct.downbeat_indices() == full.downbeat_indices());

      const auto from_audio = mir::estimate_tempo_from_audio(audio);
      const auto from_full = mir::estimate_tempo(mir::make_input_from_analyzer(full));
      REQUIRE(from_audio.size() == from_full.size());
      for (size_t i = 0; i < from_audio.size(); ++i) {
        INFO("candidate " << i);
        REQUIRE(from_audio[i].time_sigs.size() == from_full[i].time_sigs.size());
        for (size_t k = 0; k < from_audio[i].time_sigs.size(); ++k) {
          const auto& a = from_audio[i].time_sigs[k];
          const auto& b = from_full[i].time_sigs[k];
          CHECK(a.time_sig.numerator == b.time_sig.numerator);
          CHECK(a.time_sig.denominator == b.time_sig.denominator);
          CHECK(a.start_ppq == Catch::Approx(b.start_ppq).margin(1e-6));
        }
        REQUIRE(from_audio[i].segments.size() == from_full[i].segments.size());
        for (size_t k = 0; k < from_audio[i].segments.size(); ++k) {
          CHECK(from_audio[i].segments[k].bpm ==
                Catch::Approx(from_full[i].segments[k].bpm).epsilon(1e-6));
        }
      }
      CHECK(from_audio.front().time_sigs.front().time_sig.numerator ==
            music.analyze().time_signature.numerator);
    }
  }
}
