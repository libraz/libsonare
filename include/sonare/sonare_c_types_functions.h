#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

// Audio functions
/// @note Release @p out with @ref sonare_audio_destroy; it is a handle, not a
///       sonare_free_* buffer.
SonareError sonare_audio_from_buffer(const float* data, size_t length, int sample_rate,
                                     SonareAudio** out);
/// @note Release @p out with @ref sonare_audio_destroy; it is a handle, not a
///       sonare_free_* buffer.
SonareError sonare_audio_from_memory(const uint8_t* data, size_t length, SonareAudio** out);

/// @brief Decodes audio bytes once and returns every source channel, without a
///        downmix.
/// @details Same format set, size ceiling and decoded-buffer contract as
///          @ref sonare_audio_from_memory (an empty decode or a non-finite sample
///          is @c SONARE_ERROR_DECODE_FAILED, a declared rate outside the
///          supported range is @c SONARE_ERROR_INVALID_FORMAT); only the channel
///          fold differs. @p out_samples holds @p out_channels planes of
///          @p out_frames samples each, plane after plane in the source's own
///          channel order (5.1 = L R C LFE Ls Rs). A NULL @p data or a zero
///          @p length returns @c SONARE_ERROR_INVALID_PARAMETER. Every output
///          is written only on @c SONARE_OK; on failure @p out_samples is NULL
///          and the counts are 0.
/// @note Free @p out_samples with @ref sonare_free_floats.
SonareError sonare_decode_channels(const uint8_t* data, size_t length, float** out_samples,
                                   size_t* out_frames, int* out_channels, int* out_sample_rate);

/// @brief Fold options for @ref sonare_downmix.
/// @details Fill with @ref sonare_downmix_options_init before setting fields.
typedef struct {
  /// Size of this struct in bytes, written by @ref sonare_downmix_options_init.
  uint32_t struct_size;
  /// Non-zero folds the LFE plane into the front pair at -3 dB instead of
  /// dropping it. Default 0.
  int32_t include_lfe;
  /// Non-zero scales the output so the worst case, every input at full scale and
  /// in phase, stays at or below unity. Default 0.
  int32_t normalize;
} SonareDownmixOptions;

/// @brief Writes the defaults (both options off) and @c struct_size into
///        @p options; a NULL @p options is ignored.
void sonare_downmix_options_init(SonareDownmixOptions* options);

/// @brief Downmixes planar channels to a narrower layout (ITU-R BS.775).
/// @details @p channels holds @p num_channels planes of @p frames samples each. The
///          source layout is the one @p num_channels names (1 mono, 2 stereo, 6 5.1,
///          8 7.1; plane order as in @ref sonare_decode_channels). Center and
///          surround enter the front pair at -3 dB and the LFE plane is dropped
///          unless @p options asks to fold it. The BS.775 fold is not normalised, so
///          a source with several full-scale planes can fold past full scale (six
///          planes of 1.0 reach 2.414 on a stereo target) unless @p options asks to
///          normalise. A stereo/5.1/7.1 source folds to mono through the same matrix
///          the decoders use. @p target_layout is a @ref SonareChannelLayout value.
///          Supported targets narrow the bed: 7.1 to 5.1, 5.1 and 7.1 to stereo,
///          stereo/5.1/7.1 to mono, and the identity copy. A channel count outside
///          1/2/6/8 folds to @c SONARE_CHANNEL_LAYOUT_MONO as the unweighted mean of
///          its planes whatever the options, which is what the decoders do for such
///          a source, and has no other target. A NULL @p options takes the defaults.
///          Anything else (an upmix, an unknown @p target_layout, a zero @p frames or
///          @p num_channels, a NULL pointer, an @p options whose @c struct_size is
///          smaller than this struct) returns @c SONARE_ERROR_INVALID_PARAMETER.
///          @p out_samples receives the target layout's channel count of planes,
///          @p frames samples each.
/// @note Free @p out_samples with @ref sonare_free_floats.
SonareError sonare_downmix(const float* channels, size_t frames, size_t num_channels,
                           int target_layout, const SonareDownmixOptions* options,
                           float** out_samples);

