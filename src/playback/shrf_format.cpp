#include "playback/shrf_format.h"

#include <cmath>
#include <cstring>

#include "util/exception.h"

namespace sonare::playback {

namespace {

uint8_t read_u8(const uint8_t* data, size_t offset) { return data[offset]; }

uint16_t read_u16(const uint8_t* data, size_t offset) {
  return static_cast<uint16_t>(static_cast<uint16_t>(data[offset]) |
                               static_cast<uint16_t>(data[offset + 1] << 8));
}

uint32_t read_u32(const uint8_t* data, size_t offset) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[offset + i]) << (8 * i);
  return value;
}

float read_f32(const uint8_t* data, size_t offset) {
  const uint32_t bits = read_u32(data, offset);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

int16_t read_i16(const uint8_t* data, size_t offset) {
  const uint16_t bits = read_u16(data, offset);
  int16_t value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void check_finite(float value, const char* name) {
  SONARE_CHECK_MSG(std::isfinite(value), ErrorCode::InvalidParameter,
                   std::string("SHRF ") + name + " is not finite: " + util::to_text(value));
}

}  // namespace

ShrfData parse_shrf(const uint8_t* data, size_t size) {
  SONARE_CHECK_MSG(data != nullptr || size == 0, ErrorCode::InvalidParameter, "SHRF data is null");
  SONARE_CHECK_MSG(size >= kShrfHeaderBytes, ErrorCode::InvalidParameter,
                   "SHRF data is smaller than the " + util::to_text(kShrfHeaderBytes) +
                       "-byte header: " + util::to_text(size) + " bytes");
  SONARE_CHECK_MSG(std::memcmp(data, "SHRF", 4) == 0, ErrorCode::InvalidParameter,
                   "SHRF magic mismatch");

  const uint16_t version = read_u16(data, 4);
  SONARE_CHECK_MSG(version == kShrfVersion, ErrorCode::InvalidParameter,
                   "SHRF version " + util::to_text(version) + " is not supported (expected " +
                       util::to_text(kShrfVersion) + ")");

  const uint8_t quant_byte = read_u8(data, 6);
  SONARE_CHECK_MSG(
      quant_byte == static_cast<uint8_t>(ShrfQuant::Float32) ||
          quant_byte == static_cast<uint8_t>(ShrfQuant::Int16),
      ErrorCode::InvalidParameter,
      "SHRF quant " + util::to_text(quant_byte) + " is neither float32 (0) nor int16 (1)");
  const ShrfQuant quant = static_cast<ShrfQuant>(quant_byte);

  ShrfHeader header;
  header.version = version;
  header.quant = quant;
  header.sample_rate = read_u32(data, 8);
  header.taps = static_cast<int>(read_u16(data, 12));
  header.n_az = static_cast<int>(read_u16(data, 14));
  header.n_el = static_cast<int>(read_u16(data, 16));
  header.az_step_deg = read_f32(data, 20);
  header.el_min_deg = read_f32(data, 24);
  header.el_step_deg = read_f32(data, 28);
  header.scale = read_f32(data, 32);

  check_finite(header.az_step_deg, "az_step");
  check_finite(header.el_min_deg, "el_min");
  check_finite(header.el_step_deg, "el_step");
  check_finite(header.scale, "scale");

  SONARE_CHECK_MSG(header.n_az >= kShrfMinAzimuths, ErrorCode::InvalidParameter,
                   "SHRF n_az " + util::to_text(header.n_az) + " is below the minimum " +
                       util::to_text(kShrfMinAzimuths));
  SONARE_CHECK_MSG(header.n_el >= 1, ErrorCode::InvalidParameter,
                   "SHRF n_el must be at least 1, got " + util::to_text(header.n_el));
  SONARE_CHECK_RANGE("SHRF taps", header.taps, kShrfMinTaps, kShrfMaxTaps);
  SONARE_CHECK_RANGE("SHRF sample_rate", header.sample_rate, kShrfMinSampleRate,
                     kShrfMaxSampleRate);

  const double azimuth_span =
      static_cast<double>(header.n_az) * static_cast<double>(header.az_step_deg);
  SONARE_CHECK_MSG(std::abs(azimuth_span - 360.0) <= static_cast<double>(kShrfAzimuthSpanTolerance),
                   ErrorCode::InvalidParameter,
                   "SHRF n_az * az_step must span 360 degrees, got " + util::to_text(azimuth_span));

  SONARE_CHECK_MSG(header.scale > 0.0f, ErrorCode::InvalidParameter,
                   "SHRF scale must be positive, got " + util::to_text(header.scale));

  // The ITD table is always f32 regardless of `quant`, which governs the HRIR data only.
  const uint64_t direction_count =
      static_cast<uint64_t>(header.n_el) * static_cast<uint64_t>(header.n_az);
  const uint64_t itd_bytes = direction_count * sizeof(float);
  const uint64_t bytes_per_sample = quant == ShrfQuant::Int16 ? sizeof(int16_t) : sizeof(float);
  const uint64_t hrir_bytes =
      direction_count * 2 * static_cast<uint64_t>(header.taps) * bytes_per_sample;
  const uint64_t expected_size = kShrfHeaderBytes + itd_bytes + hrir_bytes;
  SONARE_CHECK_MSG(static_cast<uint64_t>(size) == expected_size, ErrorCode::InvalidParameter,
                   "SHRF size mismatch: expected " + util::to_text(expected_size) + " bytes, got " +
                       util::to_text(size));

  ShrfData result;
  result.header = header;

  result.itd.resize(static_cast<size_t>(direction_count));
  size_t offset = kShrfHeaderBytes;
  for (size_t i = 0; i < result.itd.size(); ++i) {
    const float value = read_f32(data, offset);
    check_finite(value, "ITD value");
    result.itd[i] = value;
    offset += sizeof(float);
  }

  result.hrir.resize(static_cast<size_t>(direction_count * 2 * static_cast<uint64_t>(header.taps)));
  for (size_t i = 0; i < result.hrir.size(); ++i) {
    if (quant == ShrfQuant::Int16) {
      result.hrir[i] = static_cast<float>(read_i16(data, offset)) * header.scale;
      offset += sizeof(int16_t);
    } else {
      const float value = read_f32(data, offset);
      check_finite(value, "HRIR sample");
      result.hrir[i] = value;
      offset += sizeof(float);
    }
  }

  return result;
}

}  // namespace sonare::playback
