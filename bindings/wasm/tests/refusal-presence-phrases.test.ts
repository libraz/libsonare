/**
 * A core refusal states absence through presence tokens, and the module renders them in JavaScript
 * terms ("omitted" / "given"), never as the C spelling's NULL.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, Project } from '../dist/index.js';
import { getSonareModule } from '../src/module_state.js';
import { expectRefusalOf } from './_helpers';
import { shareBuiltModuleWithSource } from './_module_realm.js';

const INVALID_PARAMETER = 4;
const REFUSAL = Symbol.for('libsonare.refusal');

interface Rendered {
  message: string;
  refusal: { text: string; predicate: string };
}

const render = (form: 'predicate' | 'relation', subjects: string[], predicate: string) =>
  (
    getSonareModule() as unknown as {
      renderRefusal: (r: object, s: string[], k: number[]) => Rendered;
    }
  ).renderRefusal(
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
  );

const caught = (run: () => unknown): Error => {
  try {
    run();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected the call to throw');
};

describe('presence tokens in a refusal', () => {
  beforeAll(async () => {
    await init();
    shareBuiltModuleWithSource();
  });

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
    expect(omitted).toBeInstanceOf(RangeError);
    expect(omitted.message).toMatch(/must be omitted unless mode is chain$/);
    expect((omitted as never as Record<symbol, { text: string }>)[REFUSAL].text).toMatch(
      /must be omitted unless mode is chain$/,
    );
    expect(expectRefusalOf(set({ mode: 'chain' })).message).toMatch(
      /must be given when mode is chain$/,
    );
  });
});
