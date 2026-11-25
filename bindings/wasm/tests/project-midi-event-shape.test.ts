/**
 * Pins the wording that refuses a `Project.setMidiEvents` element of the wrong shape. Both JS
 * surfaces accept `sysex` on an event, so the refusal lists it among the event's fields.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, midiNoteOn, Project } from '../dist/index.js';

beforeAll(async () => {
  await init();
});

describe('Project.setMidiEvents element shape', () => {
  // RED WHEN: the refusal omits `sysex?` from the fields an event may carry.
  it('refuses a non-object event naming every field an event may carry', () => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      expect(() => project.setMidiEvents(clipId, [[0, 546323556, 0] as never])).toThrow(
        new TypeError(
          'Project.setMidiEvents: events[0] must be a { ppq, word0, word1?, sysex? } object',
        ),
      );
      // Control: the same event as an object is accepted.
      const event = midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 });
      project.setMidiEvents(clipId, [event]);
      expect(project.midiEvents(clipId)).toHaveLength(1);
    } finally {
      project.destroy();
    }
  });
});
