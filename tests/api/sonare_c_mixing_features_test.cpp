/// @file sonare_c_mixing_features_test.cpp
/// @brief Mixing and feature edge-case C API tests.

#include "sonare_c_test_helpers.h"

TEST_CASE("sonare_strip_schedule_send_automation error mapping", "[c_api][mixing]") {
  SonareMixer* mixer = sonare_mixer_create(48000, 512);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "src");
  REQUIRE(strip != nullptr);

  size_t send_index = 0;
  REQUIRE(sonare_strip_add_send(strip, "send0", "bus0", -6.0f, 0, &send_index) == SONARE_OK);

  SECTION("out-of-range send_index -> INVALID_PARAMETER") {
    // A bad argument must be reported distinctly from a capacity condition.
    REQUIRE(sonare_strip_schedule_send_automation(strip, send_index + 1, 0, -3.0f, 0) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("full send lane -> OUT_OF_MEMORY") {
    // Fill the lane to its ring-buffer capacity (default 1024 usable slots).
    // Use a non-decreasing sample_pos so push() is not rejected for ordering.
    SonareError err = SONARE_OK;
    int pushed = 0;
    for (int i = 0; i < 100000; ++i) {
      err = sonare_strip_schedule_send_automation(strip, send_index, i, -3.0f, 0);
      if (err != SONARE_OK) {
        break;
      }
      ++pushed;
    }
    // The lane should accept many events, then fail with OUT_OF_MEMORY (capacity)
    // rather than INVALID_PARAMETER, mirroring the fader/pan/width schedulers.
    REQUIRE(pushed > 0);
    REQUIRE(err == SONARE_ERROR_OUT_OF_MEMORY);
  }

  sonare_mixer_destroy(mixer);
}

TEST_CASE("sonare_metering stereo pair validates both channels", "[c_api][mixing]") {
  const int sr = 48000;
  auto left = generate_sine(440.0f, sr, 0.25f);
  std::vector<float> right = left;

  SECTION("right channel NaN is rejected") {
    std::vector<float> bad_right = right;
    bad_right[bad_right.size() / 2] = std::numeric_limits<float>::quiet_NaN();
    float c = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(left.data(), bad_right.data(), left.size(), sr,
                                               &c) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("right channel Inf is rejected") {
    std::vector<float> bad_right = right;
    bad_right[0] = std::numeric_limits<float>::infinity();
    float c = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(left.data(), bad_right.data(), left.size(), sr,
                                               &c) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("left channel NaN is rejected (parity)") {
    std::vector<float> bad_left = left;
    bad_left[bad_left.size() / 2] = std::numeric_limits<float>::quiet_NaN();
    float c = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(bad_left.data(), right.data(), left.size(), sr,
                                               &c) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mel_spectrogram_ex exposes a custom Mel range from pure C", "[c_api][features]") {
  const int sr = 22050;
  const int n_fft = 1024;
  const int hop = 256;
  const int n_mels = 40;
  auto samples = generate_sine(440.0f, sr, 1.0f);

  SECTION("custom fmin/fmax forward transform round-trips with the inverse API") {
    // The forward _ex transform and the inverse sonare_mel_to_stft now share the
    // same fmin/fmax, so a custom-range round-trip is possible from pure C.
    const float fmin = 100.0f;
    const float fmax = 8000.0f;
    SonareMelResult mel = {};
    REQUIRE(sonare_mel_spectrogram_ex(samples.data(), samples.size(), sr, n_fft, hop, n_mels, fmin,
                                      fmax, 0, &mel) == SONARE_OK);
    REQUIRE(mel.power != nullptr);
    REQUIRE(mel.n_mels == n_mels);
    REQUIRE(mel.n_frames > 0);

    SonareInverseResult stft = {};
    REQUIRE(sonare_mel_to_stft(mel.power, mel.n_mels, mel.n_frames, sr, n_fft, fmin, fmax, &stft) ==
            SONARE_OK);
    REQUIRE(stft.data != nullptr);
    REQUIRE(stft.rows == n_fft / 2 + 1);
    REQUIRE(stft.n_frames == mel.n_frames);

    sonare_free_inverse_result(&stft);
    sonare_free_mel_result(&mel);
  }

  SECTION("a non-default range yields a different forward result than the default") {
    SonareMelResult def = {};
    SonareMelResult ranged = {};
    REQUIRE(sonare_mel_spectrogram(samples.data(), samples.size(), sr, n_fft, hop, n_mels, &def) ==
            SONARE_OK);
    REQUIRE(sonare_mel_spectrogram_ex(samples.data(), samples.size(), sr, n_fft, hop, n_mels,
                                      500.0f, 4000.0f, 0, &ranged) == SONARE_OK);
    const size_t total = static_cast<size_t>(n_mels) * def.n_frames;
    bool differs = false;
    for (size_t i = 0; i < total && !differs; ++i) {
      differs = std::abs(def.power[i] - ranged.power[i]) > 1e-6f;
    }
    REQUIRE(differs);
    sonare_free_mel_result(&def);
    sonare_free_mel_result(&ranged);
  }

  SECTION("sonare_mfcc_ex accepts the range and a null out is rejected") {
    SonareMfccResult mfcc = {};
    REQUIRE(sonare_mfcc_ex(samples.data(), samples.size(), sr, n_fft, hop, n_mels, 13, 100.0f,
                           8000.0f, 0, &mfcc) == SONARE_OK);
    REQUIRE(mfcc.coefficients != nullptr);
    REQUIRE(mfcc.n_mfcc == 13);
    sonare_free_mfcc_result(&mfcc);

    REQUIRE(sonare_mfcc_ex(samples.data(), samples.size(), sr, n_fft, hop, n_mels, 13, 0.0f, 0.0f,
                           0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}
