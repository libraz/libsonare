import { readdirSync, readFileSync } from 'node:fs';
import { afterAll, describe, expect, it } from 'vitest';
import { Project } from '../src/index.js';

// Every function the C headers declare; a message prefix naming one is a C entry name.
const INCLUDE = new URL('../../../include/sonare/', import.meta.url);
const C_ENTRIES = new Set(
  readdirSync(INCLUDE)
    .filter((file) => file.endsWith('.h'))
    .flatMap((file) =>
      [...readFileSync(new URL(file, INCLUDE), 'utf8').matchAll(/\b(sonare_[a-z0-9_]+)\s*\(/g)].map(
        (match) => match[1],
      ),
    ),
);
const SECOND_PREFIX = /^[A-Za-z][\w.]*: ([a-z][a-z0-9_]*): /;

function refusalOf(run: () => unknown): string {
  try {
    run();
  } catch (error) {
    return (error as Error).message;
  }
  throw new Error('expected a refusal');
}

function withProject(body: (project: Project) => unknown): () => unknown {
  return () => {
    const project = new Project();
    try {
      return body(project);
    } finally {
      project.destroy();
    }
  };
}

// Refusals whose core text opens with the C entry name.
const ROUTES: [entry: string, cause: string, run: () => unknown][] = [
  [
    'Project.addClip',
    'unknown track',
    withProject((p) => p.addClip({ trackId: 9999, lengthPpq: 4 })),
  ],
  ['Project.removeClip', 'unknown clip', withProject((p) => p.removeClip(9999))],
];

let checked = 0;

describe('the public entry name is the only one in front of a core refusal', () => {
  afterAll(() => {
    // Every route reached its refusal, against a header scan that found the C entries.
    expect(C_ENTRIES.size).toBeGreaterThan(0);
    expect(checked).toBe(ROUTES.length);
  });

  it.each(ROUTES)('%s (%s)', (entry, _cause, run) => {
    const message = refusalOf(run);
    expect(message.startsWith(`${entry}: `), message).toBe(true);
    const second = SECOND_PREFIX.exec(message)?.[1];
    expect(second !== undefined && C_ENTRIES.has(`sonare_${second}`), message).toBe(false);
    checked += 1;
  });
});
