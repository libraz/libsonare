import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError } from '../src/errors.js';
import { Audio, Mixer } from '../src/index.js';
import { addonSources } from './_addon_sources.js';

/**
 * Every failure raised by the addon carries the C-ABI code. A raw
 * `Napi::Error::New` yields a plain Error with no `code`, so the only file
 * allowed to construct one is the helper that stamps it.
 */
const HELPER_FILE = 'sonare_wrap_utils.cpp';

describe('addon error contract', () => {
  it('constructs no raw Napi::Error outside the stamping helper', () => {
    const sources = addonSources();
    expect(sources.length).toBeGreaterThan(20);
    expect(sources.some((source) => source.file === HELPER_FILE)).toBe(true);
    const offenders = sources
      .filter((source) => source.file !== HELPER_FILE)
      .filter((source) => /Napi::Error::New\s*\(/.test(source.text.replace(/\/\/.*$/gm, '')))
      .map((source) => source.file);
    expect(offenders).toEqual([]);
  });

  it('reports a scene that fails to load with the library code', () => {
    let caught: unknown;
    try {
      Mixer.fromSceneJson('{ not json');
    } catch (error) {
      caught = error;
    }
    expect(isSonareError(caught)).toBe(true);
    expect((caught as { code: number }).code).not.toBe(ErrorCode.Ok);
  });

  it('reports use of a destroyed handle as InvalidState', () => {
    const audio = Audio.fromBuffer(new Float32Array(2205), 22050);
    audio.destroy();
    let caught: unknown;
    try {
      audio.rmsDb();
    } catch (error) {
      caught = error;
    }
    expect(isSonareError(caught)).toBe(true);
    expect((caught as { code: number }).code).toBe(ErrorCode.InvalidState);
    expect((caught as Error).message).toBe('Audio has been destroyed');
  });
});
