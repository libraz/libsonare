#pragma once

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include "analysis/acoustic_analyzer.h"
#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/melody_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/section_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "core/audio.h"
#include "core/convert.h"
#include "core/spectrum.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "feature/chroma.h"
#include "feature/cqt.h"
#include "feature/mel_spectrogram.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "feature/vqt.h"
#include "quick.h"
#include "sonare.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"

using namespace sonare;
using namespace sonare_c_detail;

float* copy_float_vector_or_nan(const std::vector<float>& values, size_t count);
void fill_acoustic_result(const AcousticParameters& params, SonareAcousticResult* out);
PitchClass from_c_pitch_class(SonarePitchClass pitch);
Mode from_c_mode(SonareMode mode);
bool fill_key_profile(SonareKeyProfileType profile_type, KeyConfig* config);
bool fill_key_modes(const SonareMode* modes, size_t mode_count, KeyConfig* config);
void fill_chord_result(const std::vector<Chord>& chords, SonareChordAnalysisResult* out);
SonareError fill_cqt_result(const CqtResult& result, SonareCqtResult* out);