#ifndef __EMSCRIPTEN__
/// @note Release @p out with @ref sonare_audio_destroy; it is a handle, not a
///       sonare_free_* buffer.
SonareError sonare_audio_from_file(const char* path, SonareAudio** out);
/// @brief Loads one channel of an audio file, leaving the others out of it.
/// @details A handle carries one plane, so @ref sonare_audio_from_file downmixes
///          a multi-channel source into it. This entry takes the plane the caller
///          asks for instead, which is what a stereo render needs: pair it with
///          @ref sonare_audio_file_num_channels and load each channel in turn.
///          The file is decoded once per call, so a stereo load costs two
///          decodes; the alternative, handing back an interleaved buffer, would
///          add an ownership contract for one allocation.
/// @details Same format set and same decoded-buffer contract as
///          @ref sonare_audio_from_file: an empty decode or a non-finite sample
///          in the requested channel is @c SONARE_ERROR_DECODE_FAILED, a declared
///          rate outside the supported range is @c SONARE_ERROR_INVALID_FORMAT.
///          A negative @p channel_index, or one the file has no channel for,
///          returns @c SONARE_ERROR_INVALID_PARAMETER.
/// @note Release @p out with @ref sonare_audio_destroy; it is a handle, not a
///       sonare_free_* buffer.
SonareError sonare_audio_from_file_channel(const char* path, int channel_index, SonareAudio** out);
/// @brief Returns the positive source channel count reported by an audio file.
/// @details @p out_channels is set to 0 immediately when it is non-NULL, before
///          argument validation or file inspection. On @c SONARE_OK it receives
///          a positive channel count and is left at 0 on every failure. Both
///          @p path and @p out_channels must be non-NULL; either NULL argument
///          returns @c SONARE_ERROR_INVALID_PARAMETER. A missing/unreadable file
///          returns @c SONARE_ERROR_FILE_NOT_FOUND. A malformed recognized file
///          or decoder/probe failure returns @c SONARE_ERROR_DECODE_FAILED.
///          An unknown or unsupported format returns @c SONARE_ERROR_INVALID_FORMAT
///          when no FFmpeg decoder is enabled; with FFmpeg enabled, failures
///          while probing that format return @c SONARE_ERROR_DECODE_FAILED.
SonareError sonare_audio_file_num_channels(const char* path, int* out_channels);
#endif

/// @brief Releases @p audio; a NULL @p audio is ignored.
void sonare_audio_destroy(SonareAudio* audio);
/// @brief Borrowed view of the handle's mono samples, or NULL for a NULL
///        @p audio.
/// @details Owned by the @ref SonareAudio handle, not the caller: valid until
///          @ref sonare_audio_destroy, and never to be freed separately. Use
///          @ref sonare_audio_length for the element count, which is a sample
///          count and not frames times channels: a handle carries one plane, and
///          a multi-channel source is downmixed as the handle is built.
///          @ref sonare_audio_file_num_channels reports the channel count of a
///          file on disk, never the handle's.
const float* sonare_audio_data(const SonareAudio* audio);
/// @brief Sample count of @p audio's one plane, or 0 for a NULL @p audio.
size_t sonare_audio_length(const SonareAudio* audio);
/// @brief Sample rate of @p audio in Hz, or 0 for a NULL @p audio.
int sonare_audio_sample_rate(const SonareAudio* audio);
/// @brief Duration of @p audio in seconds, or 0 for a NULL @p audio.
float sonare_audio_duration_sec(const SonareAudio* audio);
/// @brief Detects the tempo of @p audio within a search range.
/// @details @p bpm_min and @p bpm_max follow @ref sonare_detect_bpm.
SonareError sonare_audio_detect_bpm(const SonareAudio* audio, float bpm_min, float bpm_max,
                                    float* out_bpm);
/// @brief Detects the key of @p audio, measuring the recording's tuning first.
SonareError sonare_audio_detect_key(const SonareAudio* audio, SonareKey* out_key);
/// @details @p bpm_min and @p bpm_max follow @ref sonare_detect_bpm.
/// @note Free @p out_times with @ref sonare_free_floats.
SonareError sonare_audio_detect_beats(const SonareAudio* audio, float bpm_min, float bpm_max,
                                      float** out_times, size_t* out_count);
/// @note Free @p out_times with @ref sonare_free_floats.
SonareError sonare_audio_detect_downbeats(const SonareAudio* audio, float** out_times,
                                          size_t* out_count);
/// @note Free @p out_times with @ref sonare_free_floats.
SonareError sonare_audio_detect_onsets(const SonareAudio* audio, float** out_times,
                                       size_t* out_count);
/// @brief Runs the full quick analysis pipeline and fills the flat C result.
/// @details Use the single-purpose sonare_audio_detect_* helpers for cheaper queries.
/// @param out Receives heap-owned arrays; free with sonare_free_analysis_result.
SonareError sonare_audio_analyze(const SonareAudio* audio, SonareAnalysisResult* out);

