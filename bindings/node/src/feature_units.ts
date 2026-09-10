import { addon } from './native.js';

export interface ValuesRequest {
  values: Float32Array;
}

export function hzToMel(hz: number): number {
  return addon.hzToMel(hz);
}

export function melToHz(mel: number): number {
  return addon.melToHz(mel);
}

export function hzToMidi(hz: number): number {
  return addon.hzToMidi(hz);
}

export function midiToHz(midi: number): number {
  return addon.midiToHz(midi);
}

export function hzToNote(hz: number): string {
  return addon.hzToNote(hz);
}

export function noteToHz(note: string): number {
  return addon.noteToHz(note);
}

export function framesToTime(request: { frames: number; sr?: number; hopLength?: number }): number;
export function framesToTime(frames: number, sr?: number, hopLength?: number): number;
export function framesToTime(
  frames: number | { frames: number; sr?: number; hopLength?: number },
  sr = 22050,
  hopLength = 512,
): number {
  const request = typeof frames === 'number' ? { frames, sr, hopLength } : frames;
  return addon.framesToTime(request.frames, request.sr ?? 22050, request.hopLength ?? 512);
}

export function timeToFrames(request: { time: number; sr?: number; hopLength?: number }): number;
export function timeToFrames(time: number, sr?: number, hopLength?: number): number;
export function timeToFrames(
  time: number | { time: number; sr?: number; hopLength?: number },
  sr = 22050,
  hopLength = 512,
): number {
  const request = typeof time === 'number' ? { time, sr, hopLength } : time;
  return addon.timeToFrames(request.time, request.sr ?? 22050, request.hopLength ?? 512);
}

export function framesToSamples(request: {
  frames: number;
  hopLength?: number;
  nFft?: number;
}): number;
export function framesToSamples(frames: number, hopLength?: number, nFft?: number): number;
export function framesToSamples(
  frames: number | { frames: number; hopLength?: number; nFft?: number },
  hopLength = 512,
  nFft = 0,
): number {
  const request = typeof frames === 'number' ? { frames, hopLength, nFft } : frames;
  return addon.framesToSamples(request.frames, request.hopLength ?? 512, request.nFft ?? 0);
}

export function samplesToFrames(request: {
  samples: number;
  hopLength?: number;
  nFft?: number;
}): number;
export function samplesToFrames(samples: number, hopLength?: number, nFft?: number): number;
export function samplesToFrames(
  samples: number | { samples: number; hopLength?: number; nFft?: number },
  hopLength = 512,
  nFft = 0,
): number {
  const request = typeof samples === 'number' ? { samples, hopLength, nFft } : samples;
  return addon.samplesToFrames(request.samples, request.hopLength ?? 512, request.nFft ?? 0);
}

export function powerToDb(
  request: ValuesRequest & { ref?: number; amin?: number; topDb?: number },
): Float32Array;
export function powerToDb(
  values: Float32Array,
  ref?: number,
  amin?: number,
  topDb?: number,
): Float32Array;
export function powerToDb(
  values: Float32Array | (ValuesRequest & { ref?: number; amin?: number; topDb?: number }),
  ref = 1.0,
  amin = 1e-10,
  topDb = 80.0,
): Float32Array {
  const request = values instanceof Float32Array ? { values, ref, amin, topDb } : values;
  return addon.powerToDb(
    request.values,
    request.ref ?? 1,
    request.amin ?? 1e-10,
    request.topDb ?? 80,
  );
}

export function amplitudeToDb(
  request: ValuesRequest & { ref?: number; amin?: number; topDb?: number },
): Float32Array;
export function amplitudeToDb(
  values: Float32Array,
  ref?: number,
  amin?: number,
  topDb?: number,
): Float32Array;
export function amplitudeToDb(
  values: Float32Array | (ValuesRequest & { ref?: number; amin?: number; topDb?: number }),
  ref = 1.0,
  amin = 1e-5,
  topDb = 80.0,
): Float32Array {
  const request = values instanceof Float32Array ? { values, ref, amin, topDb } : values;
  return addon.amplitudeToDb(
    request.values,
    request.ref ?? 1,
    request.amin ?? 1e-5,
    request.topDb ?? 80,
  );
}

export function dbToPower(values: Float32Array, ref = 1.0): Float32Array {
  return addon.dbToPower(values, ref);
}

export function dbToAmplitude(values: Float32Array, ref = 1.0): Float32Array {
  return addon.dbToAmplitude(values, ref);
}
