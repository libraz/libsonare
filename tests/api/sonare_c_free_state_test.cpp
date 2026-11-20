// Every result-releasing free leaves its struct in the empty state a fresh
// result starts from, so a second free, or a read after the free, sees nothing.

#include <sonare/sonare_c.h>
#include <sonare/sonare_c_engine.h>
#include <sonare/sonare_c_vocal_project.h>
#include <sonare/sonare_c_voice_changer.h>

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>

TEST_CASE("sonare_free_acoustic_result leaves the result zeroed", "[c_api][c_free]") {
  SonareAcousticResult r{};
  r.rt60 = static_cast<decltype(r.rt60)>(1);
  r.edt = static_cast<decltype(r.edt)>(1);
  r.c50 = static_cast<decltype(r.c50)>(1);
  r.c80 = static_cast<decltype(r.c80)>(1);
  r.d50 = static_cast<decltype(r.d50)>(1);
  r.band_count = static_cast<decltype(r.band_count)>(1);
  r.confidence = static_cast<decltype(r.confidence)>(1);
  r.is_blind = static_cast<decltype(r.is_blind)>(1);
  sonare_free_acoustic_result(&r);
  CHECK(r.rt60 == static_cast<decltype(r.rt60)>(0));
  CHECK(r.edt == static_cast<decltype(r.edt)>(0));
  CHECK(r.c50 == static_cast<decltype(r.c50)>(0));
  CHECK(r.c80 == static_cast<decltype(r.c80)>(0));
  CHECK(r.d50 == static_cast<decltype(r.d50)>(0));
  CHECK(r.rt60_bands == nullptr);
  CHECK(r.edt_bands == nullptr);
  CHECK(r.c50_bands == nullptr);
  CHECK(r.c80_bands == nullptr);
  CHECK(r.band_count == static_cast<decltype(r.band_count)>(0));
  CHECK(r.confidence == static_cast<decltype(r.confidence)>(0));
  CHECK(r.is_blind == static_cast<decltype(r.is_blind)>(0));
}

TEST_CASE("sonare_free_analysis_result leaves the result zeroed", "[c_api][c_free]") {
  SonareAnalysisResult r{};
  r.bpm = static_cast<decltype(r.bpm)>(1);
  r.bpm_confidence = static_cast<decltype(r.bpm_confidence)>(1);
  r.key.root = static_cast<decltype(r.key.root)>(1);
  r.key.mode = static_cast<decltype(r.key.mode)>(1);
  r.key.confidence = static_cast<decltype(r.key.confidence)>(1);
  r.time_signature.numerator = static_cast<decltype(r.time_signature.numerator)>(1);
  r.time_signature.denominator = static_cast<decltype(r.time_signature.denominator)>(1);
  r.time_signature.confidence = static_cast<decltype(r.time_signature.confidence)>(1);
  r.beat_count = static_cast<decltype(r.beat_count)>(1);
  r.bpm_candidate_count = static_cast<decltype(r.bpm_candidate_count)>(1);
  r.time_signature_candidate_count = static_cast<decltype(r.time_signature_candidate_count)>(1);
  sonare_free_analysis_result(&r);
  CHECK(r.bpm == static_cast<decltype(r.bpm)>(0));
  CHECK(r.bpm_confidence == static_cast<decltype(r.bpm_confidence)>(0));
  CHECK(r.key.root == static_cast<decltype(r.key.root)>(0));
  CHECK(r.key.mode == static_cast<decltype(r.key.mode)>(0));
  CHECK(r.key.confidence == static_cast<decltype(r.key.confidence)>(0));
  CHECK(r.time_signature.numerator == static_cast<decltype(r.time_signature.numerator)>(0));
  CHECK(r.time_signature.denominator == static_cast<decltype(r.time_signature.denominator)>(0));
  CHECK(r.time_signature.confidence == static_cast<decltype(r.time_signature.confidence)>(0));
  CHECK(r.beat_times == nullptr);
  CHECK(r.beat_count == static_cast<decltype(r.beat_count)>(0));
  CHECK(r.bpm_candidates == nullptr);
  CHECK(r.bpm_candidate_count == static_cast<decltype(r.bpm_candidate_count)>(0));
  CHECK(r.time_signature_candidates == nullptr);
  CHECK(r.time_signature_candidate_count ==
        static_cast<decltype(r.time_signature_candidate_count)>(0));
}