// Quick detection functions
/// @brief Detects the tempo within a search range.
/// @param bpm_min Lower bound of the tempo search in BPM; finite and positive.
/// @param bpm_max Upper bound in BPM; finite and above @p bpm_min. A range that is
///        not (non-positive, non-finite or inverted) is INVALID_PARAMETER, as for
///        @ref sonare_analyze_bpm. Pass 60 and 200 for the search the other
///        public tempo entries default to.
SonareError sonare_detect_bpm(const float* samples, size_t length, int sample_rate, float bpm_min,
                              float bpm_max, float* out_bpm);
/// @brief Detects the key, measuring the recording's tuning first. The key entry points that take
///   no tuning all measure it; @ref sonare_detect_key_with_tuning and
///   @ref sonare_detect_key_candidates_with_tuning take a given one (0 is concert A440).
SonareError sonare_detect_key(const float* samples, size_t length, int sample_rate,
                              SonareKey* out_key);
/// @brief Detects the key with explicit chroma settings, measuring the tuning first.
/// @param n_fft FFT size in samples at 22050 Hz, rescaled to the analysis rate.
/// @param hop_length Hop in samples of the analysed signal: the input resampled to 22050 Hz
///   when its rate is above 22050 Hz, the input itself otherwise.
/// @param use_hpss Non-zero applies harmonic-percussive separation before the chroma.
/// @param loudness_weighted Non-zero weights chroma frames by loudness.
/// @param chroma_high_pass_hz High-pass cutoff for the chroma in Hz; 0 disables it.
SonareError sonare_detect_key_with_options(const float* samples, size_t length, int sample_rate,
                                           int n_fft, int hop_length, int use_hpss,
                                           int loudness_weighted, float chroma_high_pass_hz,
                                           SonareKey* out_key);
/// @brief Detects the key with explicit chroma settings and a restricted mode set.
/// @details Chroma parameters follow @ref sonare_detect_key_with_options.
/// @param modes Modes the key may take, @p mode_count entries.
SonareError sonare_detect_key_with_options_and_modes(const float* samples, size_t length,
                                                     int sample_rate, int n_fft, int hop_length,
                                                     int use_hpss, int loudness_weighted,
                                                     float chroma_high_pass_hz,
                                                     const SonareMode* modes, size_t mode_count,
                                                     SonareKey* out_key);
/// @brief Detects the key with a mode set, a key-profile family and an optional genre hint.
/// @details Chroma parameters and @p modes follow
///   @ref sonare_detect_key_with_options_and_modes.
/// @param profile_type Key-profile family the chroma is correlated against.
/// @param genre_hint Optional genre name biasing the profile choice; may be NULL.
SonareError sonare_detect_key_with_extended_options(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float chroma_high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyProfileType profile_type, const char* genre_hint, SonareKey* out_key);
/// @brief Extended key detection with a recording tuning.
/// @details Same as @ref sonare_detect_key_with_extended_options, with the chroma shifted by
///   @p tuning_cents, in [-50, 50) where 0 is concert A440. A non-zero
///   @p tuning_auto measures the tuning from the audio instead of reading @p tuning_cents.
/// @param out_tuning_cents Optional; receives the tuning used, given or measured, in cents.
SonareError sonare_detect_key_with_tuning(const float* samples, size_t length, int sample_rate,
                                          int n_fft, int hop_length, int use_hpss,
                                          int loudness_weighted, float chroma_high_pass_hz,
                                          const SonareMode* modes, size_t mode_count,
                                          SonareKeyProfileType profile_type, const char* genre_hint,
                                          float tuning_cents, int tuning_auto, SonareKey* out_key,
                                          float* out_tuning_cents);
/// @note Free @p out_candidates with @ref sonare_free_key_candidates.
SonareError sonare_detect_key_candidates(const float* samples, size_t length, int sample_rate,
                                         int n_fft, int hop_length, int use_hpss,
                                         int loudness_weighted, float chroma_high_pass_hz,
                                         SonareKeyCandidate** out_candidates, size_t* out_count);
/// @note Free @p out_candidates with @ref sonare_free_key_candidates.
SonareError sonare_detect_key_candidates_with_modes(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float chroma_high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyCandidate** out_candidates, size_t* out_count);
/// @note Free @p out_candidates with @ref sonare_free_key_candidates.
SonareError sonare_detect_key_candidates_with_extended_options(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float chroma_high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyProfileType profile_type, const char* genre_hint, SonareKeyCandidate** out_candidates,
    size_t* out_count);
