/**
 * WASM mastering input-validation guards that mirror the C ABI. The C-ABI TU is
 * not linked into the WASM build, so these entry points re-implement the guards
 * the C ABI performs: reject a non-positive sample rate, an empty buffer, or any
 * NaN/Inf sample, and reject a stereo call whose channel lengths differ. Also
 * covers the RealtimeEngine SysEx empty-frame rejection.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masteringPairAnalysisNames,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringProcess,
  masteringProcessorNames,
  masteringStereoAnalysisNames,
  masteringStereoAnalyze,
  RealtimeEngine,
} from '../dist/index.js';

beforeAll(async () => {
  await init();
});

function nanBuffer(n: number): Float32Array {
  const buf = new Float32Array(n);
  buf[Math.floor(n / 2)] = Number.NaN;
  return buf;
}

describe('masteringProcess offline-audio input guards', () => {
  it('rejects a non-positive sample rate', () => {
    const name = masteringProcessorNames()[0];
    expect(name).toBeTruthy();
    expect(() => masteringProcess(name, new Float32Array(1024), 0, {})).toThrow();
  });

  it('rejects a NaN-containing buffer', () => {
    const name = masteringProcessorNames()[0];
    expect(() => masteringProcess(name, nanBuffer(1024), 22050, {})).toThrow();
  });

  it('rejects an empty buffer', () => {
    const name = masteringProcessorNames()[0];
    expect(() => masteringProcess(name, new Float32Array(0), 22050, {})).toThrow();
  });

  it('accepts a valid buffer and sample rate', () => {
    const name = masteringProcessorNames()[0];
    const buf = new Float32Array(1024);
    for (let i = 0; i < buf.length; i++) {
      buf[i] = Math.sin((i / buf.length) * Math.PI * 2) * 0.25;
    }
    expect(() => masteringProcess(name, buf, 22050, {})).not.toThrow();
  });
});

describe('masteringStereoAnalyze channel-length guard', () => {
  it('rejects mismatched left/right lengths', () => {
    const name = masteringStereoAnalysisNames()[0];
    expect(name).toBeTruthy();
    const left = new Float32Array(1024);
    const right = new Float32Array(512);
    expect(() => masteringStereoAnalyze(name, left, right, 22050, {})).toThrow();
  });

  it('does not throw the length guard for equal valid lengths', () => {
    const name = masteringStereoAnalysisNames()[0];
    const left = new Float32Array(1024);
    const right = new Float32Array(1024);
    for (let i = 0; i < left.length; i++) {
      const s = Math.sin((i / left.length) * Math.PI * 2) * 0.25;
      left[i] = s;
      right[i] = s;
    }
    // Equal lengths + a valid sample rate must not trip the length-mismatch or
    // input-validation guard. The analysis itself may still succeed or fail for
    // unrelated reasons; only the length-mismatch case is asserted to throw.
    expect(() => masteringStereoAnalyze(name, left, right, 22050, {})).not.toThrow();
  });
});

describe('mastering pair input guards', () => {
  it('rejects a NaN buffer in a pair processor', () => {
    const name = masteringPairProcessorNames()[0];
    expect(name).toBeTruthy();
    const source = nanBuffer(1024);
    const reference = new Float32Array(1024);
    expect(() => masteringPairProcess(name, source, reference, 22050, {})).toThrow();
  });

  it('rejects a NaN buffer in a pair analysis', () => {
    const name = masteringPairAnalysisNames()[0];
    expect(name).toBeTruthy();
    const source = nanBuffer(1024);
    const reference = new Float32Array(1024);
    expect(() => masteringPairAnalyze(name, source, reference, 22050, {})).toThrow();
  });
});

describe('RealtimeEngine pushMidiSysex empty-frame guard', () => {
  it('rejects an empty SysEx frame', () => {
    const engine = new RealtimeEngine(48000, 128);
    // The empty-frame check runs before any queue interaction, so a bare engine
    // is enough to exercise the InvalidParameter rejection path.
    expect(() => engine.pushMidiSysex(0, new Uint8Array(0))).toThrow();
  });
});
