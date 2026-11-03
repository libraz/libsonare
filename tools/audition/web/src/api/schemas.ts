/* Every shape that crosses from serve.py into the page, defined once.
 *
 * Types are inferred from these schemas and data is parsed where it enters
 * (`client.ts`), so a field the server renames fails at the fetch that brought
 * it rather than as an `undefined` three modules later. Objects are loose:
 * the server carries more than the page reads, and an added field must not
 * break a page that does not read it. Only what the page acts on is typed.
 */

import { z } from 'zod';

/** A string in each language the page speaks; `en` is the fallback. */
export const Phrase = z.union([
  z.string(),
  z.looseObject({ en: z.string().optional(), ja: z.string().optional() }),
]);
export type Phrase = z.infer<typeof Phrase>;

export const ShipState = z.enum(['current', 'stale', 'unverified', 'checking']);

/** Whether a set's unmodified render is the voice that ships today. */
export const Ships = z.looseObject({
  /** Absent where nothing can be said: no baseline render, no registry, or no units. */
  state: z.string().optional(),
  reasons: z.array(z.string()).optional(),
});
export type Ships = z.infer<typeof Ships>;

/** One row of `sets.json`. */
export const SetEntry = z.looseObject({
  id: z.string(),
  title: z.string(),
  takes: z.number(),
  compare: z.boolean(),
  group: z.string().default(''),
  path: z.string().default(''),
  ships: Ships.nullable().optional(),
});
export type SetEntry = z.infer<typeof SetEntry>;
export const SetIndex = z.array(SetEntry);

export const Role = z.enum(['model', 'reference', 'comparison']);
export type Role = z.infer<typeof Role>;

/* The manifest writes an unknown as null as readily as it leaves it out, so optional
 * fields below take both. */

/** A version a take can hold: a library render or a recording it is set against. */
export const Source = z.looseObject({
  label: z.string().nullish(),
  role: z.string().nullish(),
  scope: z.string().nullish(),
  block: z.string().nullish(),
  detail: z.string().nullish(),
  title: Phrase.nullish(),
  desc: Phrase.nullish(),
});
export type Source = z.infer<typeof Source>;

export const ComparisonStatus = z.enum(['matched', 'context_only', 'unverified', 'unavailable']);

/** One of the manifest's comparisons: a model side set against an oracle. */
export const Comparison = z.looseObject({
  id: z.string(),
  scope: z.string().nullish(),
  purpose: z.string().nullish(),
  model_sources: z.array(z.string()).default([]),
  oracle_sources: z.array(z.string()).default([]),
  status: z.string().default('unavailable'),
  may_sign_off: z.boolean().nullish(),
  reasons: z.array(z.string()).default([]),
});
export type Comparison = z.infer<typeof Comparison>;

/** A scheduled note of a take; strikes closer than `FUSED_S` are one event. */
export const TakeNote = z.looseObject({
  start: z.number(),
  note: z.number(),
  velocity: z.number(),
});
export type TakeNote = z.infer<typeof TakeNote>;

export const Item = z.looseObject({
  id: z.string(),
  label: z.string().nullish(),
  sub: z.string().nullish(),
  group: z.string().nullish(),
  tracks: z.record(z.string(), z.string()).default({}),
  meta: z.looseObject({ notes: z.array(TakeNote).optional() }).optional(),
  evidence: z.unknown().optional(),
});
export type Item = z.infer<typeof Item>;

/** What the manifest says the page is a render of. */
export const ManifestVoice = z.looseObject({
  program: z.number().nullish(),
  bank: z.number().nullish(),
  name: z.string().nullish(),
  kit: z.boolean().nullish(),
  rig: z.string().nullish(),
  patch: z.string().nullish(),
  capture: z.string().nullish(),
});

export const Manifest = z.looseObject({
  title: z.string().nullish(),
  group: z.string().nullish(),
  notes: z.string().nullish(),
  sources_note: z.string().nullish(),
  set_generation: z.string().nullable().optional(),
  schema_version: z.number().nullish(),
  voice: ManifestVoice.optional(),
  sources: z.record(z.string(), Source).default({}),
  comparisons: z.array(Comparison).default([]),
  items: z.array(Item).default([]),
  provenance: z.unknown().optional(),
});
export type Manifest = z.infer<typeof Manifest>;

/** Whether a queued voice's page can be played now. */
export const PageState = z.enum(['ready', 'render', 'rendering', 'failed', 'checking']);
export type PageState = z.infer<typeof PageState>;

/** One step of the listening run, as `wizard.build_queue` writes it. */
export const QueueStep = z.looseObject({
  set: z.string(),
  name: z.string(),
  program: z.number(),
  bank: z.number(),
  kit: z.boolean(),
  tier: z.string(),
  group: z.string().nullable(),
  page: PageState,
  ships: z.string(),
  noted: z.number(),
  log: z.string(),
  error: z.string(),
});
export type QueueStep = z.infer<typeof QueueStep>;
export const Queue = z.array(QueueStep);

