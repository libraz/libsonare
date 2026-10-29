import { describe, expect, it } from 'vitest';
import type { ProjectVocalEditApplyRequest } from '../src/types_vocal_project.js';
import { projectApplyVocalEdit, projectRehydrateVocalEdits } from '../src/vocal_project.js';

const SHA = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef';
const TOKEN = {
  sessionEpoch: '1',
  revision: '2',
  draftId: '0',
  generation: '0',
  requestId: '3',
  profileId: 4,
};
const request = {
  clipId: 7,
  expectedSourceId: 11,
  expectedSourceSampleRate: 48000,
  expectedSourceSampleCount: 3,
  expectedSourceSha256: SHA,
  expectedClipLengthPpq: 4,
  expectedSourceOffsetPpq: 0,
  renderedMono: new Float32Array([0, 0.25, 0.5]),
  renderedSampleRate: 48000,
  renderToken: TOKEN,
  sve1: new Uint8Array([83, 86, 69, 49]),
};
const native = {
  applyVocalEdit: () => {
    throw new Error('native apply reached');
  },
  getVocalEditDependencies: () => [],
  rehydrateVocalEdits: () => {
    throw new Error('native rehydrate reached');
  },
} as Parameters<typeof projectApplyVocalEdit>[0];

function apply(overrides: Record<string, unknown>): void {
  projectApplyVocalEdit(native, {
    ...request,
    ...overrides,
  } as unknown as ProjectVocalEditApplyRequest);
}

describe('Node Project vocal input number errors', () => {
  it.each([
    ['expectedClipLengthPpq', '4'],
    ['expectedSourceSampleRate', '48000'],
    ['expectedSourceSampleCount', '3'],
    ['clipId', '7'],
    ['takeId', '0'],
    ['expectedSourceId', '11'],
    ['renderedStartSample', '0'],
    ['renderToken', { ...TOKEN, profileId: '4' }],
  ])('reports TypeError for a non-number %s', (field, value) => {
    expect(() => apply({ [field]: value })).toThrow(TypeError);
  });

  it.each([
    ['expectedClipLengthPpq', Number.NaN],
    ['expectedSourceOffsetPpq', Number.POSITIVE_INFINITY],
    ['expectedSourceSampleRate', Number.NaN],
    ['expectedSourceSampleCount', Number.POSITIVE_INFINITY],
    ['clipId', Number.NaN],
    ['renderedStartSample', Number.NaN],
    ['renderToken', { ...TOKEN, profileId: Number.NaN }],
  ])('reports RangeError for a numeric value outside the %s domain', (field, value) => {
    expect(() => apply({ [field]: value })).toThrow(RangeError);
  });

  it.each([
    ['sourceId', '7'],
    ['sampleRate', '48000'],
  ])('reports TypeError for a non-number rehydrate %s', (field, value) => {
    expect(() =>
      projectRehydrateVocalEdits(native, [
        { sourceId: 7, mono: new Float32Array([0.1]), sampleRate: 48000, [field]: value },
      ] as never),
    ).toThrow(TypeError);
  });

  it.each([
    ['sourceId', Number.NaN],
    ['sampleRate', Number.POSITIVE_INFINITY],
  ])('reports RangeError for a numeric rehydrate %s outside its domain', (field, value) => {
    expect(() =>
      projectRehydrateVocalEdits(native, [
        { sourceId: 7, mono: new Float32Array([0.1]), sampleRate: 48000, [field]: value },
      ] as never),
    ).toThrow(RangeError);
  });
});
