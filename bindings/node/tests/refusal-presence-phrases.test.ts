/**
 * A core refusal states absence through presence tokens, and the addon renders them in JavaScript
 * terms ("omitted" / "given"), never as the C spelling's NULL.
 */

import { describe, expect, it } from 'vitest';
import { Project, renderNotes } from '../src/index.js';
import { addon } from '../src/native.js';
import { expectRefusal } from './_helpers';

const INVALID_PARAMETER = 4;
const REFUSAL = Symbol.for('libsonare.refusal');

interface Rendered {
  message: string;
  refusal: { text: string; predicate: string };
}

const render = (form: 'predicate' | 'relation', subjects: string[], predicate: string) =>
  addon.renderRefusal(
    {
      code: INVALID_PARAMETER,
      form,
      subjects,
      predicate,
      renamedTo: '',
      expected: '',
      callerSpelled: true,
    },
    subjects,
    subjects.map(() => 1),
  ) as Rendered;

const caught = (run: () => unknown): Error => {
  try {
    run();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected the call to throw');
};

describe('presence tokens in a refusal', () => {
  it.each([
    ['predicate', ['a'], '{must_be_absent}', 'a must be omitted'],
    ['predicate', ['a'], '{must_be_present}', 'a must be given'],
    [
      'relation',
      ['a', 'b'],
      '{0} and {1} {must_not_both_be_absent}',
      'a and b must not both be omitted',
    ],
    [
      'relation',
      ['a', 'b'],
      '{0} {must_be_present} when {1} {is_absent}',
      'a must be given when b is omitted',
    ],
    [
      'relation',
      ['a', 'b'],
      '{0} {must_be_absent} when {1} {is_present}',
      'a must be omitted when b is given',
    ],
  ] as const)('renders %s %s %s as "%s"', (form, subjects, predicate, text) => {
    const rendered = render(form, [...subjects], predicate);
    expect(rendered.message).toBe(text);
    expect(rendered.refusal.text).toBe(text);
    // The token itself stays in the parts, so a surface renders it in its own terms.
    expect(rendered.refusal.predicate).toBe(predicate);
  });

  it('words an insert list outside the chain mode as one to omit, and a missing one as one to give', () => {
    using project = new Project();
    const set = (request: object) => () =>
      project.setPartRig({ destinationId: 0, part: 0, ...request } as never);
    const omitted = caught(
      set({ mode: 'none', inserts: [{ processor: 'saturation.overdrive', params: {} }] }),
    );
    expectRefusal(
      () => {
        throw omitted;
      },
      RangeError,
      /must be omitted unless mode is chain$/,
    );
    expect((omitted as never as Record<symbol, { text: string }>)[REFUSAL].text).toMatch(
      /must be omitted unless mode is chain$/,
    );
    expectRefusal(set({ mode: 'chain' }), RangeError, /must be given when mode is chain$/);
  });

  it('words a curve edit with no track as one made while the track is omitted', () => {
    const sampleRate = 22050;
    const samples = new Float32Array(11000);
    const note = {
      onsetSample: 0,
      offsetSample: samples.length / 2,
      frameStart: 0,
      frameEnd: 10,
      medianHz: 220,
    };
    const later = { ...note, onsetSample: samples.length / 2, offsetSample: samples.length };
    const error = caught(() =>
      renderNotes({
        samples,
        sampleRate,
        notes: [note, { ...later, edit: { driftChange: 1 } }],
      }),
    );
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toBe(
      'renderNotes: notes[1].edit.vibratoDepthChange and notes[1].edit.driftChange must be 0 when f0Hz is omitted',
    );
  });
});