/// @brief Extended key candidates with a recording tuning.
/// @details Same as @ref sonare_detect_key_candidates_with_extended_options, with the tuning
///   taken as @ref sonare_detect_key_with_tuning takes it: @p tuning_cents, in [-50, 50) where 0 is
///   concert A440, or measured from the audio when @p tuning_auto is non-zero.
/// @param out_tuning_cents Optional; receives the tuning used, given or measured, in cents.
/// @note Free @p out_candidates with @ref sonare_free_key_candidates.
SonareError sonare_detect_key_candidates_with_tuning(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float chroma_high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyProfileType profile_type, const char* genre_hint, float tuning_cents, int tuning_auto,
    SonareKeyCandidate** out_candidates, size_t* out_count, float* out_tuning_cents);
/// @brief Detects beat times within a tempo search range.
/// @details @p bpm_min and @p bpm_max follow @ref sonare_detect_bpm.
/// @note Free @p out_times with @ref sonare_free_floats.
SonareError sonare_detect_beats(const float* samples, size_t length, int sample_rate, float bpm_min,
                                float bpm_max, float** out_times, size_t* out_count);
/// @note Free @p out_times with @ref sonare_free_floats.
SonareError sonare_detect_downbeats(const float* samples, size_t length, int sample_rate,
                                    float** out_times, size_t* out_count);
/// @note Free @p out_times with @ref sonare_free_floats.
SonareError sonare_detect_onsets(const float* samples, size_t length, int sample_rate,
                                 float** out_times, size_t* out_count);

/// @brief Controls peak-picking for @ref sonare_detect_onsets_ex.
/// @details Zero-initialize this POD, then set every field explicitly; the
///          legacy @ref sonare_detect_onsets function retains its defaults.
///
///          @p pre_max, @p post_max, @p pre_avg, @p post_avg, @p wait and
///          @p backtrack_range are FRAME counts, and a frame is @p hop_length
///          divided by the analysis rate. That rate is 22050 Hz on both entry
///          points -- input above it is resampled down before detection -- so a
///          field means the same span of audio whatever rate the caller's buffer
///          carries, and filling this struct with the values listed as defaults
///          reproduces @ref sonare_detect_onsets exactly.
typedef struct {
  /// FFT window length in samples at 22050 Hz, rescaled to the input rate. Default 2048.
  int32_t n_fft;
  /// Hop in samples at the analysis rate. Default 512.
  int32_t hop_length;
  /// Minimum onset strength a peak must reach; 0 selects the adaptive path.
  /// Expressed in the units of the onset envelope this call computes internally,
  /// which is not normalized, so the usable range depends on the magnitudes the
  /// caller's own audio produces and cannot be derived from the signature. The
  /// envelope is the same one sonare_onset_strength returns, so run that on
  /// representative audio to see the scale before setting this.
  float threshold;
  /// Frames before a peak that must not exceed it. Default 1.
  int32_t pre_max;
  /// Frames after a peak that must not exceed it. Default 1.
  int32_t post_max;
  /// Frames before a peak averaged for the adaptive threshold. Default 3.
  int32_t pre_avg;
  /// Frames after a peak averaged for the adaptive threshold. Default 4.
  int32_t post_avg;
  /// Offset added to the running mean of the onset envelope, in that envelope's
  /// own units -- the same quantity and the same caveat as sonare_peak_pick's
  /// delta, which this field ultimately feeds. Not a normalized or dB value.
  /// Take a small fraction of the envelope's mean or median and adjust. Default 0.06.
  float delta;
  /// Minimum frames between consecutive onsets. Default 1.
  int32_t wait;
  /// Non-zero moves each onset back to the preceding local minimum of the envelope; only 0 and 1
  /// are accepted. Default 0.
  uint8_t backtrack;
  /// Padding; set to 0.
  uint8_t reserved[3];
  /// Search range in frames for the backtracking. Default 10.
  int32_t backtrack_range;
} SonareOnsetDetectConfig;

#ifdef __cplusplus
static_assert(sizeof(SonareOnsetDetectConfig) == 44u, "SonareOnsetDetectConfig layout drift");
static_assert(offsetof(SonareOnsetDetectConfig, hop_length) == 4u,
              "SonareOnsetDetectConfig hop_length offset");
static_assert(offsetof(SonareOnsetDetectConfig, backtrack_range) == 40u,
              "SonareOnsetDetectConfig backtrack_range offset");
#endif

/// @brief Detect onsets with explicit FFT, peak-picking, and backtracking settings.
/// @note Free @p out_times with @ref sonare_free_floats.
SonareError sonare_detect_onsets_ex(const float* samples, size_t length, int sample_rate,
                                    const SonareOnsetDetectConfig* config, float** out_times,
                                    size_t* out_count);