/** A stored listening note; only what the page reads back is typed. */
export const FeedbackEntry = z.looseObject({
  id: z.string().optional(),
  at: z.string().optional(),
  grade: z.string().optional(),
  tag: z.string().optional(),
  text: z.string().optional(),
  lang: z.string().optional(),
  answers: z.array(z.unknown()).optional(),
  conditions: z.record(z.string(), z.unknown()).optional(),
  schema_version: z.number().optional(),
  evaluation: z.record(z.string(), z.unknown()).nullable().optional(),
  evidence: z.record(z.string(), z.unknown()).nullable().optional(),
});
export type FeedbackEntry = z.infer<typeof FeedbackEntry>;

/** `feedback.json`, and the body of a successful note or undo. */
export const FeedbackLog = z.looseObject({
  entries: z.array(FeedbackEntry).default([]),
  path: z.string().default(''),
  entry_id: z.string().optional(),
});
export type FeedbackLog = z.infer<typeof FeedbackLog>;

/** A note refused because the recording it names is no longer the one served. */
export const StaleRefusal = z.looseObject({
  error: z.literal('stale'),
  reason: z.string(),
  detail: z.string(),
});
export type StaleRefusal = z.infer<typeof StaleRefusal>;

/** Per set: how much has been said about it and the worst of it. */
export const FeedbackIndexRow = z.looseObject({
  n: z.number(),
  worst: z.string(),
  prefer: z.number().default(0),
  last: z.string().default(''),
  off_reference: z.number().default(0),
});
export type FeedbackIndexRow = z.infer<typeof FeedbackIndexRow>;
export const FeedbackIndex = z.record(z.string(), FeedbackIndexRow);
export type FeedbackIndex = z.infer<typeof FeedbackIndex>;

/* ------------------------------------------------------------- outgoing */

export const Hit = z.object({
  n: z.number(),
  start: z.number(),
  into: z.number(),
  notes: z.array(z.object({ note: z.number(), velocity: z.number() })),
});

export const ComparedAgainst = z.object({
  role: z.string(),
  version: z.string(),
  label: z.string(),
  chosen: z.enum(['manual', 'last_selected']),
});
export type ComparedAgainst = z.infer<typeof ComparedAgainst>;

/** What the ear met when a note was written: the fields `heard.py` reads. */
export const Conditions = z.object({
  set: z.string().nullable(),
  comparison_id: z.string().nullable(),
  take: z.string().nullable(),
  take_label: z.string().nullable(),
  version: z.string().nullable(),
  against: z.string().nullable(),
  compared_against: ComparedAgainst.nullable(),
  blind: z.boolean(),
  playhead: z.number(),
  duration: z.number().nullable(),
  hit: Hit.nullable(),
  region: z.tuple([z.number(), z.number()]).nullable(),
  match_loudness: z.boolean(),
  loop: z.boolean(),
});
export type Conditions = z.infer<typeof Conditions>;

export const Evaluation = z.object({
  comparison_id: z.string().nullable(),
  scope: z.string().nullable(),
  judged_source: z.string().nullable(),
  oracle_source: z.string().nullable(),
  comparison_status: z.string().nullable(),
  blind: z.boolean(),
});
export type Evaluation = z.infer<typeof Evaluation>;

export const EvidenceClaim = z.object({
  set_generation: z.string().nullable(),
  take: z.string().nullable(),
  source: z.string().nullable(),
});
export type EvidenceClaim = z.infer<typeof EvidenceClaim>;

export const BlindAnswer = z.object({
  take: z.string(),
  candidates: z.array(z.string()).nullable(),
  picked: z.string().nullable(),
  abstained: z.boolean(),
  comparison_id: z.string().nullable(),
  set_generation: z.string().nullable(),
  answered_at: z.string().nullable(),
});
export type BlindAnswer = z.infer<typeof BlindAnswer>;

/** A memo about the voice rather than the moment: which voice, and what it was judged against. */
export const MemoConditions = z.strictObject({
  set: z.string().nullable(),
  compared_against: ComparedAgainst.nullable(),
});

/** A blind run's tally: the picks per take, and the takes the ear could not separate. */
export const BlindConditions = z.strictObject({
  set: z.string().nullable(),
  comparison_id: z.string().nullable(),
  blind: z.literal(true),
  picks: z.record(z.string(), z.string()),
  unseparated: z.array(z.string()),
});

/** One step of the narrowing as it was answered. */
export const Answer = z.strictObject({ q: z.string(), tag: z.string(), said: z.string() });

/** A note as posted. Strict: the log is permanent, so a stray field is a bug here. */
export const NotePost = z.strictObject({
  schema_version: z.literal(2),
  lang: z.string(),
  grade: z.string(),
  tag: z.string(),
  answers: z.array(Answer),
  text: z.string(),
  conditions: z.union([Conditions.strict(), MemoConditions, BlindConditions]),
  evaluation: Evaluation,
  evidence: EvidenceClaim.nullable(),
  blind_answers: z.array(BlindAnswer).optional(),
});
export type NotePost = z.infer<typeof NotePost>;

export const UndoPost = z.strictObject({ set: z.string(), op: z.literal('undo'), id: z.string() });
export type UndoPost = z.infer<typeof UndoPost>;
