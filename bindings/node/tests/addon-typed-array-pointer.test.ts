/**
 * A zero-length typed array is a given argument. Its view carries a null element pointer, which a
 * C entry reads as an omitted argument, so every typed-array pointer the addon reads goes through
 * the one accessor that never returns null, as does an optional pointer into a copy of one.
 */

import { describe, expect, it } from 'vitest';
import { pitchCorrectTimevarying, splitNote } from '../src/index.js';
import { addonSources } from './_addon_sources.js';

const HELPER_FILE = 'sonare_wrap_options.h';
const HELPER_READ = /T\* data = array\.Data\(\);/;

describe('typed-array element pointers', () => {
  it('reads every typed-array pointer through ElementData', () => {
    const sources = addonSources();
    expect(sources.length).toBeGreaterThan(20);
    const helper = sources.find((source) => source.file === HELPER_FILE);
    expect(helper?.text).toMatch(HELPER_READ);
    let uses = 0;
    const offenders: string[] = [];
    for (const source of sources) {
      const text = source.text.replace(/\/\/.*$/gm, '').replace(HELPER_READ, '');
      uses += text.match(/sonare_node::ElementData\(/g)?.length ?? 0;
      for (const match of text.matchAll(/[\w\])>]+(?:\.|->)Data\(\)/g)) {
        const line = text.slice(
          text.lastIndexOf('\n', match.index) + 1,
          text.indexOf('\n', match.index),
        );
        if (!/External</.test(line)) {
          offenders.push(`${source.file}: ${line.trim()}`);
        }
      }
      // An optional pointer taken from a copy carries no count, so an empty copy must not read as null.
      for (const match of text.matchAll(/\w+_ptr = [\w>.-]+\.data\(\);/g)) {
        offenders.push(`${source.file}: ${match[0]}`);
      }
    }
    // A floor, so a scan that stops seeing the accessor cannot pass as clean.
    expect(uses).toBeGreaterThan(300);
    expect(offenders).toEqual([]);
  });

  it('answers an empty f0Hz with the entry rule, as a range error', () => {
    let caught: unknown;
    try {
      pitchCorrectTimevarying({
        samples: new Float32Array(4096),
        sampleRate: 22050,
        f0Hz: new Float32Array(),
        hopLength: 256,
      });
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(RangeError);
  });

  it('reads an empty voicedProb as given, so the frame count is refused rather than both omitted', () => {
    const empty = new Float32Array();
    const note = { onsetSample: 0, offsetSample: 5000, frameStart: 0, frameEnd: 10, medianHz: 220 };
    let caught: unknown;
    try {
      splitNote({
        samples: new Float32Array(11025),
        sampleRate: 22050,
        f0Hz: empty,
        voicedProb: empty,
        frameRate: 40,
        notes: [note],
        index: 0,
        frame: 0,
      });
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(RangeError);
  });
});