/// @brief Runs the full quick analysis pipeline and fills the flat C result.
/// @details Use the single-purpose sonare_detect_* helpers for cheaper queries.
/// @param out Receives heap-owned arrays; free with sonare_free_analysis_result.
SonareError sonare_analyze(const float* samples, size_t length, int sample_rate,
                           SonareAnalysisResult* out);

/// Progress callback for the JSON analysis variants: progress in [0,1] plus a
/// stage label. user_data is the opaque pointer passed to the analyze call.
typedef void (*SonareAnalyzeProgressCallback)(float progress, const char* stage, void* user_data);

/// Cooperative cancellation callback for long-running offline operations.
/// Return nonzero to stop after the current progress-report boundary.
typedef int (*SonareCancelCallback)(void* user_data);

/// @brief Full analysis serialized to a camelCase JSON object.
/// @details Unlike sonare_analyze (which fills only the flat bpm/key/beats struct), this returns
///   the complete result: chords, sections, timbre, dynamics, rhythm, melody and form, with
///   per-beat strength.
/// @param out_json Receives a heap-allocated string; release it with @ref sonare_free_string.
SonareError sonare_analyze_json(const float* samples, size_t length, int sample_rate,
                                char** out_json);
/// Capacity of SonareMusicAnalyzeOptions::meter_candidate_numerators. Part of
/// the public contract: a caller cannot request more numerators than this, and
/// every binding rejects an over-long list rather than truncating it.
#define SONARE_MAX_METER_CANDIDATE_NUMERATORS 16

/// Layout version of SonareMusicAnalyzeOptions, written by
/// sonare_music_analyze_options_default(). sonare_analyze_json_ex refuses any
/// other value, so a caller built against the layout that had no version field
/// is refused rather than read at the wrong offsets.
#define SONARE_MUSIC_ANALYZE_OPTIONS_VERSION 2

/// @brief Options for sonare_analyze_json_ex. Boolean fields are 0/1 ints.
/// @details Always start from sonare_music_analyze_options_default() rather than zeroing the
///   struct: a zeroed meter_candidate_numerator_count is rejected, not treated as "use the
///   default set". Defaults below are those of that function.
typedef struct {
  /// Must be SONARE_MUSIC_ANALYZE_OPTIONS_VERSION.
  int struct_version;
  /// Analysis window in samples at 22050 Hz, rescaled to the analysis rate. Default 2048.
  int n_fft;
  /// Hop in samples of the analysed signal: the input resampled to 22050 Hz when its rate is above
  /// 22050 Hz, the input itself otherwise. Default 512.
  int hop_length;
  /// Lower bound of the tempo search in BPM. Default 60.
  float bpm_min;
  /// Upper bound of the tempo search in BPM. Default 200.
  float bpm_max;
  /// Prior tempo estimate in BPM. Default 120.
  float start_bpm;
  /// Non-zero restricts chord detection to triads, with no seventh chords. Default 1.
  int use_triads_only;
  /// Non-zero applies harmonic-percussive separation before the chord chroma. Default 0.
  int use_hpss;
  /// High-pass cutoff for the chroma in Hz; 0 disables it. Default 80.
  float chroma_high_pass_hz;
  /// Non-zero weights the chord chroma toward the bass.
  int use_bass_weighted;
  /// Multiplier on hop_length for the chroma; larger is faster. Default 4.
  int chroma_hop_multiplier;
  /// Non-zero smooths chords with a Viterbi HMM. Default 1.
  int use_chord_hmm;
  /// Non-zero biases the chord HMM transitions using the detected key. Default 0.
  int use_chord_key_context;
  /// Candidate beam width of the chord HMM smoothing. Default 24.
  int chord_hmm_beam_width;
  /// Non-zero estimates slash-chord bass notes. Default 0.
  int detect_chord_inversions;
  /// Non-zero tracks a locally updated tempo prior through beat tracking. Default 0.
  int adaptive_tempo;
  /// Local tempo context length in beats; used only when adaptive_tempo is 1. Default 8.
  int tempo_update_interval_beats;
  /// Non-zero decodes a per-beat local tempo curve into the result's beatLocalBpm array.
  /// Off by default: it is an extra output rather than a better analysis, so a
  /// caller that does not read the curve would pay a decode over the beat grid
  /// for nothing. The curve describes the beat grid it was decoded from, and
  /// beat tracking holds a fixed tempo prior unless adaptive_tempo is also 1, so
  /// measuring a tempo that moves needs both set. The values are continuous,
  /// each a weighted average in log tempo of the surrounding beat intervals: a
  /// steady tempo reads within about 1% at every beat, and a moving one is
  /// followed a few beats late.
  int compute_tempo_curve;
  /// Meter numerators the estimator scores. Only the first
  /// meter_candidate_numerator_count entries are read; each must be in [2, 32].
  /// Widening the set does not force a wider meter; the default {3, 4, 6}
  /// reproduces the library's standard result.
  int meter_candidate_numerators[SONARE_MAX_METER_CANDIDATE_NUMERATORS];
  /// Number of entries of meter_candidate_numerators that are read; at least 1.
  int meter_candidate_numerator_count;
  /// Beat unit reported for the detected meter; a power of two in [1, 32]. The
  /// estimator still reports 8 on its own when it resolves a compound meter. Default 4.
  int meter_denominator;
  /// Tuning offset of the recording in cents; 0 is concert A440. Must be in
  /// [-50, 50). Every chroma the analysis builds (key, chords, sections) is
  /// shifted by it. sonare_reference_hz_to_tuning converts a reference pitch
  /// to this unit.
  float tuning_cents;
  /// Non-zero measures the tuning from the audio instead of reading
  /// tuning_cents. sonare_music_analyze_options_default sets it to 1; a
  /// zeroed struct reads tuning_cents. The value used, given or measured, is
  /// the result's "tuningCents".
  int tuning_auto;
} SonareMusicAnalyzeOptions;