TEST_CASE("sonare_free_boundary_result leaves the result zeroed", "[c_api][c_free]") {
  SonareBoundaryResult r{};
  r.boundary_count = static_cast<decltype(r.boundary_count)>(1);
  r.novelty_length = static_cast<decltype(r.novelty_length)>(1);
  r.novelty_peak = static_cast<decltype(r.novelty_peak)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  r.hop_length = static_cast<decltype(r.hop_length)>(1);
  r.n_frames = static_cast<decltype(r.n_frames)>(1);
  r.frame_stride = static_cast<decltype(r.frame_stride)>(1);
  sonare_free_boundary_result(&r);
  CHECK(r.boundaries == nullptr);
  CHECK(r.boundary_count == static_cast<decltype(r.boundary_count)>(0));
  CHECK(r.novelty_curve == nullptr);
  CHECK(r.novelty_length == static_cast<decltype(r.novelty_length)>(0));
  CHECK(r.novelty_peak == static_cast<decltype(r.novelty_peak)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
  CHECK(r.hop_length == static_cast<decltype(r.hop_length)>(0));
  CHECK(r.n_frames == static_cast<decltype(r.n_frames)>(0));
  CHECK(r.frame_stride == static_cast<decltype(r.frame_stride)>(0));
}

TEST_CASE("sonare_free_bpm_analysis_result leaves the result zeroed", "[c_api][c_free]") {
  SonareBpmAnalysisResult r{};
  r.bpm = static_cast<decltype(r.bpm)>(1);
  r.confidence = static_cast<decltype(r.confidence)>(1);
  r.candidate_count = static_cast<decltype(r.candidate_count)>(1);
  r.autocorrelation_count = static_cast<decltype(r.autocorrelation_count)>(1);
  r.tempogram_count = static_cast<decltype(r.tempogram_count)>(1);
  sonare_free_bpm_analysis_result(&r);
  CHECK(r.bpm == static_cast<decltype(r.bpm)>(0));
  CHECK(r.confidence == static_cast<decltype(r.confidence)>(0));
  CHECK(r.candidates == nullptr);
  CHECK(r.candidate_count == static_cast<decltype(r.candidate_count)>(0));
  CHECK(r.autocorrelation == nullptr);
  CHECK(r.autocorrelation_count == static_cast<decltype(r.autocorrelation_count)>(0));
  CHECK(r.tempogram == nullptr);
  CHECK(r.tempogram_count == static_cast<decltype(r.tempogram_count)>(0));
}

TEST_CASE("sonare_free_chord_analysis_result leaves the result zeroed", "[c_api][c_free]") {
  SonareChordAnalysisResult r{};
  r.tuning_cents = static_cast<decltype(r.tuning_cents)>(1);
  r.chord_count = static_cast<decltype(r.chord_count)>(1);
  sonare_free_chord_analysis_result(&r);
  CHECK(r.tuning_cents == static_cast<decltype(r.tuning_cents)>(0));
  CHECK(r.chords == nullptr);
  CHECK(r.chord_count == static_cast<decltype(r.chord_count)>(0));
}

TEST_CASE("sonare_free_chroma_result leaves the result zeroed", "[c_api][c_free]") {
  SonareChromaResult r{};
  r.n_chroma = static_cast<decltype(r.n_chroma)>(1);
  r.n_frames = static_cast<decltype(r.n_frames)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  r.hop_length = static_cast<decltype(r.hop_length)>(1);
  sonare_free_chroma_result(&r);
  CHECK(r.n_chroma == static_cast<decltype(r.n_chroma)>(0));
  CHECK(r.n_frames == static_cast<decltype(r.n_frames)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
  CHECK(r.hop_length == static_cast<decltype(r.hop_length)>(0));
  CHECK(r.features == nullptr);
  CHECK(r.mean_energy == nullptr);
}

TEST_CASE("sonare_free_clipping_result leaves the result zeroed", "[c_api][c_free]") {
  SonareClippingResult r{};
  r.clipped_samples = static_cast<decltype(r.clipped_samples)>(1);
  r.clipping_ratio = static_cast<decltype(r.clipping_ratio)>(1);
  r.max_clipped_peak = static_cast<decltype(r.max_clipped_peak)>(1);
  r.region_count = static_cast<decltype(r.region_count)>(1);
  sonare_free_clipping_result(&r);
  CHECK(r.clipped_samples == static_cast<decltype(r.clipped_samples)>(0));
  CHECK(r.clipping_ratio == static_cast<decltype(r.clipping_ratio)>(0));
  CHECK(r.max_clipped_peak == static_cast<decltype(r.max_clipped_peak)>(0));
  CHECK(r.regions == nullptr);
  CHECK(r.region_count == static_cast<decltype(r.region_count)>(0));
}

TEST_CASE("sonare_free_cqt_result leaves the result zeroed", "[c_api][c_free]") {
  SonareCqtResult r{};
  r.n_bins = static_cast<decltype(r.n_bins)>(1);
  r.n_frames = static_cast<decltype(r.n_frames)>(1);
  r.hop_length = static_cast<decltype(r.hop_length)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  sonare_free_cqt_result(&r);
  CHECK(r.n_bins == static_cast<decltype(r.n_bins)>(0));
  CHECK(r.n_frames == static_cast<decltype(r.n_frames)>(0));
  CHECK(r.hop_length == static_cast<decltype(r.hop_length)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
  CHECK(r.magnitude == nullptr);
  CHECK(r.frequencies == nullptr);
}

TEST_CASE("sonare_free_declick_result leaves the result zeroed", "[c_api][c_free]") {
  SonareDeclickResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.report.detected.count = static_cast<decltype(r.report.detected.count)>(1);
  r.report.detected.rejected = static_cast<decltype(r.report.detected.rejected)>(1);
  r.report.detected.longest_run_samples =
      static_cast<decltype(r.report.detected.longest_run_samples)>(1);
  r.report.detected.per_second = static_cast<decltype(r.report.detected.per_second)>(1);
  r.report.repaired_runs = static_cast<decltype(r.report.repaired_runs)>(1);
  r.report.repaired_samples = static_cast<decltype(r.report.repaired_samples)>(1);
  r.report.linked_runs = static_cast<decltype(r.report.linked_runs)>(1);
  r.report.lpc_model_used = static_cast<decltype(r.report.lpc_model_used)>(1);
  sonare_free_declick_result(&r);
  CHECK(r.samples == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.report.detected.count == static_cast<decltype(r.report.detected.count)>(0));
  CHECK(r.report.detected.rejected == static_cast<decltype(r.report.detected.rejected)>(0));
  CHECK(r.report.detected.longest_run_samples ==
        static_cast<decltype(r.report.detected.longest_run_samples)>(0));
  CHECK(r.report.detected.per_second == static_cast<decltype(r.report.detected.per_second)>(0));
  CHECK(r.report.repaired_runs == static_cast<decltype(r.report.repaired_runs)>(0));
  CHECK(r.report.repaired_samples == static_cast<decltype(r.report.repaired_samples)>(0));
  CHECK(r.report.linked_runs == static_cast<decltype(r.report.linked_runs)>(0));
  CHECK(r.report.lpc_model_used == static_cast<decltype(r.report.lpc_model_used)>(0));
}

TEST_CASE("sonare_free_declick_stereo_result leaves the result zeroed", "[c_api][c_free]") {
  SonareDeclickStereoResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.left_report.detected.count = static_cast<decltype(r.left_report.detected.count)>(1);
  r.left_report.detected.rejected = static_cast<decltype(r.left_report.detected.rejected)>(1);
  r.left_report.detected.longest_run_samples =
      static_cast<decltype(r.left_report.detected.longest_run_samples)>(1);
  r.left_report.detected.per_second = static_cast<decltype(r.left_report.detected.per_second)>(1);
  r.left_report.repaired_runs = static_cast<decltype(r.left_report.repaired_runs)>(1);
  r.left_report.repaired_samples = static_cast<decltype(r.left_report.repaired_samples)>(1);
  r.left_report.linked_runs = static_cast<decltype(r.left_report.linked_runs)>(1);
  r.left_report.lpc_model_used = static_cast<decltype(r.left_report.lpc_model_used)>(1);
  r.right_report.detected.count = static_cast<decltype(r.right_report.detected.count)>(1);
  r.right_report.detected.rejected = static_cast<decltype(r.right_report.detected.rejected)>(1);
  r.right_report.detected.longest_run_samples =
      static_cast<decltype(r.right_report.detected.longest_run_samples)>(1);
  r.right_report.detected.per_second = static_cast<decltype(r.right_report.detected.per_second)>(1);
  r.right_report.repaired_runs = static_cast<decltype(r.right_report.repaired_runs)>(1);
  r.right_report.repaired_samples = static_cast<decltype(r.right_report.repaired_samples)>(1);
  r.right_report.linked_runs = static_cast<decltype(r.right_report.linked_runs)>(1);
  r.right_report.lpc_model_used = static_cast<decltype(r.right_report.lpc_model_used)>(1);
  sonare_free_declick_stereo_result(&r);
  CHECK(r.left == nullptr);
  CHECK(r.right == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.left_report.detected.count == static_cast<decltype(r.left_report.detected.count)>(0));
  CHECK(r.left_report.detected.rejected ==
        static_cast<decltype(r.left_report.detected.rejected)>(0));
  CHECK(r.left_report.detected.longest_run_samples ==
        static_cast<decltype(r.left_report.detected.longest_run_samples)>(0));
  CHECK(r.left_report.detected.per_second ==
        static_cast<decltype(r.left_report.detected.per_second)>(0));
  CHECK(r.left_report.repaired_runs == static_cast<decltype(r.left_report.repaired_runs)>(0));
  CHECK(r.left_report.repaired_samples == static_cast<decltype(r.left_report.repaired_samples)>(0));
  CHECK(r.left_report.linked_runs == static_cast<decltype(r.left_report.linked_runs)>(0));
  CHECK(r.left_report.lpc_model_used == static_cast<decltype(r.left_report.lpc_model_used)>(0));
  CHECK(r.right_report.detected.count == static_cast<decltype(r.right_report.detected.count)>(0));
  CHECK(r.right_report.detected.rejected ==
        static_cast<decltype(r.right_report.detected.rejected)>(0));
  CHECK(r.right_report.detected.longest_run_samples ==
        static_cast<decltype(r.right_report.detected.longest_run_samples)>(0));
  CHECK(r.right_report.detected.per_second ==
        static_cast<decltype(r.right_report.detected.per_second)>(0));
  CHECK(r.right_report.repaired_runs == static_cast<decltype(r.right_report.repaired_runs)>(0));
  CHECK(r.right_report.repaired_samples ==
        static_cast<decltype(r.right_report.repaired_samples)>(0));
  CHECK(r.right_report.linked_runs == static_cast<decltype(r.right_report.linked_runs)>(0));
  CHECK(r.right_report.lpc_model_used == static_cast<decltype(r.right_report.lpc_model_used)>(0));
}

TEST_CASE("sonare_free_declip_result leaves the result zeroed", "[c_api][c_free]") {
  SonareDeclipResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.report.detected.clipped_samples = static_cast<decltype(r.report.detected.clipped_samples)>(1);
  r.report.detected.clipped_ratio = static_cast<decltype(r.report.detected.clipped_ratio)>(1);
  r.report.detected.run_count = static_cast<decltype(r.report.detected.run_count)>(1);
  r.report.detected.longest_run_samples =
      static_cast<decltype(r.report.detected.longest_run_samples)>(1);
  r.report.detected.flat_run_count = static_cast<decltype(r.report.detected.flat_run_count)>(1);
  r.report.detected.longest_flat_run_samples =
      static_cast<decltype(r.report.detected.longest_flat_run_samples)>(1);
  r.report.detected.flat_sample_count =
      static_cast<decltype(r.report.detected.flat_sample_count)>(1);
  r.report.detected.flat_level = static_cast<decltype(r.report.detected.flat_level)>(1);
  r.report.lpc_reconstructed_runs = static_cast<decltype(r.report.lpc_reconstructed_runs)>(1);
  r.report.interpolated_runs = static_cast<decltype(r.report.interpolated_runs)>(1);
  r.report.repaired_samples = static_cast<decltype(r.report.repaired_samples)>(1);
  r.report.linked_runs = static_cast<decltype(r.report.linked_runs)>(1);
  sonare_free_declip_result(&r);
  CHECK(r.samples == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.report.detected.clipped_samples ==
        static_cast<decltype(r.report.detected.clipped_samples)>(0));
  CHECK(r.report.detected.clipped_ratio ==
        static_cast<decltype(r.report.detected.clipped_ratio)>(0));
  CHECK(r.report.detected.run_count == static_cast<decltype(r.report.detected.run_count)>(0));
  CHECK(r.report.detected.longest_run_samples ==
        static_cast<decltype(r.report.detected.longest_run_samples)>(0));
  CHECK(r.report.detected.flat_run_count ==
        static_cast<decltype(r.report.detected.flat_run_count)>(0));
  CHECK(r.report.detected.longest_flat_run_samples ==
        static_cast<decltype(r.report.detected.longest_flat_run_samples)>(0));
  CHECK(r.report.detected.flat_sample_count ==
        static_cast<decltype(r.report.detected.flat_sample_count)>(0));
  CHECK(r.report.detected.flat_level == static_cast<decltype(r.report.detected.flat_level)>(0));
  CHECK(r.report.lpc_reconstructed_runs ==
        static_cast<decltype(r.report.lpc_reconstructed_runs)>(0));
  CHECK(r.report.interpolated_runs == static_cast<decltype(r.report.interpolated_runs)>(0));
  CHECK(r.report.repaired_samples == static_cast<decltype(r.report.repaired_samples)>(0));
  CHECK(r.report.linked_runs == static_cast<decltype(r.report.linked_runs)>(0));
}

TEST_CASE("sonare_free_declip_stereo_result leaves the result zeroed", "[c_api][c_free]") {
  SonareDeclipStereoResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.left_report.detected.clipped_samples =
      static_cast<decltype(r.left_report.detected.clipped_samples)>(1);
  r.left_report.detected.clipped_ratio =
      static_cast<decltype(r.left_report.detected.clipped_ratio)>(1);
  r.left_report.detected.run_count = static_cast<decltype(r.left_report.detected.run_count)>(1);
  r.left_report.detected.longest_run_samples =
      static_cast<decltype(r.left_report.detected.longest_run_samples)>(1);
  r.left_report.detected.flat_run_count =
      static_cast<decltype(r.left_report.detected.flat_run_count)>(1);
  r.left_report.detected.longest_flat_run_samples =
      static_cast<decltype(r.left_report.detected.longest_flat_run_samples)>(1);
  r.left_report.detected.flat_sample_count =
      static_cast<decltype(r.left_report.detected.flat_sample_count)>(1);
  r.left_report.detected.flat_level = static_cast<decltype(r.left_report.detected.flat_level)>(1);
  r.left_report.lpc_reconstructed_runs =
      static_cast<decltype(r.left_report.lpc_reconstructed_runs)>(1);
  r.left_report.interpolated_runs = static_cast<decltype(r.left_report.interpolated_runs)>(1);
  r.left_report.repaired_samples = static_cast<decltype(r.left_report.repaired_samples)>(1);
  r.left_report.linked_runs = static_cast<decltype(r.left_report.linked_runs)>(1);
  r.right_report.detected.clipped_samples =
      static_cast<decltype(r.right_report.detected.clipped_samples)>(1);
  r.right_report.detected.clipped_ratio =
      static_cast<decltype(r.right_report.detected.clipped_ratio)>(1);
  r.right_report.detected.run_count = static_cast<decltype(r.right_report.detected.run_count)>(1);
  r.right_report.detected.longest_run_samples =
      static_cast<decltype(r.right_report.detected.longest_run_samples)>(1);
  r.right_report.detected.flat_run_count =
      static_cast<decltype(r.right_report.detected.flat_run_count)>(1);
  r.right_report.detected.longest_flat_run_samples =
      static_cast<decltype(r.right_report.detected.longest_flat_run_samples)>(1);
  r.right_report.detected.flat_sample_count =
      static_cast<decltype(r.right_report.detected.flat_sample_count)>(1);
  r.right_report.detected.flat_level = static_cast<decltype(r.right_report.detected.flat_level)>(1);
  r.right_report.lpc_reconstructed_runs =
      static_cast<decltype(r.right_report.lpc_reconstructed_runs)>(1);
  r.right_report.interpolated_runs = static_cast<decltype(r.right_report.interpolated_runs)>(1);
  r.right_report.repaired_samples = static_cast<decltype(r.right_report.repaired_samples)>(1);
  r.right_report.linked_runs = static_cast<decltype(r.right_report.linked_runs)>(1);
  sonare_free_declip_stereo_result(&r);
  CHECK(r.left == nullptr);
  CHECK(r.right == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.left_report.detected.clipped_samples ==
        static_cast<decltype(r.left_report.detected.clipped_samples)>(0));
  CHECK(r.left_report.detected.clipped_ratio ==
        static_cast<decltype(r.left_report.detected.clipped_ratio)>(0));
  CHECK(r.left_report.detected.run_count ==
        static_cast<decltype(r.left_report.detected.run_count)>(0));
  CHECK(r.left_report.detected.longest_run_samples ==
        static_cast<decltype(r.left_report.detected.longest_run_samples)>(0));
  CHECK(r.left_report.detected.flat_run_count ==
        static_cast<decltype(r.left_report.detected.flat_run_count)>(0));
  CHECK(r.left_report.detected.longest_flat_run_samples ==
        static_cast<decltype(r.left_report.detected.longest_flat_run_samples)>(0));
  CHECK(r.left_report.detected.flat_sample_count ==
        static_cast<decltype(r.left_report.detected.flat_sample_count)>(0));
  CHECK(r.left_report.detected.flat_level ==
        static_cast<decltype(r.left_report.detected.flat_level)>(0));
  CHECK(r.left_report.lpc_reconstructed_runs ==
        static_cast<decltype(r.left_report.lpc_reconstructed_runs)>(0));
  CHECK(r.left_report.interpolated_runs ==
        static_cast<decltype(r.left_report.interpolated_runs)>(0));
  CHECK(r.left_report.repaired_samples == static_cast<decltype(r.left_report.repaired_samples)>(0));
  CHECK(r.left_report.linked_runs == static_cast<decltype(r.left_report.linked_runs)>(0));
  CHECK(r.right_report.detected.clipped_samples ==
        static_cast<decltype(r.right_report.detected.clipped_samples)>(0));
  CHECK(r.right_report.detected.clipped_ratio ==
        static_cast<decltype(r.right_report.detected.clipped_ratio)>(0));
  CHECK(r.right_report.detected.run_count ==
        static_cast<decltype(r.right_report.detected.run_count)>(0));
  CHECK(r.right_report.detected.longest_run_samples ==
        static_cast<decltype(r.right_report.detected.longest_run_samples)>(0));
  CHECK(r.right_report.detected.flat_run_count ==
        static_cast<decltype(r.right_report.detected.flat_run_count)>(0));
  CHECK(r.right_report.detected.longest_flat_run_samples ==
        static_cast<decltype(r.right_report.detected.longest_flat_run_samples)>(0));
  CHECK(r.right_report.detected.flat_sample_count ==
        static_cast<decltype(r.right_report.detected.flat_sample_count)>(0));
  CHECK(r.right_report.detected.flat_level ==
        static_cast<decltype(r.right_report.detected.flat_level)>(0));
  CHECK(r.right_report.lpc_reconstructed_runs ==
        static_cast<decltype(r.right_report.lpc_reconstructed_runs)>(0));
  CHECK(r.right_report.interpolated_runs ==
        static_cast<decltype(r.right_report.interpolated_runs)>(0));
  CHECK(r.right_report.repaired_samples ==
        static_cast<decltype(r.right_report.repaired_samples)>(0));
  CHECK(r.right_report.linked_runs == static_cast<decltype(r.right_report.linked_runs)>(0));
}

TEST_CASE("sonare_free_decrackle_result leaves the result zeroed", "[c_api][c_free]") {
  SonareDecrackleResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.report.detected.sample_count = static_cast<decltype(r.report.detected.sample_count)>(1);
  r.report.detected.sample_fraction = static_cast<decltype(r.report.detected.sample_fraction)>(1);
  r.report.detected.per_second = static_cast<decltype(r.report.detected.per_second)>(1);
  r.report.replaced_samples = static_cast<decltype(r.report.replaced_samples)>(1);
  r.report.detail_coefficients = static_cast<decltype(r.report.detail_coefficients)>(1);
  r.report.shrunk_coefficients = static_cast<decltype(r.report.shrunk_coefficients)>(1);
  r.report.noise_sigma = static_cast<decltype(r.report.noise_sigma)>(1);
  sonare_free_decrackle_result(&r);
  CHECK(r.samples == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.report.detected.sample_count == static_cast<decltype(r.report.detected.sample_count)>(0));
  CHECK(r.report.detected.sample_fraction ==
        static_cast<decltype(r.report.detected.sample_fraction)>(0));
  CHECK(r.report.detected.per_second == static_cast<decltype(r.report.detected.per_second)>(0));
  CHECK(r.report.replaced_samples == static_cast<decltype(r.report.replaced_samples)>(0));
  CHECK(r.report.detail_coefficients == static_cast<decltype(r.report.detail_coefficients)>(0));
  CHECK(r.report.shrunk_coefficients == static_cast<decltype(r.report.shrunk_coefficients)>(0));
  CHECK(r.report.noise_sigma == static_cast<decltype(r.report.noise_sigma)>(0));
}

TEST_CASE("sonare_free_decrackle_stereo_result leaves the result zeroed", "[c_api][c_free]") {
  SonareDecrackleStereoResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.left_report.detected.sample_count =
      static_cast<decltype(r.left_report.detected.sample_count)>(1);
  r.left_report.detected.sample_fraction =
      static_cast<decltype(r.left_report.detected.sample_fraction)>(1);
  r.left_report.detected.per_second = static_cast<decltype(r.left_report.detected.per_second)>(1);
  r.left_report.replaced_samples = static_cast<decltype(r.left_report.replaced_samples)>(1);
  r.left_report.detail_coefficients = static_cast<decltype(r.left_report.detail_coefficients)>(1);
  r.left_report.shrunk_coefficients = static_cast<decltype(r.left_report.shrunk_coefficients)>(1);
  r.left_report.noise_sigma = static_cast<decltype(r.left_report.noise_sigma)>(1);
  r.right_report.detected.sample_count =
      static_cast<decltype(r.right_report.detected.sample_count)>(1);
  r.right_report.detected.sample_fraction =
      static_cast<decltype(r.right_report.detected.sample_fraction)>(1);
  r.right_report.detected.per_second = static_cast<decltype(r.right_report.detected.per_second)>(1);
  r.right_report.replaced_samples = static_cast<decltype(r.right_report.replaced_samples)>(1);
  r.right_report.detail_coefficients = static_cast<decltype(r.right_report.detail_coefficients)>(1);
  r.right_report.shrunk_coefficients = static_cast<decltype(r.right_report.shrunk_coefficients)>(1);
  r.right_report.noise_sigma = static_cast<decltype(r.right_report.noise_sigma)>(1);
  sonare_free_decrackle_stereo_result(&r);
  CHECK(r.left == nullptr);
  CHECK(r.right == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.left_report.detected.sample_count ==
        static_cast<decltype(r.left_report.detected.sample_count)>(0));
  CHECK(r.left_report.detected.sample_fraction ==
        static_cast<decltype(r.left_report.detected.sample_fraction)>(0));
  CHECK(r.left_report.detected.per_second ==
        static_cast<decltype(r.left_report.detected.per_second)>(0));
  CHECK(r.left_report.replaced_samples == static_cast<decltype(r.left_report.replaced_samples)>(0));
  CHECK(r.left_report.detail_coefficients ==
        static_cast<decltype(r.left_report.detail_coefficients)>(0));
  CHECK(r.left_report.shrunk_coefficients ==
        static_cast<decltype(r.left_report.shrunk_coefficients)>(0));
  CHECK(r.left_report.noise_sigma == static_cast<decltype(r.left_report.noise_sigma)>(0));
  CHECK(r.right_report.detected.sample_count ==
        static_cast<decltype(r.right_report.detected.sample_count)>(0));
  CHECK(r.right_report.detected.sample_fraction ==
        static_cast<decltype(r.right_report.detected.sample_fraction)>(0));
  CHECK(r.right_report.detected.per_second ==
        static_cast<decltype(r.right_report.detected.per_second)>(0));
  CHECK(r.right_report.replaced_samples ==
        static_cast<decltype(r.right_report.replaced_samples)>(0));
  CHECK(r.right_report.detail_coefficients ==
        static_cast<decltype(r.right_report.detail_coefficients)>(0));
  CHECK(r.right_report.shrunk_coefficients ==
        static_cast<decltype(r.right_report.shrunk_coefficients)>(0));
  CHECK(r.right_report.noise_sigma == static_cast<decltype(r.right_report.noise_sigma)>(0));
}

TEST_CASE("sonare_free_dehum_result leaves the result zeroed", "[c_api][c_free]") {
  SonareDehumResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.report.detected.fundamental_hz = static_cast<decltype(r.report.detected.fundamental_hz)>(1);
  r.report.detected.fundamental_prominence =
      static_cast<decltype(r.report.detected.fundamental_prominence)>(1);
  r.report.detected.harmonics = static_cast<decltype(r.report.detected.harmonics)>(1);
  std::memset(&r.report.detected.harmonic_dbfs, 1, sizeof(r.report.detected.harmonic_dbfs));
  r.report.notched_harmonics = static_cast<decltype(r.report.notched_harmonics)>(1);
  r.report.applied_fundamental_hz = static_cast<decltype(r.report.applied_fundamental_hz)>(1);
  r.report.fundamental_drift_hz = static_cast<decltype(r.report.fundamental_drift_hz)>(1);
  sonare_free_dehum_result(&r);
  CHECK(r.samples == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.report.detected.fundamental_hz ==
        static_cast<decltype(r.report.detected.fundamental_hz)>(0));
  CHECK(r.report.detected.fundamental_prominence ==
        static_cast<decltype(r.report.detected.fundamental_prominence)>(0));
  CHECK(r.report.detected.harmonics == static_cast<decltype(r.report.detected.harmonics)>(0));
  for (size_t i = 0; i < sizeof(r.report.detected.harmonic_dbfs); ++i)
    CHECK(reinterpret_cast<const unsigned char*>(&r.report.detected.harmonic_dbfs)[i] == 0);
  CHECK(r.report.notched_harmonics == static_cast<decltype(r.report.notched_harmonics)>(0));
  CHECK(r.report.applied_fundamental_hz ==
        static_cast<decltype(r.report.applied_fundamental_hz)>(0));
  CHECK(r.report.fundamental_drift_hz == static_cast<decltype(r.report.fundamental_drift_hz)>(0));
}

TEST_CASE("sonare_free_dehum_stereo_result leaves the result zeroed", "[c_api][c_free]") {
  SonareDehumStereoResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.left_report.detected.fundamental_hz =
      static_cast<decltype(r.left_report.detected.fundamental_hz)>(1);
  r.left_report.detected.fundamental_prominence =
      static_cast<decltype(r.left_report.detected.fundamental_prominence)>(1);
  r.left_report.detected.harmonics = static_cast<decltype(r.left_report.detected.harmonics)>(1);
  std::memset(&r.left_report.detected.harmonic_dbfs, 1,
              sizeof(r.left_report.detected.harmonic_dbfs));
  r.left_report.notched_harmonics = static_cast<decltype(r.left_report.notched_harmonics)>(1);
  r.left_report.applied_fundamental_hz =
      static_cast<decltype(r.left_report.applied_fundamental_hz)>(1);
  r.left_report.fundamental_drift_hz = static_cast<decltype(r.left_report.fundamental_drift_hz)>(1);
  r.right_report.detected.fundamental_hz =
      static_cast<decltype(r.right_report.detected.fundamental_hz)>(1);
  r.right_report.detected.fundamental_prominence =
      static_cast<decltype(r.right_report.detected.fundamental_prominence)>(1);
  r.right_report.detected.harmonics = static_cast<decltype(r.right_report.detected.harmonics)>(1);
  std::memset(&r.right_report.detected.harmonic_dbfs, 1,
              sizeof(r.right_report.detected.harmonic_dbfs));
  r.right_report.notched_harmonics = static_cast<decltype(r.right_report.notched_harmonics)>(1);
  r.right_report.applied_fundamental_hz =
      static_cast<decltype(r.right_report.applied_fundamental_hz)>(1);
  r.right_report.fundamental_drift_hz =
      static_cast<decltype(r.right_report.fundamental_drift_hz)>(1);
  sonare_free_dehum_stereo_result(&r);
  CHECK(r.left == nullptr);
  CHECK(r.right == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.left_report.detected.fundamental_hz ==
        static_cast<decltype(r.left_report.detected.fundamental_hz)>(0));
  CHECK(r.left_report.detected.fundamental_prominence ==
        static_cast<decltype(r.left_report.detected.fundamental_prominence)>(0));
  CHECK(r.left_report.detected.harmonics ==
        static_cast<decltype(r.left_report.detected.harmonics)>(0));
  for (size_t i = 0; i < sizeof(r.left_report.detected.harmonic_dbfs); ++i)
    CHECK(reinterpret_cast<const unsigned char*>(&r.left_report.detected.harmonic_dbfs)[i] == 0);
  CHECK(r.left_report.notched_harmonics ==
        static_cast<decltype(r.left_report.notched_harmonics)>(0));
  CHECK(r.left_report.applied_fundamental_hz ==
        static_cast<decltype(r.left_report.applied_fundamental_hz)>(0));
  CHECK(r.left_report.fundamental_drift_hz ==
        static_cast<decltype(r.left_report.fundamental_drift_hz)>(0));
  CHECK(r.right_report.detected.fundamental_hz ==
        static_cast<decltype(r.right_report.detected.fundamental_hz)>(0));
  CHECK(r.right_report.detected.fundamental_prominence ==
        static_cast<decltype(r.right_report.detected.fundamental_prominence)>(0));
  CHECK(r.right_report.detected.harmonics ==
        static_cast<decltype(r.right_report.detected.harmonics)>(0));
  for (size_t i = 0; i < sizeof(r.right_report.detected.harmonic_dbfs); ++i)
    CHECK(reinterpret_cast<const unsigned char*>(&r.right_report.detected.harmonic_dbfs)[i] == 0);
  CHECK(r.right_report.notched_harmonics ==
        static_cast<decltype(r.right_report.notched_harmonics)>(0));
  CHECK(r.right_report.applied_fundamental_hz ==
        static_cast<decltype(r.right_report.applied_fundamental_hz)>(0));
  CHECK(r.right_report.fundamental_drift_hz ==
        static_cast<decltype(r.right_report.fundamental_drift_hz)>(0));
}

TEST_CASE("sonare_free_denoise_result leaves the result zeroed", "[c_api][c_free]") {
  SonareDenoiseResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.report.detected.floor_dbfs = static_cast<decltype(r.report.detected.floor_dbfs)>(1);
  std::memset(&r.report.detected.band_floor_dbfs, 1, sizeof(r.report.detected.band_floor_dbfs));
  r.report.mean_reduction_db = static_cast<decltype(r.report.mean_reduction_db)>(1);
  r.report.max_reduction_db = static_cast<decltype(r.report.max_reduction_db)>(1);
  r.report.floor_limited_fraction = static_cast<decltype(r.report.floor_limited_fraction)>(1);
  sonare_free_denoise_result(&r);
  CHECK(r.samples == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.report.detected.floor_dbfs == static_cast<decltype(r.report.detected.floor_dbfs)>(0));
  for (size_t i = 0; i < sizeof(r.report.detected.band_floor_dbfs); ++i)
    CHECK(reinterpret_cast<const unsigned char*>(&r.report.detected.band_floor_dbfs)[i] == 0);
  CHECK(r.report.mean_reduction_db == static_cast<decltype(r.report.mean_reduction_db)>(0));
  CHECK(r.report.max_reduction_db == static_cast<decltype(r.report.max_reduction_db)>(0));
  CHECK(r.report.floor_limited_fraction ==
        static_cast<decltype(r.report.floor_limited_fraction)>(0));
}

TEST_CASE("sonare_free_denoise_stereo_result leaves the result zeroed", "[c_api][c_free]") {
  SonareDenoiseStereoResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.report.detected.floor_dbfs = static_cast<decltype(r.report.detected.floor_dbfs)>(1);
  std::memset(&r.report.detected.band_floor_dbfs, 1, sizeof(r.report.detected.band_floor_dbfs));
  r.report.mean_reduction_db = static_cast<decltype(r.report.mean_reduction_db)>(1);
  r.report.max_reduction_db = static_cast<decltype(r.report.max_reduction_db)>(1);
  r.report.floor_limited_fraction = static_cast<decltype(r.report.floor_limited_fraction)>(1);
  sonare_free_denoise_stereo_result(&r);
  CHECK(r.left == nullptr);
  CHECK(r.right == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.report.detected.floor_dbfs == static_cast<decltype(r.report.detected.floor_dbfs)>(0));
  for (size_t i = 0; i < sizeof(r.report.detected.band_floor_dbfs); ++i)
    CHECK(reinterpret_cast<const unsigned char*>(&r.report.detected.band_floor_dbfs)[i] == 0);
  CHECK(r.report.mean_reduction_db == static_cast<decltype(r.report.mean_reduction_db)>(0));
  CHECK(r.report.max_reduction_db == static_cast<decltype(r.report.max_reduction_db)>(0));
  CHECK(r.report.floor_limited_fraction ==
        static_cast<decltype(r.report.floor_limited_fraction)>(0));
}

TEST_CASE("sonare_free_dereverb_result leaves the result zeroed", "[c_api][c_free]") {
  SonareDereverbResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.report.detected.late_decay_ratio_db =
      static_cast<decltype(r.report.detected.late_decay_ratio_db)>(1);
  r.report.detected.late_predictability =
      static_cast<decltype(r.report.detected.late_predictability)>(1);
  r.report.detected.has_late_predictability =
      static_cast<decltype(r.report.detected.has_late_predictability)>(1);
  r.report.mean_reduction_db = static_cast<decltype(r.report.mean_reduction_db)>(1);
  r.report.suppressed_fraction = static_cast<decltype(r.report.suppressed_fraction)>(1);
  r.report.wpe_predictor_norm = static_cast<decltype(r.report.wpe_predictor_norm)>(1);
  sonare_free_dereverb_result(&r);
  CHECK(r.samples == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.report.detected.late_decay_ratio_db ==
        static_cast<decltype(r.report.detected.late_decay_ratio_db)>(0));
  CHECK(r.report.detected.late_predictability ==
        static_cast<decltype(r.report.detected.late_predictability)>(0));
  CHECK(r.report.detected.has_late_predictability ==
        static_cast<decltype(r.report.detected.has_late_predictability)>(0));
  CHECK(r.report.mean_reduction_db == static_cast<decltype(r.report.mean_reduction_db)>(0));
  CHECK(r.report.suppressed_fraction == static_cast<decltype(r.report.suppressed_fraction)>(0));
  CHECK(r.report.wpe_predictor_norm == static_cast<decltype(r.report.wpe_predictor_norm)>(0));
}

TEST_CASE("sonare_free_dereverb_stereo_result leaves the result zeroed", "[c_api][c_free]") {
  SonareDereverbStereoResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.report.detected.late_decay_ratio_db =
      static_cast<decltype(r.report.detected.late_decay_ratio_db)>(1);
  r.report.detected.late_predictability =
      static_cast<decltype(r.report.detected.late_predictability)>(1);
  r.report.detected.has_late_predictability =
      static_cast<decltype(r.report.detected.has_late_predictability)>(1);
  r.report.mean_reduction_db = static_cast<decltype(r.report.mean_reduction_db)>(1);
  r.report.suppressed_fraction = static_cast<decltype(r.report.suppressed_fraction)>(1);
  r.report.wpe_predictor_norm = static_cast<decltype(r.report.wpe_predictor_norm)>(1);
  sonare_free_dereverb_stereo_result(&r);
  CHECK(r.left == nullptr);
  CHECK(r.right == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.report.detected.late_decay_ratio_db ==
        static_cast<decltype(r.report.detected.late_decay_ratio_db)>(0));
  CHECK(r.report.detected.late_predictability ==
        static_cast<decltype(r.report.detected.late_predictability)>(0));
  CHECK(r.report.detected.has_late_predictability ==
        static_cast<decltype(r.report.detected.has_late_predictability)>(0));
  CHECK(r.report.mean_reduction_db == static_cast<decltype(r.report.mean_reduction_db)>(0));
  CHECK(r.report.suppressed_fraction == static_cast<decltype(r.report.suppressed_fraction)>(0));
  CHECK(r.report.wpe_predictor_norm == static_cast<decltype(r.report.wpe_predictor_norm)>(0));
}

TEST_CASE("sonare_free_dynamic_range_result leaves the result zeroed", "[c_api][c_free]") {
  SonareDynamicRangeResult r{};
  r.dynamic_range_db = static_cast<decltype(r.dynamic_range_db)>(1);
  r.low_percentile_db = static_cast<decltype(r.low_percentile_db)>(1);
  r.high_percentile_db = static_cast<decltype(r.high_percentile_db)>(1);
  r.window_count = static_cast<decltype(r.window_count)>(1);
  sonare_free_dynamic_range_result(&r);
  CHECK(r.dynamic_range_db == static_cast<decltype(r.dynamic_range_db)>(0));
  CHECK(r.low_percentile_db == static_cast<decltype(r.low_percentile_db)>(0));
  CHECK(r.high_percentile_db == static_cast<decltype(r.high_percentile_db)>(0));
  CHECK(r.window_rms_db == nullptr);
  CHECK(r.window_count == static_cast<decltype(r.window_count)>(0));
}

TEST_CASE("sonare_free_dynamics_result leaves the result zeroed", "[c_api][c_free]") {
  SonareDynamicsResult r{};
  r.dynamic_range_db = static_cast<decltype(r.dynamic_range_db)>(1);
  r.peak_db = static_cast<decltype(r.peak_db)>(1);
  r.rms_db = static_cast<decltype(r.rms_db)>(1);
  r.crest_factor = static_cast<decltype(r.crest_factor)>(1);
  r.loudness_range_lu = static_cast<decltype(r.loudness_range_lu)>(1);
  r.is_compressed = static_cast<decltype(r.is_compressed)>(1);
  r.loudness_count = static_cast<decltype(r.loudness_count)>(1);
  sonare_free_dynamics_result(&r);
  CHECK(r.dynamic_range_db == static_cast<decltype(r.dynamic_range_db)>(0));
  CHECK(r.peak_db == static_cast<decltype(r.peak_db)>(0));
  CHECK(r.rms_db == static_cast<decltype(r.rms_db)>(0));
  CHECK(r.crest_factor == static_cast<decltype(r.crest_factor)>(0));
  CHECK(r.loudness_range_lu == static_cast<decltype(r.loudness_range_lu)>(0));
  CHECK(r.is_compressed == static_cast<decltype(r.is_compressed)>(0));
  CHECK(r.loudness_times_sec == nullptr);
  CHECK(r.loudness_rms_db == nullptr);
  CHECK(r.loudness_count == static_cast<decltype(r.loudness_count)>(0));
}

TEST_CASE("sonare_free_engine_bounce_result leaves the result zeroed", "[c_api][c_free]") {
  SonareEngineBounceResult r{};
  r.sample_count = static_cast<decltype(r.sample_count)>(1);
  r.frames = static_cast<decltype(r.frames)>(1);
  r.num_channels = static_cast<decltype(r.num_channels)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  r.integrated_lufs = static_cast<decltype(r.integrated_lufs)>(1);
  sonare_free_engine_bounce_result(&r);
  CHECK(r.interleaved == nullptr);
  CHECK(r.sample_count == static_cast<decltype(r.sample_count)>(0));
  CHECK(r.frames == static_cast<decltype(r.frames)>(0));
  CHECK(r.num_channels == static_cast<decltype(r.num_channels)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
  CHECK(r.integrated_lufs == static_cast<decltype(r.integrated_lufs)>(0));
}

TEST_CASE("sonare_free_external_stem_import_result leaves the result zeroed", "[c_api][c_free]") {
  SonareExternalStemImportResult r{};
  r.count = static_cast<decltype(r.count)>(1);
  sonare_free_external_stem_import_result(&r);
  CHECK(r.track_ids == nullptr);
  CHECK(r.clip_ids == nullptr);
  CHECK(r.count == static_cast<decltype(r.count)>(0));
}

TEST_CASE("sonare_free_hpss_result leaves the result zeroed", "[c_api][c_free]") {
  SonareHpssResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  sonare_free_hpss_result(&r);
  CHECK(r.harmonic == nullptr);
  CHECK(r.percussive == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
}

TEST_CASE("sonare_free_inverse_result leaves the result zeroed", "[c_api][c_free]") {
  SonareInverseResult r{};
  r.rows = static_cast<decltype(r.rows)>(1);
  r.n_frames = static_cast<decltype(r.n_frames)>(1);
  sonare_free_inverse_result(&r);
  CHECK(r.rows == static_cast<decltype(r.rows)>(0));
  CHECK(r.n_frames == static_cast<decltype(r.n_frames)>(0));
  CHECK(r.data == nullptr);
}

TEST_CASE("sonare_free_mastering_chain_result leaves the result zeroed", "[c_api][c_free]") {
  SonareMasteringChainResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  r.input_lufs = static_cast<decltype(r.input_lufs)>(1);
  r.output_lufs = static_cast<decltype(r.output_lufs)>(1);
  r.applied_gain_db = static_cast<decltype(r.applied_gain_db)>(1);
  r.stages_count = static_cast<decltype(r.stages_count)>(1);
  r.output_true_peak_dbtp = static_cast<decltype(r.output_true_peak_dbtp)>(1);
  r.output_loudness_range_lu = static_cast<decltype(r.output_loudness_range_lu)>(1);
  r.loudness_target_limited = static_cast<decltype(r.loudness_target_limited)>(1);
  r.stage_gain_reductions_count = static_cast<decltype(r.stage_gain_reductions_count)>(1);
  r.report.before.integrated_lufs = static_cast<decltype(r.report.before.integrated_lufs)>(1);
  r.report.before.max_momentary_lufs = static_cast<decltype(r.report.before.max_momentary_lufs)>(1);
  r.report.before.max_short_term_lufs =
      static_cast<decltype(r.report.before.max_short_term_lufs)>(1);
  r.report.before.true_peak_dbtp = static_cast<decltype(r.report.before.true_peak_dbtp)>(1);
  r.report.before.loudness_range_lu = static_cast<decltype(r.report.before.loudness_range_lu)>(1);
  r.report.after.integrated_lufs = static_cast<decltype(r.report.after.integrated_lufs)>(1);
  r.report.after.max_momentary_lufs = static_cast<decltype(r.report.after.max_momentary_lufs)>(1);
  r.report.after.max_short_term_lufs = static_cast<decltype(r.report.after.max_short_term_lufs)>(1);
  r.report.after.true_peak_dbtp = static_cast<decltype(r.report.after.true_peak_dbtp)>(1);
  r.report.after.loudness_range_lu = static_cast<decltype(r.report.after.loudness_range_lu)>(1);
  r.report.applied_gain_db = static_cast<decltype(r.report.applied_gain_db)>(1);
  r.report.max_gain_reduction_db = static_cast<decltype(r.report.max_gain_reduction_db)>(1);
  r.report.loudness_target_limited = static_cast<decltype(r.report.loudness_target_limited)>(1);
  std::memset(&r.report.band_energy_delta_db, 1, sizeof(r.report.band_energy_delta_db));
  r.non_finite_substitution_count = static_cast<decltype(r.non_finite_substitution_count)>(1);
  sonare_free_mastering_chain_result(&r);
  CHECK(r.samples == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
  CHECK(r.input_lufs == static_cast<decltype(r.input_lufs)>(0));
  CHECK(r.output_lufs == static_cast<decltype(r.output_lufs)>(0));
  CHECK(r.applied_gain_db == static_cast<decltype(r.applied_gain_db)>(0));
  CHECK(r.stages == nullptr);
  CHECK(r.stages_count == static_cast<decltype(r.stages_count)>(0));
  CHECK(r.output_true_peak_dbtp == static_cast<decltype(r.output_true_peak_dbtp)>(0));
  CHECK(r.output_loudness_range_lu == static_cast<decltype(r.output_loudness_range_lu)>(0));
  CHECK(r.loudness_target_limited == static_cast<decltype(r.loudness_target_limited)>(0));
  CHECK(r.stage_gain_reduction_stages == nullptr);
  CHECK(r.stage_gain_reduction_values == nullptr);
  CHECK(r.stage_gain_reductions_count == static_cast<decltype(r.stage_gain_reductions_count)>(0));
  CHECK(r.report.before.integrated_lufs ==
        static_cast<decltype(r.report.before.integrated_lufs)>(0));
  CHECK(r.report.before.max_momentary_lufs ==
        static_cast<decltype(r.report.before.max_momentary_lufs)>(0));
  CHECK(r.report.before.max_short_term_lufs ==
        static_cast<decltype(r.report.before.max_short_term_lufs)>(0));
  CHECK(r.report.before.true_peak_dbtp == static_cast<decltype(r.report.before.true_peak_dbtp)>(0));
  CHECK(r.report.before.loudness_range_lu ==
        static_cast<decltype(r.report.before.loudness_range_lu)>(0));
  CHECK(r.report.after.integrated_lufs == static_cast<decltype(r.report.after.integrated_lufs)>(0));
  CHECK(r.report.after.max_momentary_lufs ==
        static_cast<decltype(r.report.after.max_momentary_lufs)>(0));
  CHECK(r.report.after.max_short_term_lufs ==
        static_cast<decltype(r.report.after.max_short_term_lufs)>(0));
  CHECK(r.report.after.true_peak_dbtp == static_cast<decltype(r.report.after.true_peak_dbtp)>(0));
  CHECK(r.report.after.loudness_range_lu ==
        static_cast<decltype(r.report.after.loudness_range_lu)>(0));
  CHECK(r.report.applied_gain_db == static_cast<decltype(r.report.applied_gain_db)>(0));
  CHECK(r.report.max_gain_reduction_db == static_cast<decltype(r.report.max_gain_reduction_db)>(0));
  CHECK(r.report.loudness_target_limited ==
        static_cast<decltype(r.report.loudness_target_limited)>(0));
  for (size_t i = 0; i < sizeof(r.report.band_energy_delta_db); ++i)
    CHECK(reinterpret_cast<const unsigned char*>(&r.report.band_energy_delta_db)[i] == 0);
  CHECK(r.non_finite_substitution_count ==
        static_cast<decltype(r.non_finite_substitution_count)>(0));
}

TEST_CASE("sonare_free_mastering_chain_stereo_result leaves the result zeroed", "[c_api][c_free]") {
  SonareMasteringChainStereoResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  r.input_lufs = static_cast<decltype(r.input_lufs)>(1);
  r.output_lufs = static_cast<decltype(r.output_lufs)>(1);
  r.applied_gain_db = static_cast<decltype(r.applied_gain_db)>(1);
  r.stages_count = static_cast<decltype(r.stages_count)>(1);
  r.output_true_peak_dbtp = static_cast<decltype(r.output_true_peak_dbtp)>(1);
  r.output_loudness_range_lu = static_cast<decltype(r.output_loudness_range_lu)>(1);
  r.loudness_target_limited = static_cast<decltype(r.loudness_target_limited)>(1);
  r.stage_gain_reductions_count = static_cast<decltype(r.stage_gain_reductions_count)>(1);
  r.report.before.integrated_lufs = static_cast<decltype(r.report.before.integrated_lufs)>(1);
  r.report.before.max_momentary_lufs = static_cast<decltype(r.report.before.max_momentary_lufs)>(1);
  r.report.before.max_short_term_lufs =
      static_cast<decltype(r.report.before.max_short_term_lufs)>(1);
  r.report.before.true_peak_dbtp = static_cast<decltype(r.report.before.true_peak_dbtp)>(1);
  r.report.before.loudness_range_lu = static_cast<decltype(r.report.before.loudness_range_lu)>(1);
  r.report.after.integrated_lufs = static_cast<decltype(r.report.after.integrated_lufs)>(1);
  r.report.after.max_momentary_lufs = static_cast<decltype(r.report.after.max_momentary_lufs)>(1);
  r.report.after.max_short_term_lufs = static_cast<decltype(r.report.after.max_short_term_lufs)>(1);
  r.report.after.true_peak_dbtp = static_cast<decltype(r.report.after.true_peak_dbtp)>(1);
  r.report.after.loudness_range_lu = static_cast<decltype(r.report.after.loudness_range_lu)>(1);
  r.report.applied_gain_db = static_cast<decltype(r.report.applied_gain_db)>(1);
  r.report.max_gain_reduction_db = static_cast<decltype(r.report.max_gain_reduction_db)>(1);
  r.report.loudness_target_limited = static_cast<decltype(r.report.loudness_target_limited)>(1);
  std::memset(&r.report.band_energy_delta_db, 1, sizeof(r.report.band_energy_delta_db));
  r.non_finite_substitution_count = static_cast<decltype(r.non_finite_substitution_count)>(1);
  sonare_free_mastering_chain_stereo_result(&r);
  CHECK(r.left == nullptr);
  CHECK(r.right == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
  CHECK(r.input_lufs == static_cast<decltype(r.input_lufs)>(0));
  CHECK(r.output_lufs == static_cast<decltype(r.output_lufs)>(0));
  CHECK(r.applied_gain_db == static_cast<decltype(r.applied_gain_db)>(0));
  CHECK(r.stages == nullptr);
  CHECK(r.stages_count == static_cast<decltype(r.stages_count)>(0));
  CHECK(r.output_true_peak_dbtp == static_cast<decltype(r.output_true_peak_dbtp)>(0));
  CHECK(r.output_loudness_range_lu == static_cast<decltype(r.output_loudness_range_lu)>(0));
  CHECK(r.loudness_target_limited == static_cast<decltype(r.loudness_target_limited)>(0));
  CHECK(r.stage_gain_reduction_stages == nullptr);
  CHECK(r.stage_gain_reduction_values == nullptr);
  CHECK(r.stage_gain_reductions_count == static_cast<decltype(r.stage_gain_reductions_count)>(0));
  CHECK(r.report.before.integrated_lufs ==
        static_cast<decltype(r.report.before.integrated_lufs)>(0));
  CHECK(r.report.before.max_momentary_lufs ==
        static_cast<decltype(r.report.before.max_momentary_lufs)>(0));
  CHECK(r.report.before.max_short_term_lufs ==
        static_cast<decltype(r.report.before.max_short_term_lufs)>(0));
  CHECK(r.report.before.true_peak_dbtp == static_cast<decltype(r.report.before.true_peak_dbtp)>(0));
  CHECK(r.report.before.loudness_range_lu ==
        static_cast<decltype(r.report.before.loudness_range_lu)>(0));
  CHECK(r.report.after.integrated_lufs == static_cast<decltype(r.report.after.integrated_lufs)>(0));
  CHECK(r.report.after.max_momentary_lufs ==
        static_cast<decltype(r.report.after.max_momentary_lufs)>(0));
  CHECK(r.report.after.max_short_term_lufs ==
        static_cast<decltype(r.report.after.max_short_term_lufs)>(0));
  CHECK(r.report.after.true_peak_dbtp == static_cast<decltype(r.report.after.true_peak_dbtp)>(0));
  CHECK(r.report.after.loudness_range_lu ==
        static_cast<decltype(r.report.after.loudness_range_lu)>(0));
  CHECK(r.report.applied_gain_db == static_cast<decltype(r.report.applied_gain_db)>(0));
  CHECK(r.report.max_gain_reduction_db == static_cast<decltype(r.report.max_gain_reduction_db)>(0));
  CHECK(r.report.loudness_target_limited ==
        static_cast<decltype(r.report.loudness_target_limited)>(0));
  for (size_t i = 0; i < sizeof(r.report.band_energy_delta_db); ++i)
    CHECK(reinterpret_cast<const unsigned char*>(&r.report.band_energy_delta_db)[i] == 0);
  CHECK(r.non_finite_substitution_count ==
        static_cast<decltype(r.non_finite_substitution_count)>(0));
}

TEST_CASE("sonare_free_mastering_result leaves the result zeroed", "[c_api][c_free]") {
  SonareMasteringResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  r.input_lufs = static_cast<decltype(r.input_lufs)>(1);
  r.output_lufs = static_cast<decltype(r.output_lufs)>(1);
  r.applied_gain_db = static_cast<decltype(r.applied_gain_db)>(1);
  r.latency_samples = static_cast<decltype(r.latency_samples)>(1);
  r.loudness_target_limited = static_cast<decltype(r.loudness_target_limited)>(1);
  r.non_finite_substitution_count = static_cast<decltype(r.non_finite_substitution_count)>(1);
  sonare_free_mastering_result(&r);
  CHECK(r.samples == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
  CHECK(r.input_lufs == static_cast<decltype(r.input_lufs)>(0));
  CHECK(r.output_lufs == static_cast<decltype(r.output_lufs)>(0));
  CHECK(r.applied_gain_db == static_cast<decltype(r.applied_gain_db)>(0));
  CHECK(r.latency_samples == static_cast<decltype(r.latency_samples)>(0));
  CHECK(r.loudness_target_limited == static_cast<decltype(r.loudness_target_limited)>(0));
  CHECK(r.non_finite_substitution_count ==
        static_cast<decltype(r.non_finite_substitution_count)>(0));
}

TEST_CASE("sonare_free_mastering_stereo_result leaves the result zeroed", "[c_api][c_free]") {
  SonareMasteringStereoResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  r.input_lufs = static_cast<decltype(r.input_lufs)>(1);
  r.output_lufs = static_cast<decltype(r.output_lufs)>(1);
  r.applied_gain_db = static_cast<decltype(r.applied_gain_db)>(1);
  r.latency_samples = static_cast<decltype(r.latency_samples)>(1);
  r.loudness_target_limited = static_cast<decltype(r.loudness_target_limited)>(1);
  r.non_finite_substitution_count = static_cast<decltype(r.non_finite_substitution_count)>(1);
  sonare_free_mastering_stereo_result(&r);
  CHECK(r.left == nullptr);
  CHECK(r.right == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
  CHECK(r.input_lufs == static_cast<decltype(r.input_lufs)>(0));
  CHECK(r.output_lufs == static_cast<decltype(r.output_lufs)>(0));
  CHECK(r.applied_gain_db == static_cast<decltype(r.applied_gain_db)>(0));
  CHECK(r.latency_samples == static_cast<decltype(r.latency_samples)>(0));
  CHECK(r.loudness_target_limited == static_cast<decltype(r.loudness_target_limited)>(0));
  CHECK(r.non_finite_substitution_count ==
        static_cast<decltype(r.non_finite_substitution_count)>(0));
}

TEST_CASE("sonare_free_mel_result leaves the result zeroed", "[c_api][c_free]") {
  SonareMelResult r{};
  r.n_mels = static_cast<decltype(r.n_mels)>(1);
  r.n_frames = static_cast<decltype(r.n_frames)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  r.hop_length = static_cast<decltype(r.hop_length)>(1);
  r.n_fft = static_cast<decltype(r.n_fft)>(1);
  r.fmin_hz = static_cast<decltype(r.fmin_hz)>(1);
  r.fmax_hz = static_cast<decltype(r.fmax_hz)>(1);
  r.htk = static_cast<decltype(r.htk)>(1);
  r.is_db = static_cast<decltype(r.is_db)>(1);
  sonare_free_mel_result(&r);
  CHECK(r.n_mels == static_cast<decltype(r.n_mels)>(0));
  CHECK(r.n_frames == static_cast<decltype(r.n_frames)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
  CHECK(r.hop_length == static_cast<decltype(r.hop_length)>(0));
  CHECK(r.n_fft == static_cast<decltype(r.n_fft)>(0));
  CHECK(r.fmin_hz == static_cast<decltype(r.fmin_hz)>(0));
  CHECK(r.fmax_hz == static_cast<decltype(r.fmax_hz)>(0));
  CHECK(r.htk == static_cast<decltype(r.htk)>(0));
  CHECK(r.is_db == static_cast<decltype(r.is_db)>(0));
  CHECK(r.power == nullptr);
  CHECK(r.db == nullptr);
}

TEST_CASE("sonare_free_melody_result leaves the result zeroed", "[c_api][c_free]") {
  SonareMelodyResult r{};
  r.point_count = static_cast<decltype(r.point_count)>(1);
  r.pitch_range_octaves = static_cast<decltype(r.pitch_range_octaves)>(1);
  r.pitch_stability = static_cast<decltype(r.pitch_stability)>(1);
  r.mean_frequency_hz = static_cast<decltype(r.mean_frequency_hz)>(1);
  r.vibrato_rate_hz = static_cast<decltype(r.vibrato_rate_hz)>(1);
  sonare_free_melody_result(&r);
  CHECK(r.points == nullptr);
  CHECK(r.point_count == static_cast<decltype(r.point_count)>(0));
  CHECK(r.pitch_range_octaves == static_cast<decltype(r.pitch_range_octaves)>(0));
  CHECK(r.pitch_stability == static_cast<decltype(r.pitch_stability)>(0));
  CHECK(r.mean_frequency_hz == static_cast<decltype(r.mean_frequency_hz)>(0));
  CHECK(r.vibrato_rate_hz == static_cast<decltype(r.vibrato_rate_hz)>(0));
}

TEST_CASE("sonare_free_mfcc_result leaves the result zeroed", "[c_api][c_free]") {
  SonareMfccResult r{};
  r.n_mfcc = static_cast<decltype(r.n_mfcc)>(1);
  r.n_frames = static_cast<decltype(r.n_frames)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  r.hop_length = static_cast<decltype(r.hop_length)>(1);
  r.n_fft = static_cast<decltype(r.n_fft)>(1);
  r.n_mels = static_cast<decltype(r.n_mels)>(1);
  r.fmin_hz = static_cast<decltype(r.fmin_hz)>(1);
  r.fmax_hz = static_cast<decltype(r.fmax_hz)>(1);
  r.htk = static_cast<decltype(r.htk)>(1);
  r.is_db = static_cast<decltype(r.is_db)>(1);
  r.lifter = static_cast<decltype(r.lifter)>(1);
  sonare_free_mfcc_result(&r);
  CHECK(r.n_mfcc == static_cast<decltype(r.n_mfcc)>(0));
  CHECK(r.n_frames == static_cast<decltype(r.n_frames)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
  CHECK(r.hop_length == static_cast<decltype(r.hop_length)>(0));
  CHECK(r.n_fft == static_cast<decltype(r.n_fft)>(0));
  CHECK(r.n_mels == static_cast<decltype(r.n_mels)>(0));
  CHECK(r.fmin_hz == static_cast<decltype(r.fmin_hz)>(0));
  CHECK(r.fmax_hz == static_cast<decltype(r.fmax_hz)>(0));
  CHECK(r.htk == static_cast<decltype(r.htk)>(0));
  CHECK(r.is_db == static_cast<decltype(r.is_db)>(0));
  CHECK(r.lifter == static_cast<decltype(r.lifter)>(0));
  CHECK(r.coefficients == nullptr);
}

TEST_CASE("sonare_free_normalize_stereo_result leaves the result zeroed", "[c_api][c_free]") {
  SonareNormalizeStereoResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.applied_gain_db = static_cast<decltype(r.applied_gain_db)>(1);
  sonare_free_normalize_stereo_result(&r);
  CHECK(r.left == nullptr);
  CHECK(r.right == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.applied_gain_db == static_cast<decltype(r.applied_gain_db)>(0));
}

TEST_CASE("sonare_free_note_objects_result leaves the result zeroed", "[c_api][c_free]") {
  SonareNoteObjectsResult r{};
  r.count = static_cast<decltype(r.count)>(1);
  r.amplitude_count = static_cast<decltype(r.amplitude_count)>(1);
  r.envelope_count = static_cast<decltype(r.envelope_count)>(1);
  sonare_free_note_objects_result(&r);
  CHECK(r.notes == nullptr);
  CHECK(r.count == static_cast<decltype(r.count)>(0));
  CHECK(r.amplitude == nullptr);
  CHECK(r.amplitude_count == static_cast<decltype(r.amplitude_count)>(0));
  CHECK(r.envelopes == nullptr);
  CHECK(r.envelope_count == static_cast<decltype(r.envelope_count)>(0));
}

TEST_CASE("sonare_free_note_segments_result leaves the result zeroed", "[c_api][c_free]") {
  SonareNoteSegmentsResult r{};
  r.count = static_cast<decltype(r.count)>(1);
  sonare_free_note_segments_result(&r);
  CHECK(r.segments == nullptr);
  CHECK(r.count == static_cast<decltype(r.count)>(0));
}

TEST_CASE("sonare_free_percussive_events_result leaves the result zeroed", "[c_api][c_free]") {
  SonarePercussiveEventsResult r{};
  r.count = static_cast<decltype(r.count)>(1);
  sonare_free_percussive_events_result(&r);
  CHECK(r.events == nullptr);
  CHECK(r.count == static_cast<decltype(r.count)>(0));
}

TEST_CASE("sonare_free_phase_scope_result leaves the result zeroed", "[c_api][c_free]") {
  SonarePhaseScopeResult r{};
  r.point_count = static_cast<decltype(r.point_count)>(1);
  r.correlation = static_cast<decltype(r.correlation)>(1);
  r.average_abs_angle_rad = static_cast<decltype(r.average_abs_angle_rad)>(1);
  r.max_radius = static_cast<decltype(r.max_radius)>(1);
  sonare_free_phase_scope_result(&r);
  CHECK(r.points == nullptr);
  CHECK(r.point_count == static_cast<decltype(r.point_count)>(0));
  CHECK(r.correlation == static_cast<decltype(r.correlation)>(0));
  CHECK(r.average_abs_angle_rad == static_cast<decltype(r.average_abs_angle_rad)>(0));
  CHECK(r.max_radius == static_cast<decltype(r.max_radius)>(0));
}

TEST_CASE("sonare_free_pitch_decomposition_result leaves the result zeroed", "[c_api][c_free]") {
  SonarePitchDecompositionResult r{};
  r.center_hz = static_cast<decltype(r.center_hz)>(1);
  r.count = static_cast<decltype(r.count)>(1);
  sonare_free_pitch_decomposition_result(&r);
  CHECK(r.center_hz == static_cast<decltype(r.center_hz)>(0));
  CHECK(r.drift_cents == nullptr);
  CHECK(r.vibrato_cents == nullptr);
  CHECK(r.count == static_cast<decltype(r.count)>(0));
}

TEST_CASE("sonare_free_pitch_result leaves the result zeroed", "[c_api][c_free]") {
  SonarePitchResult r{};
  r.n_frames = static_cast<decltype(r.n_frames)>(1);
  r.median_f0 = static_cast<decltype(r.median_f0)>(1);
  r.mean_f0 = static_cast<decltype(r.mean_f0)>(1);
  sonare_free_pitch_result(&r);
  CHECK(r.n_frames == static_cast<decltype(r.n_frames)>(0));
  CHECK(r.f0 == nullptr);
  CHECK(r.voiced_prob == nullptr);
  CHECK(r.voiced_flag == nullptr);
  CHECK(r.median_f0 == static_cast<decltype(r.median_f0)>(0));
  CHECK(r.mean_f0 == static_cast<decltype(r.mean_f0)>(0));
}

TEST_CASE("sonare_free_project_assist_sidecar leaves the result zeroed", "[c_api][c_free]") {
  SonareProjectAssistSidecar r{};
  r.target_track_id = static_cast<decltype(r.target_track_id)>(1);
  r.region_start_ppq = static_cast<decltype(r.region_start_ppq)>(1);
  r.region_end_ppq = static_cast<decltype(r.region_end_ppq)>(1);
  r.payload_len = static_cast<decltype(r.payload_len)>(1);
  sonare_free_project_assist_sidecar(&r);
  CHECK(r.module_id == nullptr);
  CHECK(r.target_track_id == static_cast<decltype(r.target_track_id)>(0));
  CHECK(r.region_start_ppq == static_cast<decltype(r.region_start_ppq)>(0));
  CHECK(r.region_end_ppq == static_cast<decltype(r.region_end_ppq)>(0));
  CHECK(r.payload == nullptr);
  CHECK(r.payload_len == static_cast<decltype(r.payload_len)>(0));
}

TEST_CASE("sonare_free_project_audio_source_metadata leaves the result zeroed", "[c_api][c_free]") {
  SonareProjectAudioSourceMetadata r{};
  sonare_free_project_audio_source_metadata(&r);
  CHECK(r.content_hash == nullptr);
  CHECK(r.external_stem_role == nullptr);
}

TEST_CASE("sonare_free_project_compile_result leaves the result zeroed", "[c_api][c_free]") {
  SonareProjectCompileResult r{};
  r.diagnostic_count = static_cast<decltype(r.diagnostic_count)>(1);
  r.has_timeline = static_cast<decltype(r.has_timeline)>(1);
  sonare_free_project_compile_result(&r);
  CHECK(r.diagnostics == nullptr);
  CHECK(r.diagnostic_count == static_cast<decltype(r.diagnostic_count)>(0));
  CHECK(r.messages == nullptr);
  CHECK(r.has_timeline == static_cast<decltype(r.has_timeline)>(0));
}

TEST_CASE("sonare_free_project_vocal_edit_dependencies_result leaves the result zeroed",
          "[c_api][c_free]") {
  SonareProjectVocalEditDependenciesResult r{};
  r.dependency_count = static_cast<decltype(r.dependency_count)>(1);
  sonare_free_project_vocal_edit_dependencies_result(&r);
  CHECK(r.dependencies == nullptr);
  CHECK(r.dependency_count == static_cast<decltype(r.dependency_count)>(0));
}

TEST_CASE("sonare_free_project_vocal_rehydrate_result leaves the result zeroed",
          "[c_api][c_free]") {
  SonareProjectVocalRehydrateResult r{};
  r.item_count = static_cast<decltype(r.item_count)>(1);
  sonare_free_project_vocal_rehydrate_result(&r);
  CHECK(r.items == nullptr);
  CHECK(r.item_count == static_cast<decltype(r.item_count)>(0));
}

TEST_CASE("sonare_free_reassigned_spectrogram_result leaves the result zeroed", "[c_api][c_free]") {
  SonareReassignedSpectrogramResult r{};
  r.n_bins = static_cast<decltype(r.n_bins)>(1);
  r.n_frames = static_cast<decltype(r.n_frames)>(1);
  sonare_free_reassigned_spectrogram_result(&r);
  CHECK(r.n_bins == static_cast<decltype(r.n_bins)>(0));
  CHECK(r.n_frames == static_cast<decltype(r.n_frames)>(0));
  CHECK(r.magnitude == nullptr);
  CHECK(r.times == nullptr);
  CHECK(r.frequencies == nullptr);
}

TEST_CASE("sonare_free_rhythm_result leaves the result zeroed", "[c_api][c_free]") {
  SonareRhythmResult r{};
  r.bpm = static_cast<decltype(r.bpm)>(1);
  r.time_signature.numerator = static_cast<decltype(r.time_signature.numerator)>(1);
  r.time_signature.denominator = static_cast<decltype(r.time_signature.denominator)>(1);
  r.time_signature.confidence = static_cast<decltype(r.time_signature.confidence)>(1);
  r.groove_type = static_cast<decltype(r.groove_type)>(1);
  r.syncopation = static_cast<decltype(r.syncopation)>(1);
  r.pattern_regularity = static_cast<decltype(r.pattern_regularity)>(1);
  r.tempo_stability = static_cast<decltype(r.tempo_stability)>(1);
  r.beat_interval_count = static_cast<decltype(r.beat_interval_count)>(1);
  sonare_free_rhythm_result(&r);
  CHECK(r.bpm == static_cast<decltype(r.bpm)>(0));
  CHECK(r.time_signature.numerator == static_cast<decltype(r.time_signature.numerator)>(0));
  CHECK(r.time_signature.denominator == static_cast<decltype(r.time_signature.denominator)>(0));
  CHECK(r.time_signature.confidence == static_cast<decltype(r.time_signature.confidence)>(0));
  CHECK(r.groove_type == static_cast<decltype(r.groove_type)>(0));
  CHECK(r.syncopation == static_cast<decltype(r.syncopation)>(0));
  CHECK(r.pattern_regularity == static_cast<decltype(r.pattern_regularity)>(0));
  CHECK(r.tempo_stability == static_cast<decltype(r.tempo_stability)>(0));
  CHECK(r.beat_intervals == nullptr);
  CHECK(r.beat_interval_count == static_cast<decltype(r.beat_interval_count)>(0));
}

TEST_CASE("sonare_free_section_result leaves the result zeroed", "[c_api][c_free]") {
  SonareSectionResult r{};
  r.section_count = static_cast<decltype(r.section_count)>(1);
  sonare_free_section_result(&r);
  CHECK(r.sections == nullptr);
  CHECK(r.section_count == static_cast<decltype(r.section_count)>(0));
  CHECK(r.form == nullptr);
}

TEST_CASE("sonare_free_segment_indices leaves the result zeroed", "[c_api][c_free]") {
  SonareSegmentIndices r{};
  r.count = static_cast<decltype(r.count)>(1);
  sonare_free_segment_indices(&r);
  CHECK(r.values == nullptr);
  CHECK(r.count == static_cast<decltype(r.count)>(0));
}

TEST_CASE("sonare_free_segment_matrix leaves the result zeroed", "[c_api][c_free]") {
  SonareSegmentMatrix r{};
  r.rows = static_cast<decltype(r.rows)>(1);
  r.cols = static_cast<decltype(r.cols)>(1);
  sonare_free_segment_matrix(&r);
  CHECK(r.rows == static_cast<decltype(r.rows)>(0));
  CHECK(r.cols == static_cast<decltype(r.cols)>(0));
  CHECK(r.values == nullptr);
}

TEST_CASE("sonare_free_spectrum_result leaves the result zeroed", "[c_api][c_free]") {
  SonareSpectrumResult r{};
  r.bin_count = static_cast<decltype(r.bin_count)>(1);
  r.n_fft = static_cast<decltype(r.n_fft)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  sonare_free_spectrum_result(&r);
  CHECK(r.frequencies == nullptr);
  CHECK(r.magnitude == nullptr);
  CHECK(r.power == nullptr);
  CHECK(r.db == nullptr);
  CHECK(r.bin_count == static_cast<decltype(r.bin_count)>(0));
  CHECK(r.n_fft == static_cast<decltype(r.n_fft)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
}

TEST_CASE("sonare_free_stft_result leaves the result zeroed", "[c_api][c_free]") {
  SonareStftResult r{};
  r.n_bins = static_cast<decltype(r.n_bins)>(1);
  r.n_frames = static_cast<decltype(r.n_frames)>(1);
  r.n_fft = static_cast<decltype(r.n_fft)>(1);
  r.hop_length = static_cast<decltype(r.hop_length)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  sonare_free_stft_result(&r);
  CHECK(r.n_bins == static_cast<decltype(r.n_bins)>(0));
  CHECK(r.n_frames == static_cast<decltype(r.n_frames)>(0));
  CHECK(r.n_fft == static_cast<decltype(r.n_fft)>(0));
  CHECK(r.hop_length == static_cast<decltype(r.hop_length)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
  CHECK(r.magnitude == nullptr);
  CHECK(r.power == nullptr);
}

TEST_CASE("sonare_free_stream_frames leaves the result zeroed", "[c_api][c_free]") {
  SonareStreamFrames r{};
  r.n_frames = static_cast<decltype(r.n_frames)>(1);
  r.n_mels = static_cast<decltype(r.n_mels)>(1);
  r.feature_flags = static_cast<decltype(r.feature_flags)>(1);
  r.n_chroma = static_cast<decltype(r.n_chroma)>(1);
  sonare_free_stream_frames(&r);
  CHECK(r.n_frames == static_cast<decltype(r.n_frames)>(0));
  CHECK(r.n_mels == static_cast<decltype(r.n_mels)>(0));
  CHECK(r.timestamps_sec == nullptr);
  CHECK(r.mel == nullptr);
  CHECK(r.chroma == nullptr);
  CHECK(r.onset_strength == nullptr);
  CHECK(r.rms_energy == nullptr);
  CHECK(r.spectral_centroid == nullptr);
  CHECK(r.spectral_flatness == nullptr);
  CHECK(r.chord_root == nullptr);
  CHECK(r.chord_quality == nullptr);
  CHECK(r.chord_confidence == nullptr);
  CHECK(r.feature_flags == static_cast<decltype(r.feature_flags)>(0));
  CHECK(r.n_chroma == static_cast<decltype(r.n_chroma)>(0));
}

TEST_CASE("sonare_free_stream_frames_i16 leaves the result zeroed", "[c_api][c_free]") {
  SonareStreamFramesI16 r{};
  r.n_frames = static_cast<decltype(r.n_frames)>(1);
  r.n_mels = static_cast<decltype(r.n_mels)>(1);
  r.feature_flags = static_cast<decltype(r.feature_flags)>(1);
  r.n_chroma = static_cast<decltype(r.n_chroma)>(1);
  sonare_free_stream_frames_i16(&r);
  CHECK(r.n_frames == static_cast<decltype(r.n_frames)>(0));
  CHECK(r.n_mels == static_cast<decltype(r.n_mels)>(0));
  CHECK(r.timestamps_sec == nullptr);
  CHECK(r.mel == nullptr);
  CHECK(r.chroma == nullptr);
  CHECK(r.onset_strength == nullptr);
  CHECK(r.rms_energy == nullptr);
  CHECK(r.spectral_centroid == nullptr);
  CHECK(r.spectral_flatness == nullptr);
  CHECK(r.feature_flags == static_cast<decltype(r.feature_flags)>(0));
  CHECK(r.n_chroma == static_cast<decltype(r.n_chroma)>(0));
}

TEST_CASE("sonare_free_stream_frames_u8 leaves the result zeroed", "[c_api][c_free]") {
  SonareStreamFramesU8 r{};
  r.n_frames = static_cast<decltype(r.n_frames)>(1);
  r.n_mels = static_cast<decltype(r.n_mels)>(1);
  r.feature_flags = static_cast<decltype(r.feature_flags)>(1);
  r.n_chroma = static_cast<decltype(r.n_chroma)>(1);
  sonare_free_stream_frames_u8(&r);
  CHECK(r.n_frames == static_cast<decltype(r.n_frames)>(0));
  CHECK(r.n_mels == static_cast<decltype(r.n_mels)>(0));
  CHECK(r.timestamps_sec == nullptr);
  CHECK(r.mel == nullptr);
  CHECK(r.chroma == nullptr);
  CHECK(r.onset_strength == nullptr);
  CHECK(r.rms_energy == nullptr);
  CHECK(r.spectral_centroid == nullptr);
  CHECK(r.spectral_flatness == nullptr);
  CHECK(r.feature_flags == static_cast<decltype(r.feature_flags)>(0));
  CHECK(r.n_chroma == static_cast<decltype(r.n_chroma)>(0));
}

TEST_CASE("sonare_free_stream_stats leaves the result zeroed", "[c_api][c_free]") {
  SonareStreamStats r{};
  r.total_frames = static_cast<decltype(r.total_frames)>(1);
  r.total_samples = static_cast<decltype(r.total_samples)>(1);
  r.duration_sec = static_cast<decltype(r.duration_sec)>(1);
  r.pending_frames = static_cast<decltype(r.pending_frames)>(1);
  r.dropped_output_frames = static_cast<decltype(r.dropped_output_frames)>(1);
  r.bpm = static_cast<decltype(r.bpm)>(1);
  r.bpm_confidence = static_cast<decltype(r.bpm_confidence)>(1);
  r.bpm_candidate_count = static_cast<decltype(r.bpm_candidate_count)>(1);
  r.key = static_cast<decltype(r.key)>(1);
  r.key_minor = static_cast<decltype(r.key_minor)>(1);
  r.key_confidence = static_cast<decltype(r.key_confidence)>(1);
  r.chord_root = static_cast<decltype(r.chord_root)>(1);
  r.chord_quality = static_cast<decltype(r.chord_quality)>(1);
  r.chord_confidence = static_cast<decltype(r.chord_confidence)>(1);
  r.chord_start_sec = static_cast<decltype(r.chord_start_sec)>(1);
  r.current_bar = static_cast<decltype(r.current_bar)>(1);
  r.bar_duration_sec = static_cast<decltype(r.bar_duration_sec)>(1);
  r.chord_progression_count = static_cast<decltype(r.chord_progression_count)>(1);
  r.bar_chord_progression_count = static_cast<decltype(r.bar_chord_progression_count)>(1);
  r.pattern_length = static_cast<decltype(r.pattern_length)>(1);
  r.voted_pattern_count = static_cast<decltype(r.voted_pattern_count)>(1);
  std::memset(&r.detected_pattern_name, 1, sizeof(r.detected_pattern_name));
  r.detected_pattern_score = static_cast<decltype(r.detected_pattern_score)>(1);
  r.all_pattern_scores_count = static_cast<decltype(r.all_pattern_scores_count)>(1);
  r.accumulated_sec = static_cast<decltype(r.accumulated_sec)>(1);
  r.used_frames = static_cast<decltype(r.used_frames)>(1);
  r.updated = static_cast<decltype(r.updated)>(1);
  r.dropped_chord_progression_entries =
      static_cast<decltype(r.dropped_chord_progression_entries)>(1);
  r.dropped_bar_progression_entries = static_cast<decltype(r.dropped_bar_progression_entries)>(1);
  r.non_finite_discard_blocks = static_cast<decltype(r.non_finite_discard_blocks)>(1);
  sonare_free_stream_stats(&r);
  CHECK(r.total_frames == static_cast<decltype(r.total_frames)>(0));
  CHECK(r.total_samples == static_cast<decltype(r.total_samples)>(0));
  CHECK(r.duration_sec == static_cast<decltype(r.duration_sec)>(0));
  CHECK(r.pending_frames == static_cast<decltype(r.pending_frames)>(0));
  CHECK(r.dropped_output_frames == static_cast<decltype(r.dropped_output_frames)>(0));
  CHECK(r.bpm == static_cast<decltype(r.bpm)>(0));
  CHECK(r.bpm_confidence == static_cast<decltype(r.bpm_confidence)>(0));
  CHECK(r.bpm_candidate_count == static_cast<decltype(r.bpm_candidate_count)>(0));
  CHECK(r.key == static_cast<decltype(r.key)>(0));
  CHECK(r.key_minor == static_cast<decltype(r.key_minor)>(0));
  CHECK(r.key_confidence == static_cast<decltype(r.key_confidence)>(0));
  CHECK(r.chord_root == static_cast<decltype(r.chord_root)>(0));
  CHECK(r.chord_quality == static_cast<decltype(r.chord_quality)>(0));
  CHECK(r.chord_confidence == static_cast<decltype(r.chord_confidence)>(0));
  CHECK(r.chord_start_sec == static_cast<decltype(r.chord_start_sec)>(0));
  CHECK(r.current_bar == static_cast<decltype(r.current_bar)>(0));
  CHECK(r.bar_duration_sec == static_cast<decltype(r.bar_duration_sec)>(0));
  CHECK(r.chord_progression_count == static_cast<decltype(r.chord_progression_count)>(0));
  CHECK(r.chord_progression == nullptr);
  CHECK(r.bar_chord_progression_count == static_cast<decltype(r.bar_chord_progression_count)>(0));
  CHECK(r.bar_chord_progression == nullptr);
  CHECK(r.pattern_length == static_cast<decltype(r.pattern_length)>(0));
  CHECK(r.voted_pattern_count == static_cast<decltype(r.voted_pattern_count)>(0));
  CHECK(r.voted_pattern == nullptr);
  for (size_t i = 0; i < sizeof(r.detected_pattern_name); ++i)
    CHECK(reinterpret_cast<const unsigned char*>(&r.detected_pattern_name)[i] == 0);
  CHECK(r.detected_pattern_score == static_cast<decltype(r.detected_pattern_score)>(0));
  CHECK(r.all_pattern_scores_count == static_cast<decltype(r.all_pattern_scores_count)>(0));
  CHECK(r.all_pattern_scores == nullptr);
  CHECK(r.accumulated_sec == static_cast<decltype(r.accumulated_sec)>(0));
  CHECK(r.used_frames == static_cast<decltype(r.used_frames)>(0));
  CHECK(r.updated == static_cast<decltype(r.updated)>(0));
  CHECK(r.dropped_chord_progression_entries ==
        static_cast<decltype(r.dropped_chord_progression_entries)>(0));
  CHECK(r.dropped_bar_progression_entries ==
        static_cast<decltype(r.dropped_bar_progression_entries)>(0));
  CHECK(r.non_finite_discard_blocks == static_cast<decltype(r.non_finite_discard_blocks)>(0));
}

TEST_CASE("sonare_free_string_array leaves the result zeroed", "[c_api][c_free]") {
  SonareStringArray r{};
  r.count = static_cast<decltype(r.count)>(1);
  sonare_free_string_array(&r);
  CHECK(r.items == nullptr);
  CHECK(r.count == static_cast<decltype(r.count)>(0));
}

TEST_CASE("sonare_free_timbre_result leaves the result zeroed", "[c_api][c_free]") {
  SonareTimbreResult r{};
  r.brightness = static_cast<decltype(r.brightness)>(1);
  r.warmth = static_cast<decltype(r.warmth)>(1);
  r.density = static_cast<decltype(r.density)>(1);
  r.roughness = static_cast<decltype(r.roughness)>(1);
  r.complexity = static_cast<decltype(r.complexity)>(1);
  r.spectral_centroid_count = static_cast<decltype(r.spectral_centroid_count)>(1);
  r.spectral_flatness_count = static_cast<decltype(r.spectral_flatness_count)>(1);
  r.spectral_rolloff_count = static_cast<decltype(r.spectral_rolloff_count)>(1);
  r.timbre_over_time_count = static_cast<decltype(r.timbre_over_time_count)>(1);
  sonare_free_timbre_result(&r);
  CHECK(r.brightness == static_cast<decltype(r.brightness)>(0));
  CHECK(r.warmth == static_cast<decltype(r.warmth)>(0));
  CHECK(r.density == static_cast<decltype(r.density)>(0));
  CHECK(r.roughness == static_cast<decltype(r.roughness)>(0));
  CHECK(r.complexity == static_cast<decltype(r.complexity)>(0));
  CHECK(r.spectral_centroid == nullptr);
  CHECK(r.spectral_centroid_count == static_cast<decltype(r.spectral_centroid_count)>(0));
  CHECK(r.spectral_flatness == nullptr);
  CHECK(r.spectral_flatness_count == static_cast<decltype(r.spectral_flatness_count)>(0));
  CHECK(r.spectral_rolloff == nullptr);
  CHECK(r.spectral_rolloff_count == static_cast<decltype(r.spectral_rolloff_count)>(0));
  CHECK(r.timbre_over_time == nullptr);
  CHECK(r.timbre_over_time_count == static_cast<decltype(r.timbre_over_time_count)>(0));
}

TEST_CASE("sonare_free_transcribe_result leaves the result zeroed", "[c_api][c_free]") {
  SonareTranscribeResult r{};
  r.tuning_cents = static_cast<decltype(r.tuning_cents)>(1);
  r.count = static_cast<decltype(r.count)>(1);
  r.note_count = static_cast<decltype(r.note_count)>(1);
  r.tempo_bpm = static_cast<decltype(r.tempo_bpm)>(1);
  sonare_free_transcribe_result(&r);
  CHECK(r.tuning_cents == static_cast<decltype(r.tuning_cents)>(0));
  CHECK(r.events == nullptr);
  CHECK(r.count == static_cast<decltype(r.count)>(0));
  CHECK(r.note_count == static_cast<decltype(r.note_count)>(0));
  CHECK(r.tempo_bpm == static_cast<decltype(r.tempo_bpm)>(0));
}

TEST_CASE("sonare_free_trim_silence_result leaves the result zeroed", "[c_api][c_free]") {
  SonareTrimSilenceResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.report.range.first = static_cast<decltype(r.report.range.first)>(1);
  r.report.range.last_exclusive = static_cast<decltype(r.report.range.last_exclusive)>(1);
  r.report.removed_head_samples = static_cast<decltype(r.report.removed_head_samples)>(1);
  r.report.removed_tail_samples = static_cast<decltype(r.report.removed_tail_samples)>(1);
  sonare_free_trim_silence_result(&r);
  CHECK(r.samples == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.report.range.first == static_cast<decltype(r.report.range.first)>(0));
  CHECK(r.report.range.last_exclusive == static_cast<decltype(r.report.range.last_exclusive)>(0));
  CHECK(r.report.removed_head_samples == static_cast<decltype(r.report.removed_head_samples)>(0));
  CHECK(r.report.removed_tail_samples == static_cast<decltype(r.report.removed_tail_samples)>(0));
}

TEST_CASE("sonare_free_trim_silence_stereo_result leaves the result zeroed", "[c_api][c_free]") {
  SonareTrimSilenceStereoResult r{};
  r.length = static_cast<decltype(r.length)>(1);
  r.report.range.first = static_cast<decltype(r.report.range.first)>(1);
  r.report.range.last_exclusive = static_cast<decltype(r.report.range.last_exclusive)>(1);
  r.report.removed_head_samples = static_cast<decltype(r.report.removed_head_samples)>(1);
  r.report.removed_tail_samples = static_cast<decltype(r.report.removed_tail_samples)>(1);
  r.left_range.first = static_cast<decltype(r.left_range.first)>(1);
  r.left_range.last_exclusive = static_cast<decltype(r.left_range.last_exclusive)>(1);
  r.right_range.first = static_cast<decltype(r.right_range.first)>(1);
  r.right_range.last_exclusive = static_cast<decltype(r.right_range.last_exclusive)>(1);
  sonare_free_trim_silence_stereo_result(&r);
  CHECK(r.left == nullptr);
  CHECK(r.right == nullptr);
  CHECK(r.length == static_cast<decltype(r.length)>(0));
  CHECK(r.report.range.first == static_cast<decltype(r.report.range.first)>(0));
  CHECK(r.report.range.last_exclusive == static_cast<decltype(r.report.range.last_exclusive)>(0));
  CHECK(r.report.removed_head_samples == static_cast<decltype(r.report.removed_head_samples)>(0));
  CHECK(r.report.removed_tail_samples == static_cast<decltype(r.report.removed_tail_samples)>(0));
  CHECK(r.left_range.first == static_cast<decltype(r.left_range.first)>(0));
  CHECK(r.left_range.last_exclusive == static_cast<decltype(r.left_range.last_exclusive)>(0));
  CHECK(r.right_range.first == static_cast<decltype(r.right_range.first)>(0));
  CHECK(r.right_range.last_exclusive == static_cast<decltype(r.right_range.last_exclusive)>(0));
}

TEST_CASE("sonare_free_vectorscope_result leaves the result zeroed", "[c_api][c_free]") {
  SonareVectorscopeResult r{};
  r.point_count = static_cast<decltype(r.point_count)>(1);
  sonare_free_vectorscope_result(&r);
  CHECK(r.points == nullptr);
  CHECK(r.point_count == static_cast<decltype(r.point_count)>(0));
}

TEST_CASE("sonare_free_vocal_analysis_result leaves the result zeroed", "[c_api][c_free]") {
  SonareVocalAnalysisResult r{};
  r.analysis.struct_size = static_cast<decltype(r.analysis.struct_size)>(1);
  r.analysis.schema_version = static_cast<decltype(r.analysis.schema_version)>(1);
  r.analysis.frame_origin_sample = static_cast<decltype(r.analysis.frame_origin_sample)>(1);
  r.analysis.samples_per_frame = static_cast<decltype(r.analysis.samples_per_frame)>(1);
  r.analysis.frame_length_samples = static_cast<decltype(r.analysis.frame_length_samples)>(1);
  r.analysis.frame_count = static_cast<decltype(r.analysis.frame_count)>(1);
  r.analysis.algorithm_version = static_cast<decltype(r.analysis.algorithm_version)>(1);
  r.analysis.fmin_hz = static_cast<decltype(r.analysis.fmin_hz)>(1);
  r.analysis.fmax_hz = static_cast<decltype(r.analysis.fmax_hz)>(1);
  r.analysis.yin_threshold = static_cast<decltype(r.analysis.yin_threshold)>(1);
  r.analysis.voiced_threshold = static_cast<decltype(r.analysis.voiced_threshold)>(1);
  r.analysis.centered = static_cast<decltype(r.analysis.centered)>(1);
  r.analysis.segmentation_threshold_cents =
      static_cast<decltype(r.analysis.segmentation_threshold_cents)>(1);
  r.analysis.min_note_ms = static_cast<decltype(r.analysis.min_note_ms)>(1);
  r.analysis.reference_hz = static_cast<decltype(r.analysis.reference_hz)>(1);
  std::memset(&r.source_sha256, 1, sizeof(r.source_sha256));
  std::memset(&r.analysis_sha256, 1, sizeof(r.analysis_sha256));
  r.source_length_samples = static_cast<decltype(r.source_length_samples)>(1);
  r.sample_rate = static_cast<decltype(r.sample_rate)>(1);
  sonare_free_vocal_analysis_result(&r);
  CHECK(r.analysis.struct_size == static_cast<decltype(r.analysis.struct_size)>(0));
  CHECK(r.analysis.schema_version == static_cast<decltype(r.analysis.schema_version)>(0));
  CHECK(r.analysis.frame_origin_sample == static_cast<decltype(r.analysis.frame_origin_sample)>(0));
  CHECK(r.analysis.samples_per_frame == static_cast<decltype(r.analysis.samples_per_frame)>(0));
  CHECK(r.analysis.frame_length_samples ==
        static_cast<decltype(r.analysis.frame_length_samples)>(0));
  CHECK(r.analysis.f0_hz == nullptr);
  CHECK(r.analysis.voiced == nullptr);
  CHECK(r.analysis.frame_count == static_cast<decltype(r.analysis.frame_count)>(0));
  CHECK(r.analysis.algorithm_id == nullptr);
  CHECK(r.analysis.algorithm_version == static_cast<decltype(r.analysis.algorithm_version)>(0));
  CHECK(r.analysis.fmin_hz == static_cast<decltype(r.analysis.fmin_hz)>(0));
  CHECK(r.analysis.fmax_hz == static_cast<decltype(r.analysis.fmax_hz)>(0));
  CHECK(r.analysis.yin_threshold == static_cast<decltype(r.analysis.yin_threshold)>(0));
  CHECK(r.analysis.voiced_threshold == static_cast<decltype(r.analysis.voiced_threshold)>(0));
  CHECK(r.analysis.centered == static_cast<decltype(r.analysis.centered)>(0));
  CHECK(r.analysis.segmentation_threshold_cents ==
        static_cast<decltype(r.analysis.segmentation_threshold_cents)>(0));
  CHECK(r.analysis.min_note_ms == static_cast<decltype(r.analysis.min_note_ms)>(0));
  CHECK(r.analysis.reference_hz == static_cast<decltype(r.analysis.reference_hz)>(0));
  for (size_t i = 0; i < sizeof(r.source_sha256); ++i)
    CHECK(reinterpret_cast<const unsigned char*>(&r.source_sha256)[i] == 0);
  for (size_t i = 0; i < sizeof(r.analysis_sha256); ++i)
    CHECK(reinterpret_cast<const unsigned char*>(&r.analysis_sha256)[i] == 0);
  CHECK(r.source_length_samples == static_cast<decltype(r.source_length_samples)>(0));
  CHECK(r.sample_rate == static_cast<decltype(r.sample_rate)>(0));
}

TEST_CASE("sonare_free_vocal_edit_result leaves the result zeroed", "[c_api][c_free]") {
  SonareVocalEditResult r{};
  r.token.session_epoch = static_cast<decltype(r.token.session_epoch)>(1);
  r.token.revision = static_cast<decltype(r.token.revision)>(1);
  r.token.draft_id = static_cast<decltype(r.token.draft_id)>(1);
  r.token.generation = static_cast<decltype(r.token.generation)>(1);
  r.token.request_id = static_cast<decltype(r.token.request_id)>(1);
  r.token.profile_id = static_cast<decltype(r.token.profile_id)>(1);
  r.dirty_range_count = static_cast<decltype(r.dirty_range_count)>(1);
  r.id_change_count = static_cast<decltype(r.id_change_count)>(1);
  sonare_free_vocal_edit_result(&r);
  CHECK(r.token.session_epoch == static_cast<decltype(r.token.session_epoch)>(0));
  CHECK(r.token.revision == static_cast<decltype(r.token.revision)>(0));
  CHECK(r.token.draft_id == static_cast<decltype(r.token.draft_id)>(0));
  CHECK(r.token.generation == static_cast<decltype(r.token.generation)>(0));
  CHECK(r.token.request_id == static_cast<decltype(r.token.request_id)>(0));
  CHECK(r.token.profile_id == static_cast<decltype(r.token.profile_id)>(0));
  CHECK(r.dirty_ranges == nullptr);
  CHECK(r.dirty_range_count == static_cast<decltype(r.dirty_range_count)>(0));
  CHECK(r.id_changes == nullptr);
  CHECK(r.id_change_count == static_cast<decltype(r.id_change_count)>(0));
}

TEST_CASE("sonare_free_vocal_notes_result leaves the result zeroed", "[c_api][c_free]") {
  SonareVocalNotesResult r{};
  r.transition_count = static_cast<decltype(r.transition_count)>(1);
  sonare_free_vocal_notes_result(&r);
  CHECK(r.notes == nullptr);
  CHECK(r.note_count == static_cast<decltype(r.note_count)>(0));
  CHECK(r.transitions == nullptr);
  CHECK(r.transition_count == static_cast<decltype(r.transition_count)>(0));
}

TEST_CASE("sonare_free_vocal_pitch_result leaves the result zeroed", "[c_api][c_free]") {
  SonareVocalPitchResult r{};
  r.frame_count = static_cast<decltype(r.frame_count)>(1);
  sonare_free_vocal_pitch_result(&r);
  CHECK(r.source_samples == nullptr);
  CHECK(r.measured_midi == nullptr);
  CHECK(r.target_midi == nullptr);
  CHECK(r.effective_midi == nullptr);
  CHECK(r.voiced == nullptr);
  CHECK(r.has_target == nullptr);
  CHECK(r.frame_count == static_cast<decltype(r.frame_count)>(0));
}

TEST_CASE("sonare_free_vocal_render_result leaves the result zeroed", "[c_api][c_free]") {
  SonareVocalRenderResult r{};
  r.sample_count = static_cast<decltype(r.sample_count)>(1);
  r.start_sample = static_cast<decltype(r.start_sample)>(1);
  r.token.session_epoch = static_cast<decltype(r.token.session_epoch)>(1);
  r.token.revision = static_cast<decltype(r.token.revision)>(1);
  r.token.draft_id = static_cast<decltype(r.token.draft_id)>(1);
  r.token.generation = static_cast<decltype(r.token.generation)>(1);
  r.token.request_id = static_cast<decltype(r.token.request_id)>(1);
  r.token.profile_id = static_cast<decltype(r.token.profile_id)>(1);
  r.processed_range_count = static_cast<decltype(r.processed_range_count)>(1);
  r.cache_hit_units = static_cast<decltype(r.cache_hit_units)>(1);
  r.dry_passed_frames = static_cast<decltype(r.dry_passed_frames)>(1);
  r.limited_correction_frames = static_cast<decltype(r.limited_correction_frames)>(1);
  sonare_free_vocal_render_result(&r);
  CHECK(r.samples == nullptr);
  CHECK(r.sample_count == static_cast<decltype(r.sample_count)>(0));
  CHECK(r.start_sample == static_cast<decltype(r.start_sample)>(0));
  CHECK(r.token.session_epoch == static_cast<decltype(r.token.session_epoch)>(0));
  CHECK(r.token.revision == static_cast<decltype(r.token.revision)>(0));
  CHECK(r.token.draft_id == static_cast<decltype(r.token.draft_id)>(0));
  CHECK(r.token.generation == static_cast<decltype(r.token.generation)>(0));
  CHECK(r.token.request_id == static_cast<decltype(r.token.request_id)>(0));
  CHECK(r.token.profile_id == static_cast<decltype(r.token.profile_id)>(0));
  CHECK(r.processed_ranges == nullptr);
  CHECK(r.processed_range_count == static_cast<decltype(r.processed_range_count)>(0));
  CHECK(r.cache_hit_units == static_cast<decltype(r.cache_hit_units)>(0));
  CHECK(r.dry_passed_frames == static_cast<decltype(r.dry_passed_frames)>(0));
  CHECK(r.limited_correction_frames == static_cast<decltype(r.limited_correction_frames)>(0));
}

TEST_CASE("sonare_free_vocal_state_bytes leaves the result zeroed", "[c_api][c_free]") {
  SonareVocalStateBytes r{};
  r.size = static_cast<decltype(r.size)>(1);
  sonare_free_vocal_state_bytes(&r);
  CHECK(r.data == nullptr);
  CHECK(r.size == static_cast<decltype(r.size)>(0));
}

TEST_CASE("sonare_free_waveform_peak_pyramid_result leaves the result zeroed", "[c_api][c_free]") {
  SonareWaveformPeakPyramidResult r{};
  r.level_count = static_cast<decltype(r.level_count)>(1);
  sonare_free_waveform_peak_pyramid_result(&r);
  CHECK(r.levels == nullptr);
  CHECK(r.level_count == static_cast<decltype(r.level_count)>(0));
}

TEST_CASE("sonare_free_waveform_peaks_result leaves the result zeroed", "[c_api][c_free]") {
  SonareWaveformPeaksResult r{};
  r.channels = static_cast<decltype(r.channels)>(1);
  r.bucket_count = static_cast<decltype(r.bucket_count)>(1);
  r.samples_per_bucket = static_cast<decltype(r.samples_per_bucket)>(1);
  sonare_free_waveform_peaks_result(&r);
  CHECK(r.min == nullptr);
  CHECK(r.max == nullptr);
  CHECK(r.channels == static_cast<decltype(r.channels)>(0));
  CHECK(r.bucket_count == static_cast<decltype(r.bucket_count)>(0));
  CHECK(r.samples_per_bucket == static_cast<decltype(r.samples_per_bucket)>(0));
}
