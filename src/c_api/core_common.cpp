#include "c_api/core_internal.h"

float* copy_float_vector_or_nan(const std::vector<float>& values, size_t count) {
  if (count == 0) {
    return nullptr;
  }
  float* out = new float[count];
  for (size_t i = 0; i < count; ++i) {
    out[i] = i < values.size() ? values[i] : std::numeric_limits<float>::quiet_NaN();
  }
  return out;
}

void fill_acoustic_result(const AcousticParameters& params, SonareAcousticResult* out) {
  out->rt60 = params.rt60;
  out->edt = params.edt;
  out->c50 = params.c50;
  out->c80 = params.c80;
  out->d50 = params.d50;
  out->band_count = params.rt60_bands.size();
  out->rt60_bands = copy_float_vector_or_nan(params.rt60_bands, out->band_count);
  out->edt_bands = copy_float_vector_or_nan(params.edt_bands, out->band_count);
  // Clarity bands are not computed in blind mode; expose null (rather than a
  // NaN-filled array) so callers can distinguish "not computed" from "invalid".
  out->c50_bands = params.c50_bands.empty()
                       ? nullptr
                       : copy_float_vector_or_nan(params.c50_bands, out->band_count);
  out->c80_bands = params.c80_bands.empty()
                       ? nullptr
                       : copy_float_vector_or_nan(params.c80_bands, out->band_count);
  out->confidence = params.confidence;
  out->is_blind = params.is_blind ? 1 : 0;
}

PitchClass from_c_pitch_class(SonarePitchClass pitch) {
  const int value = static_cast<int>(pitch);
  if (value < static_cast<int>(PitchClass::C) || value > static_cast<int>(PitchClass::B)) {
    return PitchClass::C;
  }
  return static_cast<PitchClass>(value);
}

Mode from_c_mode(SonareMode mode) {
  const int value = static_cast<int>(mode);
  if (value < static_cast<int>(Mode::Major) || value > static_cast<int>(Mode::Locrian)) {
    return Mode::Major;
  }
  return static_cast<Mode>(value);
}

bool fill_key_profile(SonareKeyProfileType profile_type, KeyConfig* config) {
  if (config == nullptr) {
    return false;
  }
  switch (profile_type) {
    case SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER:
      config->profile_type = KeyProfileType::KrumhanslSchmuckler;
      return true;
    case SONARE_KEY_PROFILE_TEMPERLEY:
      config->profile_type = KeyProfileType::Temperley;
      return true;
    case SONARE_KEY_PROFILE_SHAATH:
      config->profile_type = KeyProfileType::Shaath;
      return true;
    case SONARE_KEY_PROFILE_FARALDO_EDMT:
      config->profile_type = KeyProfileType::FaraldoEDMT;
      return true;
    case SONARE_KEY_PROFILE_FARALDO_EDMA:
      config->profile_type = KeyProfileType::FaraldoEDMA;
      return true;
    case SONARE_KEY_PROFILE_FARALDO_EDMM:
      config->profile_type = KeyProfileType::FaraldoEDMM;
      return true;
    case SONARE_KEY_PROFILE_BELLMAN_BUDGE:
      config->profile_type = KeyProfileType::BellmanBudge;
      return true;
    default:
      return false;
  }
}

bool fill_key_modes(const SonareMode* modes, size_t mode_count, KeyConfig* config) {
  if (mode_count == 0) {
    return true;
  }
  if (modes == nullptr || config == nullptr) {
    return false;
  }
  config->modes.clear();
  config->modes.reserve(mode_count);
  for (size_t i = 0; i < mode_count; ++i) {
    const int value = static_cast<int>(modes[i]);
    if (value < static_cast<int>(Mode::Major) || value > static_cast<int>(Mode::Locrian)) {
      return false;
    }
    config->modes.push_back(static_cast<Mode>(value));
  }
  return true;
}

void fill_chord_result(const std::vector<Chord>& chords, SonareChordAnalysisResult* out) {
  out->chord_count = chords.size();
  if (chords.empty()) {
    return;
  }

  std::unique_ptr<SonareChord[]> data(new SonareChord[chords.size()]);
  for (size_t i = 0; i < chords.size(); ++i) {
    data[i].root = static_cast<SonarePitchClass>(chords[i].root);
    data[i].quality = to_c_chord_quality(chords[i].quality);
    data[i].start = chords[i].start;
    data[i].end = chords[i].end;
    data[i].confidence = chords[i].confidence;
    data[i].bass = static_cast<SonarePitchClass>(chords[i].bass);
  }
  out->chords = release_array(data);
}

SonareError fill_cqt_result(const CqtResult& result, SonareCqtResult* out) {
  *out = {};
  out->n_bins = result.n_bins();
  out->n_frames = result.n_frames();
  out->hop_length = result.hop_length();
  out->sample_rate = result.sample_rate();
  const std::vector<float>& magnitude = result.magnitude();
  if (!magnitude.empty()) {
    auto mag = std::make_unique<float[]>(magnitude.size());
    std::copy(magnitude.begin(), magnitude.end(), mag.get());
    out->magnitude = mag.release();
  }
  const std::vector<float>& freqs = result.frequencies();
  if (!freqs.empty()) {
    auto fr = std::make_unique<float[]>(freqs.size());
    std::copy(freqs.begin(), freqs.end(), fr.get());
    out->frequencies = fr.release();
  }
  return SONARE_OK;
}
