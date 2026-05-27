#pragma once

/// @file acoustic_analyzer.h
/// @brief Room acoustic parameter analysis from impulse responses.

#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Configuration for acoustic parameter analysis.
struct AcousticConfig {
  enum class Mode {
    Auto,
    Blind,
    ImpulseResponse,
  };

  Mode mode = Mode::Auto;
  int n_octave_bands = 6;
  int n_third_octave_subbands = 24;
  float min_decay_db = 30.0f;
  float noise_floor_margin_db = 10.0f;
};

/// @brief Acoustic room parameters.
struct AcousticParameters {
  float rt60 = 0.0f;
  float edt = 0.0f;
  float c50 = 0.0f;
  float c80 = 0.0f;
  float d50 = 0.0f;
  std::vector<float> rt60_bands;
  std::vector<float> edt_bands;
  std::vector<float> c50_bands;
  std::vector<float> c80_bands;
  float confidence = 0.0f;
  bool is_blind = false;
};

/// @brief Acoustic analyzer for RT60, EDT, and clarity metrics.
class AcousticAnalyzer {
 public:
  /// @brief Constructs an acoustic analyzer.
  /// @details Auto mode routes impulse-like inputs to IR analysis; otherwise blind mode estimates
  /// RT60 from detected free-decay energy segments.
  explicit AcousticAnalyzer(const Audio& audio, const AcousticConfig& config = AcousticConfig());

  /// @brief Constructs an analyzer from an impulse response.
  static AcousticAnalyzer from_impulse_response(const Audio& ir,
                                                const AcousticConfig& config = AcousticConfig());

  /// @brief Returns acoustic analysis results.
  const AcousticParameters& parameters() const { return parameters_; }

 private:
  AcousticAnalyzer(const Audio& audio, const AcousticConfig& config, bool impulse_response);

  void analyze_impulse_response(const Audio& ir);
  void analyze_blind(const Audio& audio);
  void set_unsupported_blind_result();

  AcousticConfig config_;
  AcousticParameters parameters_;
};

/// @brief Detects acoustic parameters from audio.
AcousticParameters detect_acoustic(const Audio& audio,
                                   const AcousticConfig& config = AcousticConfig());

/// @brief Computes acoustic parameters from an impulse response.
AcousticParameters analyze_impulse_response(const Audio& ir,
                                            const AcousticConfig& config = AcousticConfig());

}  // namespace sonare
