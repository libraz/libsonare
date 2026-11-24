/**
 * Each project closed set the facade spells holds the core's tokens in the
 * core's order.
 *
 * The core's list is read off the refusal a document with an unknown token at
 * that field earns ("<path> must be one of <tokens>"), so the comparison is
 * against the table the serializer actually uses. The facade's token→ordinal map
 * is sorted by ordinal, so a member out of place reads as a drift too.
 */

import { beforeAll, describe, expect, expectTypeOf, it } from 'vitest';
import { automationTargetKindNames, init, Project } from '../dist/index.js';
import { AUTOMATION_CURVE_VALUES } from '../src/codes.js';
import type { ProjectLoopMode, ProjectTrackKind } from '../src/index';
import {
  CHORD_QUALITY_CODES,
  FADE_CURVES,
  KEY_MODES,
  LOOP_MODES,
  MARKER_KINDS,
  OVERLAP_POLICIES,
  SOURCE_KINDS,
  TRACK_KINDS,
  WARP_MODES,
} from '../src/internal/project_tokens.js';

/** A document carrying a value at every enum field the tables cover. */
function richDocument(): Record<string, unknown> {
  const project = new Project();
  try {
    const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
    project.addClip({ trackId, startPpq: 0, lengthPpq: 4 });
    project.setMarker({ ppq: 1, kind: 'text', name: 'cue' });
    project.addAutomationLane(trackId, {
      targetKind: 'trackFaderDb',
      points: [{ ppq: 0, value: -3, curveToNext: 'hold' }],
    });
    project.annotateKeys([{ startPpq: 0, endPpq: 4, tonicPc: 0, mode: 'major' }]);
    project.annotateChords([{ startPpq: 0, endPpq: 4, rootPc: 0, quality: 'major' }]);
    return JSON.parse(project.toJson()) as Record<string, unknown>;
  } finally {
    project.destroy();
  }
}

/** The tokens the core lists when the field at @p path holds an unknown one. */
function coreTokens(
  document: Record<string, unknown>,
  path: readonly (string | number)[],
): string[] {
  const copy = structuredClone(document);
  let at = copy as Record<string | number, unknown>;
  for (const step of path.slice(0, -1)) {
    at = at[step] as Record<string | number, unknown>;
  }
  const last = path[path.length - 1] as string | number;
  expect(at[last], path.join('.')).toBeTypeOf('string');
  at[last] = 'notAToken';
  let message = '';
  try {
    Project.fromJson(JSON.stringify(copy)).destroy();
  } catch (error) {
    message = (error as Error).message;
  }
  const listed = message.match(/must be one of (.+)$/);
  if (!listed) {
    throw new Error(`${path.join('.')}: no token list in ${JSON.stringify(message)}`);
  }
  return listed[1].split(', ');
}

/** The facade's tokens in ordinal order. */
const inOrdinalOrder = (map: Readonly<Record<string, number>>): string[] =>
  Object.entries(map)
    .sort(([, a], [, b]) => a - b)
    .map(([token]) => token);

const TABLES: readonly [string, Readonly<Record<string, number>>, readonly (string | number)[]][] =
  [
    ['warp mode', WARP_MODES, ['clips', 0, 'warpMode']],
    ['fade curve', FADE_CURVES, ['clips', 0, 'fadeIn', 'curve']],
    ['loop mode', LOOP_MODES, ['clips', 0, 'loopMode']],
    ['overlap policy', OVERLAP_POLICIES, ['overlapPolicy']],
    ['marker kind', MARKER_KINDS, ['markers', 0, 'kind']],
    ['track kind', TRACK_KINDS, ['tracks', 0, 'kind']],
    ['source kind', SOURCE_KINDS, ['sources', 0, 'kind']],
    ['chord quality', CHORD_QUALITY_CODES, ['annotation', 'chords', 0, 'quality']],
    ['key mode', KEY_MODES, ['annotation', 'keys', 0, 'mode']],
    [
      'automation curve',
      AUTOMATION_CURVE_VALUES,
      ['tracks', 0, 'automationLanes', 0, 'points', 0, 'curveToNext'],
    ],
  ];

describe('project token tables', () => {
  let document: Record<string, unknown>;
  beforeAll(async () => {
    await init();
    document = richDocument();
  });

  it.each(TABLES)('the %s list is the core table in order', (_name, map, path) => {
    expect(inOrdinalOrder(map)).toEqual(coreTokens(document, path));
  });

  it('reads automation target tokens from the C name table in order', () => {
    expect(automationTargetKindNames()).toEqual(
      coreTokens(document, ['tracks', 0, 'automationLanes', 0, 'targetKind']),
    );
  });

  it('uses string tokens for project loop modes', () => {
    expectTypeOf<ProjectLoopMode>().toEqualTypeOf<'off' | 'loop'>();
    expect(inOrdinalOrder(LOOP_MODES)).toEqual(['off', 'loop']);
  });

  it('uses string tokens for project track kinds', () => {
    expectTypeOf<ProjectTrackKind>().toEqualTypeOf<'audio' | 'midi' | 'aux'>();
    expect(inOrdinalOrder(TRACK_KINDS)).toEqual(['audio', 'midi', 'aux']);
  });
});
