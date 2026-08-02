/// @file sonare_c_abi_version_test.cpp
/// @brief Aggregate C-ABI version accessor, length-checked inverse transforms,
///        and the uniform non-finite input policy for the compat transforms.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "c_api/sonare_c_error_mapping.h"

TEST_CASE("sonare_abi_version mirrors the compile-time aggregate", "[c_api][abi]") {
  REQUIRE(sonare_abi_version() == SONARE_ABI_VERSION);
  REQUIRE(sonare_abi_version() != 0u);
  // The low byte encodes the feature-struct ABI version.
  REQUIRE((sonare_abi_version() & 0xFFu) == SONARE_FEATURE_ABI_VERSION);
}

TEST_CASE("C error mapping preserves cancellation", "[c_api][abi]") {
  REQUIRE(sonare_c_detail::error_code_from_c_error(SONARE_ERROR_CANCELLED) ==
          sonare::ErrorCode::Cancelled);
  REQUIRE(sonare_c_detail::error_code_from_c_error(SONARE_ERROR_INVALID_PARAMETER) ==
          sonare::ErrorCode::InvalidParameter);
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

TEST_CASE("MFCC inverse C API accepts the forward lifter", "[c_api][abi][inverse_features]") {
  constexpr int n_mfcc = 5;
  constexpr int n_frames = 4;
  constexpr int n_mels = 8;
  std::vector<float> lifted(static_cast<size_t>(n_mfcc) * n_frames, 0.0f);
  for (int coefficient = 0; coefficient < n_mfcc; ++coefficient) {
    const float lift = 1.0f + 11.0f * std::sin(3.14159265358979323846f * coefficient / 22.0f);
    for (int frame = 0; frame < n_frames; ++frame) {
      lifted[static_cast<size_t>(coefficient) * n_frames + frame] =
          (0.2f + 0.1f * coefficient) * lift;
    }
  }

  SonareInverseResult with_lifter{};
  SonareInverseResult without_lifter{};
  REQUIRE(sonare_mfcc_to_mel_ex(lifted.data(), n_mfcc, n_frames, n_mels, 22.0f, &with_lifter) ==
          SONARE_OK);
  REQUIRE(sonare_mfcc_to_mel(lifted.data(), n_mfcc, n_frames, n_mels, &without_lifter) ==
          SONARE_OK);
  REQUIRE(with_lifter.rows == without_lifter.rows);
  REQUIRE(with_lifter.n_frames == without_lifter.n_frames);
  const size_t length = static_cast<size_t>(with_lifter.rows) * with_lifter.n_frames;
  bool differs = false;
  for (size_t i = 0; i < length; ++i) {
    REQUIRE(std::isfinite(with_lifter.data[i]));
    differs = differs || std::abs(with_lifter.data[i] - without_lifter.data[i]) > 1.0e-5f;
  }
  REQUIRE(differs);
  sonare_free_inverse_result(&with_lifter);
  sonare_free_inverse_result(&without_lifter);
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
