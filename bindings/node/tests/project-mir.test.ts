import { describe, expect, it } from 'vitest';
import { Project } from '../src/index.js';

describe('Project snapToGrid', () => {
  const withProject = (body: (project: Project) => void): void => {
    const project = new Project();
    try {
      project.setSampleRate(48000);
      body(project);
    } finally {
      project.destroy();
    }
  };

  it('snaps a coordinate to the bar, beat and subdivision grid', () => {
    withProject((project) => {
      // `division` defaults to beat lines, 0 selects bar lines, and >= 2 that
      // many equal subdivisions per beat.
      expect(project.snapToGrid(1.02, 1.0)).toBe(1.0);
      expect(project.snapToGrid(1.02, 1.0, 1)).toBe(1.0);
      expect(project.snapToGrid(0.27, 1.0, 4)).toBe(0.25);
      expect(project.snapToGrid(4.02, 1.0, 0)).toBe(4.0);
    });
  });

  it('leaves a coordinate alone at zero strength', () => {
    withProject((project) => {
      expect(project.snapToGrid(1.02, 0.0)).toBeCloseTo(1.02, 12);
    });
  });

  it('rejects a negative division', () => {
    withProject((project) => {
      // -1 is representable as an int, so this is the C ABI's own rejection
      // rather than the argument guard's.
      expect(() => project.snapToGrid(1.0, 1.0, -1)).toThrow(RangeError);
    });
  });
});

// The MIR entry points hand integer arguments straight to the C ABI: int sample
// rates, an int grid division, int program/bank numbers, and size_t indices. A
// NaN, an infinity or a magnitude past the target type reaching the
// float-to-integer conversion is undefined behaviour, so each argument is
// range-checked before the cast rather than after it.
describe('Project MIR integer arguments are range-checked', () => {
  const UNREPRESENTABLE_AS_INT = [
    ['NaN', Number.NaN],
    ['Infinity', Number.POSITIVE_INFINITY],
    ['-Infinity', Number.NEGATIVE_INFINITY],
    ['a magnitude past the int range', 2 ** 40],
    ['a magnitude below the int range', -(2 ** 40)],
    ['a fraction', 1.5],
  ] as const;

  const UNREPRESENTABLE_AS_SIZE = [
    ['NaN', Number.NaN],
    ['Infinity', Number.POSITIVE_INFINITY],
    ['a negative index', -1],
    ['a fraction', 1.5],
    ['a magnitude past Number.MAX_SAFE_INTEGER', 2 ** 64],
  ] as const;

  const withProject = (body: (project: Project) => void): void => {
    const project = new Project();
    try {
      project.setSampleRate(48000);
      body(project);
    } finally {
      project.destroy();
    }
  };

  // The guards fire before any analysis runs, so a token buffer is enough.
  const silence = () => new Float32Array(1024);

  // One case per guarded argument, so a guard that goes missing is named by the
  // failing test rather than hidden behind whichever site is asserted first.
  type Site = readonly [string, (project: Project, clipId: number, value: number) => unknown];

  const INT_SITES: readonly Site[] = [
    ['setProgram program', (project, clipId, value) => project.setProgram(clipId, value)],
    [
      'setProgram bank',
      (project, clipId, value) => project.setProgram(clipId, 24, { bank: { msb: value, lsb: 0 } }),
    ],
    ['autoTempo sampleRate', (project, _clipId, value) => project.autoTempo(silence(), value)],
    [
      'analyzeTempo sampleRate',
      (project, _clipId, value) => project.analyzeTempo(silence(), value),
    ],
    ['snapToGrid division', (project, _clipId, value) => project.snapToGrid(1.02, 1.0, value)],
  ];

  const SIZE_SITES: readonly Site[] = [
    [
      'autoTempo candidateIndex',
      (project, _clipId, value) => project.autoTempo(silence(), 22050, { candidateIndex: value }),
    ],
  ];

  for (const [site, invoke] of INT_SITES) {
    it(`rejects a number no int can hold at ${site}`, () => {
      withProject((project) => {
        const { clipId } = project.addMidiClip(0, 4);
        for (const [label, value] of UNREPRESENTABLE_AS_INT) {
          expect(() => invoke(project, clipId, value), `${site} accepted ${label}`).toThrow(
            RangeError,
          );
        }
      });
    });
  }

  for (const [site, invoke] of SIZE_SITES) {
    it(`rejects a number no size_t can hold at ${site}`, () => {
      withProject((project) => {
        const { clipId } = project.addMidiClip(0, 4);
        for (const [label, value] of UNREPRESENTABLE_AS_SIZE) {
          expect(() => invoke(project, clipId, value), `${site} accepted ${label}`).toThrow(
            RangeError,
          );
        }
      });
    });
  }

  it('rejects a non-number where an integer argument is expected', () => {
    withProject((project) => {
      const { clipId } = project.addMidiClip(0, 4);
      expect(() => project.setProgram(clipId, '24' as never)).toThrow(TypeError);
      // An optional argument reads null like an omitted one, matching the
      // group/channel readers on the same entry points.
      expect(project.snapToGrid(1.02, 1.0, null as never)).toBe(project.snapToGrid(1.02, 1.0));
    });
  });

  // Non-vacuity: the values the guards accept still reach the C ABI, so the
  // rejections above are the guards discriminating rather than a blanket throw.
  it('passes an in-range integer argument through to the C ABI', () => {
    withProject((project) => {
      const { clipId } = project.addMidiClip(0, 4);
      expect(() => project.setProgram(clipId, 24)).not.toThrow();
      // An omitted bank emits no Bank Select; an explicit bank is a real value.
      expect(() => project.setProgram(clipId, 24, { bank: { msb: 0, lsb: 0 } })).not.toThrow();
      expect(() => project.setProgram(clipId, 24, { group: 0, channel: 3 })).not.toThrow();
      expect(project.snapToGrid(0.27, 1.0, 4)).toBe(0.25);
      expect(project.assistSidecars()).toEqual([]);
    });
  });
});
