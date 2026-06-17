#pragma once

#include <sonare/sonare_c.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/convert.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "feature/nnls_chroma.h"
#include "feature/onset.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "sonare_c_internal.h"
#include "streaming/stream_analyzer.h"
#include "streaming/stream_config.h"
#include "streaming/stream_frame.h"

using namespace sonare;
using namespace sonare_c_detail;

NnlsChromaConfig make_fast_nnls_chroma_config(int hop_length = 512);
SonareError fill_inverse_result(const std::vector<float>& data, int rows, int n_frames,
                                SonareInverseResult* out);
SonareError fill_audio_samples(const Audio& audio, float** out, size_t* out_length);
SonareError fill_pitch_result(const PitchResult& result, SonarePitchResult* out);
