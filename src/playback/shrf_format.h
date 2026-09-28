#pragma once

/// @file shrf_format.h
/// @brief SHRF v1: the HRTF container of the playback renderer.
///
/// Little-endian. Header (36 bytes): magic "SHRF", version u16 = 1, quant u8
/// (0 float32, 1 int16), reserved u8, sample_rate u32, taps u16, n_az u16,
/// n_el u16, reserved u16, az_step f32, el_min f32, el_step f32 (degrees),
/// scale f32 (int16 sample = value * scale; 1.0 for float32). Then the ITD
/// table, n_el * n_az f32 in samples (positive: the left ear lags), then the
/// minimum-phase HRIRs, n_el * n_az * 2 (L, R) * taps. Rows ascend in
/// elevation; each row starts at azimuth 0 (front) and ascends by az_step
/// (positive right). Elevation is positive upward.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonare::playback {

inline constexpr uint16_t kShrfVersion = 1;
inline constexpr size_t kShrfHeaderBytes = 36;
inline constexpr int kShrfMinTaps = 16;
inline constexpr int kShrfMaxTaps = 1024;
inline constexpr int kShrfMinAzimuths = 4;
inline constexpr uint32_t kShrfMinSampleRate = 8000;
inline constexpr uint32_t kShrfMaxSampleRate = 384000;
/// Tolerance of the `n_az * az_step == 360` check, in degrees.
inline constexpr float kShrfAzimuthSpanTolerance = 1e-3f;

enum class ShrfQuant : uint8_t { Float32 = 0, Int16 = 1 };

/// Decoded header fields.
struct ShrfHeader {
  uint16_t version = kShrfVersion;
  ShrfQuant quant = ShrfQuant::Float32;
  uint32_t sample_rate = 0;
  int taps = 0;
  int n_az = 0;
  int n_el = 0;
  float az_step_deg = 0.0f;
  float el_min_deg = 0.0f;
  float el_step_deg = 0.0f;
  float scale = 1.0f;
};

/// A parsed set with the HRIRs dequantized to float.
struct ShrfData {
  ShrfHeader header;
  std::vector<float> itd;   ///< [el][az], samples at header.sample_rate
  std::vector<float> hrir;  ///< [el][az][ear][tap], ear 0 = left
};

/// Parses and validates SHRF v1 bytes (magic, version, azimuth span, counts,
/// tap and rate ranges, finite values, positive scale, exact size).
/// @throws SonareException(InvalidParameter) naming the violation.
ShrfData parse_shrf(const uint8_t* data, size_t size);

}  // namespace sonare::playback