/// @brief Returns the library defaults for every field, with struct_version set.
SonareMusicAnalyzeOptions sonare_music_analyze_options_default(void);
/// @note Free @p out_json with @ref sonare_free_string.
SonareError sonare_analyze_json_ex(const float* samples, size_t length, int sample_rate,
                                   const SonareMusicAnalyzeOptions* options, char** out_json);

/// @brief Scoring options for sonare_estimate_meter_json.
/// @details Always start from sonare_meter_options_default() rather than zeroing the struct: a
///   zeroed candidate_numerator_count is rejected, not treated as "use the default set".
typedef struct {
  /// Meter numerators to score. Only the first candidate_numerator_count
  /// entries are read; each must be in [2, 32].
  int candidate_numerators[SONARE_MAX_METER_CANDIDATE_NUMERATORS];
  /// Number of entries of candidate_numerators that are read; at least 1.
  int candidate_numerator_count;
  /// Beat unit reported for the detected meter; a power of two in [1, 32]. Default 4.
  /// Reported as requested: whether a beat divides into three is measured from
  /// energy between the beats, which per-beat accents do not carry, so a
  /// compound meter is not resolvable on this path. A six whose beats accent
  /// 3+3 comes back with this denominator and the grouping {3, 3}.
  int denominator;
  /// Weight of the downbeat accent in a candidate's score. Default 1.
  float downbeat_weight;
  /// Weight of the bar-to-bar consistency in a candidate's score. Default 0.5.
  float measure_weight;
  /// Weight of the subdivision support in a candidate's score. Default 0.15.
  float subdivision_weight;
  /// Subdivision support at which a meter is resolved as compound. Default 0.85. Consulted
  /// only where there is a subdivision to measure, so it has no effect on
  /// sonare_estimate_meter_json.
  float compound_subdivision_threshold;
} SonareMeterOptions;

/// @brief Returns the library defaults for every field.
SonareMeterOptions sonare_meter_options_default(void);

/// @brief Estimates meter over a caller-supplied beat series, without audio and
///   without re-running analysis.
/// @details beat_times and beat_strengths are parallel
/// arrays of beat_count entries; beat_times must be non-decreasing.
/// AnalysisResult beatObservations.onsetStrength is the intended strength
/// source, being the windowed value the library's own downbeat pass scores;
/// beats[].strength also works but is a single unwindowed envelope frame.
/// Neither needs pre-scaling: the series is divided by its own maximum before
/// scoring, so only the accent contrast within it is read.
/// A series assembled by hand from an onset envelope -- one frame read at each
/// beat time -- is neither of those, and it carries a sample-rate dependence
/// neither of them has: a hop counted in samples frames a different amount of
/// time at each rate, so one waveform sampled at 32000, 44100 and 48000 Hz has
/// produced three different winning numerators off beat times identical to the
/// sample. Widening the read to a window around the beat does not remove it.
/// The result carries a grouping alongside the numerator: how the bar divides
/// into accent groups of two and three beats, so a seven comes back as 3+2+2 or
/// 2+2+3 rather than as a bare seven.
/// Read "searched" before treating any field as a detection: a beat series too
/// short to score any candidate reports a fixed fallback instead, whose
/// confidence is 0 rather than a middling value a caller could mistake for a
/// measurement.
/// The name "confidence" covers two quantities: on "timeSignature" it is the
/// margin over the runner-up, on a "candidates" entry the candidate's share of
/// the summed support. They are not comparable.
/// Emits the schema documented on meter_result_to_json. On any error *out_json
/// is set to NULL, so a caller may check it instead of the return code.
/// Free *out_json with sonare_free_string.
SonareError sonare_estimate_meter_json(const float* beat_times, const float* beat_strengths,
                                       size_t beat_count, const SonareMeterOptions* options,
                                       char** out_json);

