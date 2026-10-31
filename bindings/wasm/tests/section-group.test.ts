/**
 * Tests for the repetition `group` carried by every section.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { analyze, analyzeSections, init } from '../dist/index.js';

const SR = 22050;
const SPAN_SEC = 8;

function tone(freq: number, partials: number, sec: number): Float32Array {
  const out = new Float32Array(Math.floor(SR * sec));
  for (let i = 0; i < out.length; i++) {
    let v = 0;
    for (let h = 1; h <= partials; h++) {
      v += Math.sin((2 * Math.PI * freq * h * i) / SR) / h;
    }
    out[i] = (0.4 * v) / partials;
  }
  return out;
}

/** A-B-A-B: 8 s spans of a pure low tone and a bright six-partial tone. */
function abab(): Float32Array {
  const a = tone(220, 1, SPAN_SEC);
  const b = tone(311.13, 6, SPAN_SEC);
  const out = new Float32Array(a.length * 4);
  [a, b, a, b].forEach((span, k) => {
    out.set(span, k * a.length);
  });
  return out;
}

function expectFirstAppearanceNumbering(groups: number[]): void {
  let next = 0;
  for (const g of groups) {
    expect(Number.isInteger(g)).toBe(true);
    expect(g).toBeGreaterThanOrEqual(0);
    expect(g).toBeLessThanOrEqual(next);
    if (g === next) next++;
  }
}

describe('Section.group', () => {
  beforeAll(async () => {
    await init();
  });

  const samples = abab();

  it('analyzeSections gives an integer group per section, numbered by first appearance', () => {
    const sections = analyzeSections(samples, SR, { minSectionSec: 4 });
    expect(sections.length).toBeGreaterThan(0);
    expectFirstAppearanceNumbering(sections.map((s) => s.group));
  });

  it('analyzeSections yields at least two distinct groups on A-B-A-B', () => {
    const sections = analyzeSections(samples, SR, { minSectionSec: 4 });
    expect(new Set(sections.map((s) => s.group)).size).toBeGreaterThanOrEqual(2);
  });

  it('analyze carries the same group field', () => {
    const result = analyze(samples, SR);
    expect(result.sections.length).toBeGreaterThan(0);
    expectFirstAppearanceNumbering(result.sections.map((s) => s.group));
  });
});
