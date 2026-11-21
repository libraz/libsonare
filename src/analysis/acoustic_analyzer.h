#pragma once

/// @file acoustic_analyzer.h
/// @brief Room acoustic parameter analysis from impulse responses.

#include <vector>

#include "core/audio.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

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

/// @brief Domains of the AcousticConfig members, stated once for the core check and the readers.
inline constexpr util::check::NumericDomain kAcousticBandCountDomain =
    util::check::integer_domain_of<int>(0);
inline constexpr util::check::NumericDomain kAcousticMinDecayDbDomain =
    util::check::real_domain_above(0.0, false);
inline constexpr util::check::NumericDomain kAcousticNoiseFloorMarginDbDomain =
    util::check::real_domain_above(0.0, true);

/// @brief Refuses an AcousticConfig outside the argument domain every public surface shares.
/// @throws SonareException(InvalidParameter) for a negative band or subband count, a
///         non-finite or non-positive min_decay_db, or a negative or non-finite
///         noise_floor_margin_db.
inline void validate_acoustic_config(const AcousticConfig& config) {
  util::check::within(config.n_octave_bands, kAcousticBandCountDomain, "n_octave_bands");
  util::check::within(config.n_third_octave_subbands, kAcousticBandCountDomain,
                      "n_third_octave_subbands");
  util::check::within(config.min_decay_db, kAcousticMinDecayDbDomain, "min_decay_db");
  util::check::within(config.noise_floor_margin_db, kAcousticNoiseFloorMarginDbDomain,
                      "noise_floor_margin_db");
}

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

/// @brief Centre frequency of octave band @p band in the layout every acoustic
///        result uses: band 0 is 125 Hz and each one after it is an octave up.
float octave_band_center_hz(int band) noexcept;

/// @brief The single reverberation time a room is quoted by: the average of the
///        500 Hz and 1 kHz octaves (ISO 3382's mid-frequency T).
/// @details Bands that did not converge are NaN rather than absent, so those are
///          skipped. When neither mid band is usable the average falls back to
///          whatever bands are, which keeps a narrow-band estimate usable
///          instead of discarding it; with nothing finite at all it returns 0.
float mid_frequency_rt60(const std::vector<float>& rt60_bands) noexcept;

}  // namespace sonare