/* sonare_analyze_json_ex with per-stage progress and cancellation. Either
   callback may be null. When cancelled, returns SONARE_ERROR_CANCELLED and
   leaves *out_json NULL. */
/// @note Free @p out_json with @ref sonare_free_string.
/// @param user_data Passed back to @p callback unchanged, as @p cancel_user_data is to
///   @p cancel_cb. No callback or user pointer is retained past this call.
SonareError sonare_analyze_json_ex_with_progress(
    const float* samples, size_t length, int sample_rate, const SonareMusicAnalyzeOptions* options,
    SonareAnalyzeProgressCallback callback, void* user_data, char** out_json,
    SonareCancelCallback cancel_cb, void* cancel_user_data);

/// @brief Releases a float buffer the library allocated; NULL is ignored.
void sonare_free_floats(float* ptr);
/// @brief Releases an int buffer the library allocated; NULL is ignored.
void sonare_free_ints(int* ptr);
/// @brief Releases a string the library handed to the caller; NULL is ignored.
void sonare_free_string(char* ptr);
/// @brief Releases a key-candidate array; NULL is ignored.
void sonare_free_key_candidates(SonareKeyCandidate* ptr);
/// @brief Releases the arrays of an analysis result and zeroes their counts; NULL is ignored.
void sonare_free_analysis_result(SonareAnalysisResult* result);

// Error handling
/// @brief Returns a short static description of @p error.
/// @details A string literal with static storage duration: never NULL, never
///          invalidated by any later call, and never to be freed. Contrast
///          @ref sonare_last_error_message, which is thread-local and
///          overwritten.
const char* sonare_error_message(SonareError error);

/// @brief Returns the detailed message for the most recent error on the calling thread.
/// @details The returned string is owned by libsonare and valid until the next API call on
///   the same thread. The pointer is never NULL; returns an empty string ("") when no
///   detailed message is currently recorded.
///
///   CONTRACT (read carefully):
///   - Public C-ABI calls that return SonareError CLEAR the thread-local
///     message on entry, so a detailed message can never leak into a later
///     error-code result. Diagnostic accessors and void cleanup helpers do not
///     clear it, allowing callers to release partial outputs before inspection.
///   - AUDIO-THREAD entry points are the exception: they neither clear nor
///     record a message. The first use of a thread-local object can allocate
///     TLS storage on some platforms, which would break their no-allocation
///     contract. They also never throw, so they have no exception-path detail
///     to report. The consequence is that after one of them returns an error
///     code, this function still returns whatever an earlier control-thread
///     call left behind -- do not read it as belonging to the failed call.
///     Validate and configure on the control thread instead. This class is the
///     engine block-render entry points and the realtime voice-changer
///     process / latency entry points.
///   - A message is recorded on every caught-C++-exception return path (the
///     library mapped a thrown sonare::SonareException / std::exception to a
///     SonareError); under normal memory conditions this returns the exact
///     exception's what() text. If storing that diagnostic fails, this remains
///     non-NULL and returns a static fallback while
///     @ref sonare_last_error_code retains the mapped error code. A few
///     validation paths record a message of their own.
///   - Most validation early-returns that produce an error code WITHOUT throwing
///     (e.g. a NULL out-pointer, an out-of-range sample rate, a non-finite input
///     sample) record NO message: this returns "" even though the call failed.
///     Use sonare_error_message(SonareError) for a human-readable string for ANY
///     error code; use this only for the extra detail of exception-path errors.
///   - Functions that return a handle pointer record a code (see
///     @ref sonare_last_error_code) and a message on EVERY NULL return,
///     validation returns included, because the pointer cannot carry a code.
///   - A refusal reported by a core diagnostic (an invalid room in
///     @ref sonare_synthesize_rir) records its first Error as
///     ``acoustic.code: explanation``.
/// @return Pointer to a NUL-terminated thread-local message string.
const char* sonare_last_error_message(void);

/// @brief Returns the error code recorded together with the detailed message on
///        the calling thread, or SONARE_OK when nothing is recorded.
/// @details Functions returning a handle pointer report failure as NULL; this is
///   how their caller learns the code. The same calls that clear
///   @ref sonare_last_error_message on entry clear this code, and every path
///   that records a message records the code beside it (for a SonareError
///   return, the code returned). Audio-thread entry points neither clear nor
///   record it, exactly as for the message. Diagnostic accessors do not clear it.
/// @return The recorded SonareError, or SONARE_OK.
SonareError sonare_last_error_code(void);

