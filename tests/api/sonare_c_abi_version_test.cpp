/// @file sonare_c_abi_version_test.cpp
/// @brief Aggregate C-ABI version accessor, length-checked inverse transforms,
///        and the uniform non-finite input policy for the compat transforms.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "sonare_c.h"

TEST_CASE("sonare_abi_version mirrors the compile-time aggregate", "[c_api][abi]") {
  REQUIRE(sonare_abi_version() == SONARE_ABI_VERSION);
  REQUIRE(sonare_abi_version() != 0u);
  // The low byte encodes the feature-struct ABI version.
  REQUIRE((sonare_abi_version() & 0xFFu) == SONARE_FEATURE_ABI_VERSION);
}

TEST_CASE("length-checked inverse transforms reject a short input buffer", "[c_api][abi]") {
  const int n_mfcc = 13;
  const int n_frames = 8;
  const int n_mels = 40;
  std::vector<float> mfcc(static_cast<size_t>(n_mfcc) * n_frames, 0.1f);

  SonareInverseResult result{};
  // Correct length succeeds.
  REQUIRE(sonare_mfcc_to_mel_checked(mfcc.data(), mfcc.size(), n_mfcc, n_frames, n_mels, &result) ==
          SONARE_OK);
  sonare_free_inverse_result(&result);

  // A buffer one element too short is rejected instead of over-reading.
  SonareInverseResult bad{};
  REQUIRE(sonare_mfcc_to_mel_checked(mfcc.data(), mfcc.size() - 1, n_mfcc, n_frames, n_mels,
                                     &bad) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("inverse transforms reject non-finite input uniformly", "[c_api][abi]") {
  const int n_frames = 4;
  const int n_mels = 8;
  const int n_mfcc = 5;
  const int sample_rate = 22050;
  const int n_fft = 256;
  const int hop_length = 64;

  std::vector<float> mel(static_cast<size_t>(n_mels) * n_frames, 0.1f);
  mel[3] = std::nanf("");
  SonareInverseResult stft{};
  REQUIRE(sonare_mel_to_stft_checked(mel.data(), mel.size(), n_mels, n_frames, sample_rate, n_fft,
                                     0.0f, 0.0f, &stft) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(stft.data == nullptr);

  float* audio = nullptr;
  size_t audio_len = 0;
  REQUIRE(sonare_mel_to_audio_checked(mel.data(), mel.size(), n_mels, n_frames, sample_rate, n_fft,
                                      hop_length, 0.0f, 0.0f, 2, &audio,
                                      &audio_len) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(audio == nullptr);
  REQUIRE(audio_len == 0);

  std::vector<float> mfcc(static_cast<size_t>(n_mfcc) * n_frames, 0.1f);
  mfcc[7] = INFINITY;
  SonareInverseResult out_mel{};
  REQUIRE(sonare_mfcc_to_mel_checked(mfcc.data(), mfcc.size(), n_mfcc, n_frames, n_mels,
                                     &out_mel) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out_mel.data == nullptr);

  REQUIRE(sonare_mfcc_to_audio_checked(mfcc.data(), mfcc.size(), n_mfcc, n_frames, n_mels,
                                       sample_rate, n_fft, hop_length, 0.0f, 0.0f, 2, &audio,
                                       &audio_len) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(audio == nullptr);
  REQUIRE(audio_len == 0);
}

TEST_CASE("compat transforms reject non-finite input uniformly", "[c_api][abi]") {
  std::vector<float> values = {0.5f, 0.25f, std::nanf(""), 0.1f};
  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_amplitude_to_db(values.data(), values.size(), 1.0f, 1e-5f, 80.0f, &out,
                                 &out_len) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out == nullptr);

  // The same call with all-finite input succeeds.
  std::vector<float> finite = {0.5f, 0.25f, 0.125f, 0.1f};
  REQUIRE(sonare_amplitude_to_db(finite.data(), finite.size(), 1.0f, 1e-5f, 80.0f, &out,
                                 &out_len) == SONARE_OK);
  sonare_free_floats(out);
}
