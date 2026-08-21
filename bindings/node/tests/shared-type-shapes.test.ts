/**
 * Public type names the Node and WASM packages both export must mean the same
 * shape.
 *
 * `DynamicsResult` used to be the analysis metrics in one package and the
 * processor envelope `{ samples, latencySamples }` in the other, so a shared
 * TypeScript module annotated with it type-checked against one package and
 * failed against the other — the identifier resolved either way and only the
 * member list gave it away.
 *
 * The two packages are separate npm packages and cannot import one module, so
 * what makes a drift red is this: each package builds a sample object with
 * `satisfies`, which is exact (a missing field is a compile error, an extra one
 * is an excess-property error), and then compares that object's keys against
 * `tests/conformance/shared_type_shapes.json`. Adding, removing or renaming a
 * field on either side fails that package's own suite against the shared file,
 * and the sibling suite reads the same file.
 */

import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';
import type {
  DynamicsAnalysisResult,
  DynamicsProcessorResult,
  DynamicsResult,
} from '../src/index.js';

interface SharedTypeShapes {
  types: Record<string, string[]>;
  aliases: Record<string, string>;
}

const shapes = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/shared_type_shapes.json', import.meta.url),
    'utf8',
  ),
) as SharedTypeShapes;

// Exact by construction: `satisfies` rejects a missing or extra field, so these
// literals cannot drift from the interfaces without a compile error.
const dynamicsResult = {
  dynamicRangeDb: 0,
  peakDb: 0,
  rmsDb: 0,
  crestFactor: 0,
  loudnessRangeDb: 0,
  isCompressed: false,
  loudnessTimes: new Float32Array(0),
  loudnessRmsDb: new Float32Array(0),
} satisfies DynamicsResult;

const dynamicsProcessorResult = {
  samples: new Float32Array(0),
  latencySamples: 0,
} satisfies DynamicsProcessorResult;

describe('shared public type shapes', () => {
  it('DynamicsResult is the analysis shape the shared corpus declares', () => {
    expect(Object.keys(dynamicsResult).sort()).toEqual([...shapes.types.DynamicsResult].sort());
  });

  it('DynamicsProcessorResult is the processor envelope the shared corpus declares', () => {
    expect(Object.keys(dynamicsProcessorResult).sort()).toEqual(
      [...shapes.types.DynamicsProcessorResult].sort(),
    );
  });

  it('the two shapes stay disjoint, so a mix-up cannot be silent', () => {
    const analysis = new Set(shapes.types.DynamicsResult);
    const overlap = shapes.types.DynamicsProcessorResult.filter((key) => analysis.has(key));
    expect(overlap).toEqual([]);
  });

  it('DynamicsAnalysisResult remains an alias of DynamicsResult', () => {
    expect(shapes.aliases.DynamicsAnalysisResult).toBe('DynamicsResult');
    // Assignable in both directions, which an alias is and a separate
    // interface of the same shape would also be — the corpus entry above is
    // what pins it to being the same name in both packages.
    const asAlias: DynamicsAnalysisResult = dynamicsResult;
    const asCanonical: DynamicsResult = asAlias;
    expect(asCanonical.isCompressed).toBe(false);
  });
});
