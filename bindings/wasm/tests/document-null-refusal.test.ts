import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, Project } from '../dist/index.js';

// A document's content is refused by the core as InvalidFormat; the facade only
// serializes it, so a value of the wrong kind must arrive and be refused there.
const WRONG_KINDS: [string, unknown][] = [
  ['a string', '12'],
  ['a boolean', true],
  ['null', null],
  ['NaN (serialized as null)', Number.NaN],
];

describe('document values of the wrong kind', () => {
  beforeAll(async () => {
    await init();
  });

  it.each(WRONG_KINDS)('refuses %s on a MIDI-FX number field as InvalidFormat', (_kind, value) => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      for (const key of ['transposeSemitones', 'velocityScale', 'seed', 'quantizePpq']) {
        expect(() =>
          project.previewMidiFxCount({ clipId, config: { [key]: value } as never }),
        ).toThrow(
          expect.objectContaining({
            code: ErrorCode.InvalidFormat,
            message: expect.stringContaining(`${key} must be a number`),
          }),
        );
      }
    } finally {
      project.destroy();
    }
  });
});
