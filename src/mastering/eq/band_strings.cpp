#include "mastering/eq/band_strings.h"

#include <cstddef>

namespace sonare::mastering::eq {
namespace {

constexpr EqBandType kBandTypes[] = {
    EqBandType::Peak,     EqBandType::LowShelf, EqBandType::HighShelf, EqBandType::LowPass,
    EqBandType::HighPass, EqBandType::BandPass, EqBandType::Notch,     EqBandType::TiltShelf,
    EqBandType::FlatTilt, EqBandType::AllPass,
};
constexpr BiquadCoeffMode kCoeffModes[] = {BiquadCoeffMode::Rbj, BiquadCoeffMode::Vicanek};
constexpr StereoPlacement kPlacements[] = {StereoPlacement::Stereo, StereoPlacement::Left,
                                           StereoPlacement::Right, StereoPlacement::Mid,
                                           StereoPlacement::Side};
constexpr PhaseMode kPhaseModes[] = {PhaseMode::Inherit, PhaseMode::ZeroLatency,
                                     PhaseMode::NaturalPhase, PhaseMode::LinearPhase};

template <typename Enum, std::size_t N, typename ToString>
std::optional<Enum> parse_token(const Enum (&all)[N], ToString to_string, std::string_view value) {
  for (const Enum candidate : all) {
    if (value == to_string(candidate)) return candidate;
  }
  return std::nullopt;
}

}  // namespace

const char* band_type_to_string(EqBandType type) {
  switch (type) {
    case EqBandType::Peak:
      return "peak";
    case EqBandType::LowShelf:
      return "lowShelf";
    case EqBandType::HighShelf:
      return "highShelf";
    case EqBandType::LowPass:
      return "lowPass";
    case EqBandType::HighPass:
      return "highPass";
    case EqBandType::BandPass:
      return "bandPass";
    case EqBandType::Notch:
      return "notch";
    case EqBandType::TiltShelf:
      return "tiltShelf";
    case EqBandType::FlatTilt:
      return "flatTilt";
    case EqBandType::AllPass:
      return "allPass";
  }
  return "peak";
}

const char* coeff_mode_to_string(BiquadCoeffMode mode) {
  return mode == BiquadCoeffMode::Vicanek ? "vicanek" : "rbj";
}

const char* placement_to_string(StereoPlacement placement) {
  switch (placement) {
    case StereoPlacement::Stereo:
      return "stereo";
    case StereoPlacement::Left:
      return "left";
    case StereoPlacement::Right:
      return "right";
    case StereoPlacement::Mid:
      return "mid";
    case StereoPlacement::Side:
      return "side";
  }
  return "stereo";
}

const char* phase_mode_to_string(PhaseMode phase) {
  switch (phase) {
    case PhaseMode::Inherit:
      return "inherit";
    case PhaseMode::ZeroLatency:
      return "zeroLatency";
    case PhaseMode::NaturalPhase:
      return "naturalPhase";
    case PhaseMode::LinearPhase:
      return "linearPhase";
  }
  return "inherit";
}

const char* band_type_tokens() {
  return "peak, lowShelf, highShelf, lowPass, highPass, bandPass, notch, tiltShelf, flatTilt, "
         "allPass";
}

const char* coeff_mode_tokens() { return "rbj, vicanek"; }

const char* placement_tokens() { return "stereo, left, right, mid, side"; }

const char* phase_mode_tokens() { return "inherit, zeroLatency, naturalPhase, linearPhase"; }

std::optional<EqBandType> band_type_from_string(std::string_view value) {
  return parse_token(kBandTypes, band_type_to_string, value);
}

std::optional<BiquadCoeffMode> coeff_mode_from_string(std::string_view value) {
  return parse_token(kCoeffModes, coeff_mode_to_string, value);
}

std::optional<StereoPlacement> placement_from_string(std::string_view value) {
  return parse_token(kPlacements, placement_to_string, value);
}

std::optional<PhaseMode> phase_mode_from_string(std::string_view value) {
  return parse_token(kPhaseModes, phase_mode_to_string, value);
}

std::optional<PhaseMode> phase_mode_from_int(int mode) {
  switch (mode) {
    case 1:
      return PhaseMode::ZeroLatency;
    case 2:
      return PhaseMode::NaturalPhase;
    case 3:
      return PhaseMode::LinearPhase;
    default:
      return std::nullopt;
  }
}

std::optional<StereoPlacement> placement_from_int(int placement) {
  switch (placement) {
    case 0:
      return StereoPlacement::Stereo;
    case 1:
      return StereoPlacement::Left;
    case 2:
      return StereoPlacement::Right;
    case 3:
      return StereoPlacement::Mid;
    case 4:
      return StereoPlacement::Side;
    default:
      return std::nullopt;
  }
}

}  // namespace sonare::mastering::eq
