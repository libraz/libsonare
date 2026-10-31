import { describe, expect, it } from 'vitest';
import { analyze, analyzeSections } from '../src/index.js';

const SR = 22050;
const SPAN_SEC = 8;

function partials(freq: number, count: number, seconds: number): Float32Array {
  const n = Math.floor(SR * seconds);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    let v = 0;
    for (let k = 1; k <= count; k++) {
      v += Math.sin((2 * Math.PI * freq * k * i) / SR) / k;
    }
    out[i] = (0.4 * v) / count;
  }
  return out;
}

/** A-B-A-B: 220 Hz with 1 partial vs 311.13 Hz with 6 partials. */
function abab(): Float32Array {
  const a = partials(220, 1, SPAN_SEC);
  const b = partials(311.13, 6, SPAN_SEC);
  const out = new Float32Array(a.length * 4);
  out.set(a, 0);
  out.set(b, a.length);
  out.set(a, a.length * 2);
  out.set(b, a.length * 3);
  return out;
}

function expectFirstAppearanceNumbering(groups: number[]): void {
  let next = 0;
  for (const g of groups) {
    expect(Number.isInteger(g)).toBe(true);
    expect(g).toBeGreaterThanOrEqual(0);
    expect(g).toBeLessThanOrEqual(next);
    if (g === next) {
      next++;
    }
  }
}

describe('section group', () => {
  const samples = abab();

  it('analyzeSections gives every section an integer group numbered by first appearance', () => {
    const sections = analyzeSections(samples, SR, { minSectionSec: 4 });
    expect(sections.length).toBeGreaterThan(0);
    expectFirstAppearanceNumbering(sections.map((s) => s.group));
  });

  it('analyze gives every section an integer group numbered by first appearance', () => {
    const { sections } = analyze(samples, SR);
    expect(sections.length).toBeGreaterThan(0);
    expectFirstAppearanceNumbering(sections.map((s) => s.group));
  });

  it('an A-B-A-B fixture yields at least two distinct groups', () => {
    const sections = analyzeSections(samples, SR, { minSectionSec: 4 });
    expect(new Set(sections.map((s) => s.group)).size).toBeGreaterThanOrEqual(2);
  });
});
