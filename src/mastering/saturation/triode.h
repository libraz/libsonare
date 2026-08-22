#pragma once

/// @file triode.h
/// @brief Shared 12AX7 triode current law.
///
/// Dempwolf & Zoelzer, "A Physically-motivated Triode Model for Circuit
/// Simulations", DAFx-11, equations (10)-(12), with the Table 1 first fitted
/// 12AX7 system. Both the single-stage `saturation.tube` processor and the
/// cascaded preamp inside `saturation.ampSim` evaluate this same law, so it
/// lives here rather than being duplicated per processor.
///
/// The law is evaluated as a static transfer curve at a fixed plate voltage
/// rather than by solving the load line implicitly. That is a deliberate
/// realtime reduction: an implicit solve has no bounded iteration count and so
/// cannot be promised allocation-free on the audio thread.

#include <cmath>

namespace sonare::mastering::saturation::triode {

/// Fitted 12AX7 parameters (DAFx-11 Table 1, first system). Voltages are Vg/Va
/// relative to the cathode; currents are in the same milliampere scale as the
/// paper's figures.
struct Dempwolf12Ax7 {
  static constexpr float G = 2.242e-3f;
  static constexpr float mu = 103.2f;
  static constexpr float gamma = 1.26f;
  static constexpr float C = 3.40f;
  static constexpr float Gg = 6.177e-4f;
  static constexpr float xi = 1.314f;
  static constexpr float Cg = 9.901f;
  static constexpr float Ig0 = 8.025e-8f;
};

/// Fixed plate (anode) supply voltage in volts, the operating point the current
/// law is evaluated at.
inline constexpr float kPlateVoltageV = 250.0f;

/// The paper's smoothing function log(1 + exp(c*x))/c (equations (8)-(9)),
/// with the two asymptotes taken directly to keep exp() in range.
inline float smooth_positive(float c, float x) {
  const float z = c * x;
  if (z > 30.0f) return x;
  if (z < -30.0f) return std::exp(z) / c;
  return std::log1p(std::exp(z)) / c;
}

/// Cathode current, equation (10).
inline float cathode_current_ma(float vg, float va) {
  const float effective = va / Dempwolf12Ax7::mu + vg;
  return Dempwolf12Ax7::G *
         std::pow(smooth_positive(Dempwolf12Ax7::C, effective), Dempwolf12Ax7::gamma);
}

/// Grid current, equation (11). Non-negligible only once the grid swings
/// positive, which is what clips one half of the waveform harder than the other
/// and, through the coupling cap, causes blocking distortion in a cascade.
inline float grid_current_ma(float vg) {
  return Dempwolf12Ax7::Gg * std::pow(smooth_positive(Dempwolf12Ax7::Cg, vg), Dempwolf12Ax7::xi) +
         Dempwolf12Ax7::Ig0;
}

/// Plate (anode) current, equation (12).
inline float plate_current_ma(float vg, float va) {
  return cathode_current_ma(vg, va) - grid_current_ma(vg);
}

}  // namespace sonare::mastering::saturation::triode
