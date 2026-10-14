/// @file sonare_c_abi_version_test.cpp
/// @brief Aggregate C-ABI version accessor, length-checked inverse transforms,
///        and the uniform non-finite input policy for the compat transforms.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "c_api/sonare_c_error_mapping.h"
#include "c_api/sonare_c_internal.h"
#ifdef SONARE_WITH_MIXING
#include "support/alloc_guard.h"
#endif

TEST_CASE("sonare_abi_version mirrors the compile-time aggregate", "[c_api][abi]") {
  REQUIRE(sonare_abi_version() == SONARE_ABI_VERSION);
  REQUIRE(sonare_abi_version() != 0u);
  // The low byte encodes the feature-struct ABI version.
  REQUIRE((sonare_abi_version() & 0xFFu) == SONARE_FEATURE_ABI_VERSION);
}

TEST_CASE("C exception recording preserves the existing classifications", "[c_api][abi]") {
  const auto classify = [](auto&& thrower) -> SonareError {
    try {
      thrower();
    } catch (...) {
      return sonare_c_detail::record_current_exception();
    }
    return SONARE_OK;
  };

  REQUIRE(classify([] {
            throw sonare::SonareException(sonare::ErrorCode::EncodeFailed, "encode failed");
          }) == SONARE_ERROR_ENCODE_FAILED);
  REQUIRE(sonare_last_error_code() == SONARE_ERROR_ENCODE_FAILED);
  REQUIRE(std::string(sonare_last_error_message()) == "encode failed");

  REQUIRE(classify([] { throw std::bad_alloc(); }) == SONARE_ERROR_OUT_OF_MEMORY);
  REQUIRE(classify([] { throw std::invalid_argument("bad argument"); }) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(classify([] { throw std::logic_error("bad state"); }) == SONARE_ERROR_INVALID_STATE);
  REQUIRE(classify([] { throw std::runtime_error("runtime failure"); }) == SONARE_ERROR_UNKNOWN);
  REQUIRE(classify([] { throw 42; }) == SONARE_ERROR_UNKNOWN);
  REQUIRE(std::string(sonare_last_error_message()) != "");

  sonare_c_detail::clear_last_error();
}

TEST_CASE("C diagnostic writer remains noexcept and preserves normal diagnostics", "[c_api][abi]") {
  static_assert(noexcept(sonare_c_detail::set_last_error(SONARE_OK, nullptr)));
  static_assert(noexcept(sonare_c_detail::clear_last_error()));
  static_assert(noexcept(sonare_c_detail::last_error_message()));

  const std::string long_message(4096, 'x');
  sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_STATE, long_message.c_str());
  REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_STATE);
  REQUIRE(std::string(sonare_last_error_message()) == long_message);

  // SONARE_OK is also used for successful calls that carry a diagnostic.
  sonare_c_detail::set_last_error(SONARE_OK, "success diagnostic");
  REQUIRE(sonare_last_error_code() == SONARE_OK);
  REQUIRE(std::string(sonare_last_error_message()) == "success diagnostic");

  sonare_c_detail::clear_last_error();
  REQUIRE(sonare_last_error_code() == SONARE_OK);
  REQUIRE(std::string(sonare_last_error_message()).empty());
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("C output helpers publish only after a complete copy", "[c_api][abi]") {
  const std::vector<float> values = {0.25f, -0.5f, 0.75f};

  float* vector_out = const_cast<float*>(values.data());
  size_t vector_length = 123;
  bool vector_threw = false;
  {
    sonare::test::AllocationFailureAtGuard fail_first(1);
    try {
      (void)sonare_c_detail::copy_vector(values, &vector_out, &vector_length);
    } catch (const std::bad_alloc&) {
      vector_threw = true;
    }
  }
  if (vector_out != nullptr && vector_out != values.data()) sonare_free_floats(vector_out);
  REQUIRE(vector_threw);
  REQUIRE(vector_out == nullptr);
  REQUIRE(vector_length == 0);

  const std::vector<float> empty;
  vector_out = const_cast<float*>(values.data());
  vector_length = 123;
  REQUIRE(sonare_c_detail::copy_vector(empty, &vector_out, &vector_length) == SONARE_OK);
  REQUIRE(vector_out == nullptr);
  REQUIRE(vector_length == 0);
  REQUIRE(sonare_c_detail::copy_vector(values, static_cast<float**>(nullptr), &vector_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_c_detail::copy_vector(values, &vector_out, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  const sonare::Audio audio = sonare::Audio::from_buffer(values.data(), values.size(), 22050);
  float* audio_out = const_cast<float*>(values.data());
  size_t audio_length = 123;
  bool audio_threw = false;
  {
    sonare::test::AllocationFailureAtGuard fail_first(1);
    try {
      (void)sonare_c_detail::copy_audio_result(audio, &audio_out, &audio_length);
    } catch (const std::bad_alloc&) {
      audio_threw = true;
    }
  }
  if (audio_out != nullptr && audio_out != values.data()) sonare_free_floats(audio_out);
  REQUIRE(audio_threw);
  REQUIRE(audio_out == nullptr);
  REQUIRE(audio_length == 0);

  REQUIRE(sonare_c_detail::copy_audio_result(audio, nullptr, &audio_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_c_detail::copy_audio_result(audio, &audio_out, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("C diagnostic storage failure preserves the code and recovers", "[c_api][abi]") {
  bool escaped = false;
  SonareError fallback_code = SONARE_OK;
  std::string fallback_message;
  SonareError cleared_code = SONARE_ERROR_UNKNOWN;
  std::string cleared_message;
  SonareError recovered_code = SONARE_OK;
  std::string recovered_message;

  std::thread worker([&] {
    // A fresh thread starts with an empty TLS string. Construct the requested
    // text before arming the allocator so the first guarded allocation is the
    // diagnostic assignment itself.
    const std::string long_message(4096, 'x');
    try {
      sonare::test::AllocationFailureAtGuard fail_first(1);
      sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_FORMAT, long_message.c_str());
    } catch (...) {
      escaped = true;
    }

    fallback_code = sonare_last_error_code();
    fallback_message = sonare_last_error_message();

    sonare_c_detail::clear_last_error();
    cleared_code = sonare_last_error_code();
    cleared_message = sonare_last_error_message();

    sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_STATE, long_message.c_str());
    recovered_code = sonare_last_error_code();
    recovered_message = sonare_last_error_message();
    sonare_c_detail::clear_last_error();
  });
  worker.join();

  REQUIRE_FALSE(escaped);
  REQUIRE(fallback_code == SONARE_ERROR_INVALID_FORMAT);
  REQUIRE_FALSE(fallback_message.empty());
  REQUIRE(fallback_message != std::string(4096, 'x'));
  REQUIRE(cleared_code == SONARE_OK);
  REQUIRE(cleared_message.empty());
  REQUIRE(recovered_code == SONARE_ERROR_INVALID_STATE);
  REQUIRE(recovered_message.size() == 4096);
  REQUIRE(recovered_message == std::string(4096, 'x'));
}
#endif

TEST_CASE("C error mapping preserves cancellation", "[c_api][abi]") {
  static_assert(noexcept(sonare_c_detail::c_error_from_error_code(sonare::ErrorCode::Ok)));
  static_assert(sonare_c_detail::c_error_from_error_code(sonare::ErrorCode::Ok) ==
                SONARE_ERROR_UNKNOWN);
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

  // The rejection comes from the core transform, so it also records a message
  // naming the function rather than returning a bare code.
  const char* detail = sonare_last_error_message();
  REQUIRE(detail != nullptr);
  REQUIRE(std::string(detail).find("non-finite") != std::string::npos);
}

TEST_CASE("Griffin-Lim inverse entry points reject non-finite input uniformly", "[c_api][abi]") {
  const int sample_rate = 22050;
  float* audio = nullptr;
  size_t audio_len = 0;

  const int n_fft = 4;
  const int n_bins = n_fft / 2 + 1;
  const int n_frames = 5;
  std::vector<float> magnitude(static_cast<size_t>(n_bins) * n_frames, 1.0f);
  REQUIRE(sonare_griffin_lim(magnitude.data(), magnitude.size(), n_bins, n_frames, n_fft, 1,
                             sample_rate, 1, 0.0f, &audio, &audio_len) == SONARE_OK);
  sonare_free_floats(audio);

  magnitude[4] = std::nanf("");
  audio = nullptr;
  audio_len = 0;
  REQUIRE(sonare_griffin_lim(magnitude.data(), magnitude.size(), n_bins, n_frames, n_fft, 1,
                             sample_rate, 1, 0.0f, &audio,
                             &audio_len) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(audio == nullptr);
  REQUIRE(audio_len == 0);

  const int cq_sr = 8000;
  const int cq_bins = 12;
  const int cq_frames = 4;
  std::vector<float> cq_magnitude(static_cast<size_t>(cq_bins) * cq_frames, 0.5f);
  cq_magnitude[5] = INFINITY;
  audio = nullptr;
  audio_len = 0;
  REQUIRE(sonare_cqt_to_audio_checked(cq_magnitude.data(), cq_magnitude.size(), cq_bins, cq_frames,
                                      cq_sr, 128, 130.8128f, 12, 1, &audio,
                                      &audio_len) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(audio == nullptr);

  audio = nullptr;
  audio_len = 0;
  REQUIRE(sonare_vqt_to_audio_checked(cq_magnitude.data(), cq_magnitude.size(), cq_bins, cq_frames,
                                      cq_sr, 128, 130.8128f, 12, 24.0f, 1, &audio,
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
