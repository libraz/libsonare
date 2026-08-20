/**
 * The Node native addon calls several C++ core entry points directly,
 * bypassing the C-ABI translation unit's input validation. Those entry
 * points now re-apply `sonare::validate_offline_audio_input` themselves so
 * NaN/Inf samples and out-of-range sample rates are rejected identically to
 * the Python (ctypes) and WASM (embind) surfaces, instead of silently
 * producing garbage or NaN output.
 */

import { describe, expect, it } from 'vitest';
import { hpss, masterAudio, mastering } from '../src/index.js';

const SR = 44100;

function sine(n: number, freq = 220, amp = 0.5): Float32Array {
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

function withNaN(n: number): Float32Array {
  const buf = sine(n);
  buf[10] = Number.NaN;
  return buf;
}

function withInf(n: number): Float32Array {
  const buf = sine(n);
  buf[10] = Number.POSITIVE_INFINITY;
  return buf;
}

describe('addon direct-core-call input validation (Node)', () => {
  describe('mastering()', () => {
    it('rejects NaN samples', () => {
      expect(() => mastering(withNaN(2048), SR)).toThrow();
    });
    it('rejects Infinity samples', () => {
      expect(() => mastering(withInf(2048), SR)).toThrow();
    });
    it('rejects an out-of-range sample rate', () => {
      expect(() => mastering(sine(2048), 100)).toThrow();
    });
    it('does not throw on valid input at 44100 Hz', () => {
      expect(() => mastering(sine(2048), SR)).not.toThrow();
    });
  });

  describe('masterAudio()', () => {
    it('rejects NaN samples', () => {
      expect(() => masterAudio(withNaN(4096), SR)).toThrow();
    });
    it('rejects Infinity samples', () => {
      expect(() => masterAudio(withInf(4096), SR)).toThrow();
    });
    it('rejects an out-of-range sample rate', () => {
      expect(() => masterAudio(sine(4096), 100)).toThrow();
    });
    it('does not throw on valid input at 44100 Hz', () => {
      expect(() => masterAudio(sine(4096), SR)).not.toThrow();
    });
  });

  describe('hpss()', () => {
    it('rejects NaN samples', () => {
      expect(() => hpss(withNaN(2048), SR)).toThrow();
    });
    it('rejects Infinity samples', () => {
      expect(() => hpss(withInf(2048), SR)).toThrow();
    });
    it('rejects an out-of-range sample rate', () => {
      expect(() => hpss(sine(2048), 100)).toThrow();
    });
    it('does not throw on valid input at 44100 Hz', () => {
      expect(() => hpss(sine(2048), SR)).not.toThrow();
    });
  });
});
