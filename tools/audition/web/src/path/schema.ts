/* The path record a model render carries in the manifest's loose evidence field. */

import { z } from 'zod';

const Part = z.looseObject({
  part: z.number(),
  stages: z.array(z.string()).default([]),
  enabled: z.array(z.boolean()).default([]),
  unit: z.number().nullable().optional(),
  send_tap: z.string().nullable().optional(),
});
export type Part = z.infer<typeof Part>;

const Unit = z.looseObject({
  unit: z.number(),
  type: z.union([z.string(), z.number()]),
  realization: z.string(),
  stages: z.array(z.string()).default([]),
  enabled: z.array(z.boolean()).default([]),
});
export type Unit = z.infer<typeof Unit>;

export const TopologyEvent = z.looseObject({
  kind: z.literal('topology'),
  frame: z.number(),
  parts: z.array(Part).default([]),
  units: z.array(Unit).default([]),
});
export type TopologyEvent = z.infer<typeof TopologyEvent>;

export const ParamEvent = z.looseObject({
  kind: z.literal('param'),
  frame: z.number(),
  unit: z.number(),
  slot: z.number(),
  value: z.number(),
});
export type ParamEvent = z.infer<typeof ParamEvent>;

/** Any other event kind a newer library may write; carried, never drawn. */
const OtherEvent = z.looseObject({
  kind: z.string().refine((k) => k !== 'topology' && k !== 'param'),
  frame: z.number(),
});

export const PathEvent = z.union([TopologyEvent, ParamEvent, OtherEvent]);
export type PathEvent = z.infer<typeof PathEvent>;

export const PathRecord = z.looseObject({
  events: z.array(PathEvent).default([]),
  reason: z.string().nullable().optional(),
});
export type PathRecord = z.infer<typeof PathRecord>;

/** What one version carries about how it was rendered. */
export const Evidence = z.looseObject({
  status: z.string().nullable().optional(),
  reason: z.string().nullable().optional(),
  channel: z.number().nullable().optional(),
  sends: z.array(z.number()).nullable().optional(),
  complete: z.boolean().nullable().optional(),
  path: PathRecord.nullable().optional(),
});
export type Evidence = z.infer<typeof Evidence>;
