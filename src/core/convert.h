#pragma once

/// @file convert.h
/// @brief Unit conversion functions for audio/music processing.

#include <cmath>
#include <string>

namespace sonare {

/// @brief Converts Hz to mel frequency (Slaney formula).
/// @param hz Frequency in Hz
/// @return Mel frequency
/// @details Uses librosa default (Slaney) formula:
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
/// @return MIDI note number (A4 = 440Hz = 69)
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
/// @return Frame index (floor of time * sr / hop_length, librosa compatible)
/// @note Uses floor for consistency with librosa.core.time_to_frames.
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
