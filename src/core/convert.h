#pragma once

/// @file convert.h
/// @brief Unit conversion functions for audio/music processing.

#include <cmath>
#include <string>
#include <vector>

namespace sonare {

/// @brief Converts Hz to mel frequency (Slaney formula).
/// @param hz Frequency in Hz
/// @return Mel frequency
/// @details Uses the Slaney-style mel formula:
///          Linear below 1000 Hz, logarithmic above.
float hz_to_mel(float hz);

/// @brief Converts mel frequency to Hz (Slaney formula).
/// @param mel Mel frequency
/// @return Frequency in Hz
float mel_to_hz(float mel);

/// @brief Converts Hz to mel frequency (HTK formula).
/// @param hz Frequency in Hz
/// @return Mel frequency (HTK scale)
float hz_to_mel_htk(float hz);

/// @brief Converts mel frequency to Hz (HTK formula).
/// @param mel Mel frequency (HTK scale)
/// @return Frequency in Hz
float mel_to_hz_htk(float mel);

/// @brief Converts Hz to MIDI note number.
/// @param hz Frequency in Hz
/// @return MIDI note number (A4 = 440Hz = 69). Returns -inf for non-positive
///         Hz, since 0 is a valid MIDI value (C-1) and cannot double as an
///         invalid-input sentinel.
float hz_to_midi(float hz);

/// @brief Converts MIDI note number to Hz.
/// @param midi MIDI note number
/// @return Frequency in Hz
float midi_to_hz(float midi);

/// @brief Converts Hz to note name.
/// @param hz Frequency in Hz
/// @return Note name (e.g., "A4", "C#5")
std::string hz_to_note(float hz);

/// @brief Converts note name to Hz.
/// @param note Note name (e.g., "A4", "C#5", "Db4")
/// @return Frequency in Hz
float note_to_hz(const std::string& note);

/// @brief Converts frame index to time in seconds.
/// @param frames Frame index
/// @param sr Sample rate
/// @param hop_length Hop length in samples
/// @return Time in seconds
float frames_to_time(int frames, int sr, int hop_length);

/// @brief Converts time in seconds to frame index.
/// @param time Time in seconds
/// @param sr Sample rate
/// @param hop_length Hop length in samples
/// @return Frame index (floor of time * sr / hop_length)
/// @note Uses floor for deterministic frame alignment.
int time_to_frames(float time, int sr, int hop_length);

/// @brief Converts sample count to time in seconds.
/// @param samples Number of samples
/// @param sr Sample rate
/// @return Time in seconds
float samples_to_time(int samples, int sr);

/// @brief Converts time in seconds to sample count.
/// @param time Time in seconds
/// @param sr Sample rate
/// @return Number of samples
int time_to_samples(float time, int sr);

/// @brief Converts a frame index to a sample index (librosa compatible).
/// @param frames Frame index
/// @param hop_length Hop length in samples
/// @param n_fft FFT size used to center frames. If > 0, n_fft/2 is added to
///        the result (mirrors librosa's `n_fft` argument, default behavior is
///        no centering offset).
/// @return Sample index of the frame's hop start (+ n_fft/2 when applicable).
int frames_to_samples(int frames, int hop_length, int n_fft = 0);

/// @brief Vector variant of frames_to_samples.
std::vector<int> frames_to_samples(const std::vector<int>& frames, int hop_length, int n_fft = 0);

/// @brief Converts a sample index to a frame index (floor).
/// @param samples Sample index
/// @param hop_length Hop length in samples
/// @param n_fft FFT size. If > 0, n_fft/2 is subtracted before dividing by
///        hop_length (mirrors librosa).
/// @return Frame index.
int samples_to_frames(int samples, int hop_length, int n_fft = 0);

/// @brief Vector variant of samples_to_frames.
std::vector<int> samples_to_frames(const std::vector<int>& samples, int hop_length, int n_fft = 0);

/// @brief Converts FFT bin index to Hz.
/// @param bin Bin index
/// @param sr Sample rate
/// @param n_fft FFT size
/// @return Frequency in Hz
float bin_to_hz(int bin, int sr, int n_fft);

/// @brief Converts Hz to nearest FFT bin index.
/// @param hz Frequency in Hz
/// @param sr Sample rate
/// @param n_fft FFT size
/// @return Bin index
int hz_to_bin(float hz, int sr, int n_fft);

}  // namespace sonare