/// @brief One entry of the structured diagnostic channel. Caller-allocated: fill
///        it with @ref sonare_diagnostic_init before passing it to
///        @ref sonare_last_diagnostic.
/// @details @c code and @c message are borrowed: owned by the library, never
///   NULL, never freed by the caller, and valid until the next API call that records or clears a
///   diagnostic on the same thread.
///
///   The warning codes a call publishes are stable strings a caller may switch on:
///   | code                           | published by                                   |
///   |---|---|
///   | `mixing.ignored_insert_param`  | sonare_mixer_from_scene_json                   |
///   | `mixing.undeclared_send`       | sonare_mixer_from_scene_json                   |
///   | `acoustic.*`                   | sonare_synthesize_rir, sonare_room_morph,      |
///   |                                | sonare_room_morph_stereo                       |
typedef struct {
  uint32_t struct_size;               ///< Written by sonare_diagnostic_init.
  const char* code;                   ///< Stable dotted code, such as `mixing.undeclared_send`.
  const char* message;                ///< Human-readable detail.
  SonareDiagnosticSeverity severity;  ///< How serious the entry is.
} SonareDiagnostic;

/// @brief Fills @p diagnostic with empty strings, SONARE_DIAGNOSTIC_INFO and its
///        @c struct_size. A NULL @p diagnostic is ignored.
void sonare_diagnostic_init(SonareDiagnostic* diagnostic);

/// @brief Number of structured diagnostics the most recent call published on the
///        calling thread, or 0 when it published none.
/// @details The only warning channel: a call that succeeds with a recoverable
///   problem (an ignored scene key, a clamped RIR tail) publishes it here, never
///   through sonare_last_error_message.
///   - Cleared at the entry of every call that may publish, so a count read after
///     a later clean call is 0 rather than stale.
///   - Reading does not consume: the list stays valid until the next API call
///     that records or clears a diagnostic on the same thread.
size_t sonare_last_diagnostic_count(void);

/// @brief Copies diagnostic @p index of the most recent call into @p out.
/// @return SONARE_ERROR_INVALID_PARAMETER when @p out is NULL, its @c struct_size
///   is below sizeof(SonareDiagnostic), or @p index is not below
///   sonare_last_diagnostic_count(); @p out is left untouched then. Like the
///   other sonare_last_* accessors it records no error message, so the message
///   of the call being inspected survives the read.
SonareError sonare_last_diagnostic(size_t index, SonareDiagnostic* out);

// Version
/// @brief Returns the library version string (e.g. "1.2.3").
/// @details A string literal with static storage duration: never NULL, never
///          invalidated by any later call, and never to be freed.
const char* sonare_version(void);
/// @brief Returns SONARE_ENGINE_ABI_VERSION as compiled into the library.
uint32_t sonare_engine_abi_version(void);

/// @brief Builds a JSON document describing the capabilities of this build.
/// @details The document contains the library version, project and engine ABI
///   versions, platform, enabled feature families, available decode backends,
///   SIMD mode, and reported hardware concurrency.
/// @param[out] out_json Receives a NUL-terminated UTF-8 document the caller
///   releases with @ref sonare_free_string; set to NULL on failure.
/// @return SONARE_OK, or an error code with the reason in the last error.
SonareError sonare_capabilities_json(char** out_json);

/// @brief Builds a JSON document describing the host this library runs on.
/// @details Holds @c cpu (@c logical_cores, @c physical_cores), @c memory
///   (@c total_gb, @c available_gb, bytes / 1024^3) and @c parallel
///   (@c enabled, @c workers, @c strategy). A fact the platform cannot report
///   is absent from its object, never 0; @c parallel is always present and
///   assumes one core when the logical count is unknown.
/// @param[out] out_json Receives a NUL-terminated UTF-8 document the caller
///   releases with @ref sonare_free_string; set to NULL on failure.
/// @return SONARE_OK, or an error code with the reason in the last error.
SonareError sonare_system_info_json(char** out_json);

/// @brief Returns 1 when the named feature is compiled into this build, 0 otherwise.
/// @details Reads the same table as the @c features object of
///   sonare_capabilities_json(), and takes its key names (@c "ffmpeg",
///   @c "vocalEdit", @c "mastering", ...). An unknown name or NULL returns 0.
int sonare_has_feature(const char* name);

#ifdef __cplusplus
}
#endif
