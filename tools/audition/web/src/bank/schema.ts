/* The shapes `bank.json` and a page's provenance carry, refined past the shared loose ones.
 *
 * Absent and null fields normalise to one empty value here, so the readers below do not each
 * decide what a missing list or number means. Objects stay loose: the server carries more than
 * the page reads.
 */

import { z } from 'zod';

const text = z
  .string()
  .nullish()
  .transform((s) => s ?? '');
const count = z
  .number()
  .nullish()
  .transform((n) => n ?? 0);
const flag = z
  .boolean()
  .nullish()
  .transform((b) => b ?? false);
const words = z
  .array(z.string())
  .nullish()
  .transform((a) => a ?? []);

/** A claim about a voice (`structure`, `music`) and where it stands. */
const Claim = z.looseObject({ state: z.string().nullish() });

/** The slot a stand-in answers for, and why. */
export const Approximated = z.looseObject({
  answered_by: text,
  reason: z.string().nullish(),
});

/** What a slot is aimed at, by policy, and how the captured reference stands against it. */
export const Target = z.looseObject({
  branch: z.string().nullish(),
  timbre: z.string().nullish(),
  state: z.string().nullish(),
  reason: z.string().nullish(),
  approximated: Approximated.nullish(),
  answers: z.array(z.string()).nullish(),
});
export type Target = z.infer<typeof Target>;

const Coverage = z.looseObject({
  canonical: count,
  gated: count,
  complete: flag,
  excused: words,
  gaps: words,
});

const Agreement = z.looseObject({
  total: count,
  inside: count,
  outside: z
    .record(z.string(), z.number())
    .nullish()
    .transform((o) => o ?? {}),
  unjudgeable: words,
});

const Axes = z.looseObject({
  structure: Claim.nullish(),
  music: Claim.nullish(),
  timbres: z.number().nullish(),
  profile_rows: z.number().nullish(),
  gate_state: z.string().nullish(),
  coverage: Coverage.nullish(),
  agreement: Agreement.nullish(),
});

/** The version a voice ships at now. */
const ShipNow = z.looseObject({
  unit: text,
  version: z.union([z.string(), z.number()]),
  date: text,
});

/** One row of `bank.json`: a GM program, a GS variation or a kit, and how far it has got. */
export const BankVoiceRow = z.looseObject({
  slug: z.string(),
  name: text,
  program: count,
  bank: count,
  kit: flag,
  group: text,
  engine: z.string().nullish(),
  patch: z.string().nullish(),
  tone_class: z.string().nullish(),
  stage: count,
  next: text,
  capture: z.string().nullish(),
  open_candidates: words,
  target: Target.nullish(),
  ships: ShipNow.nullish(),
  axes: Axes.nullish(),
});
export type BankVoiceRow = z.infer<typeof BankVoiceRow>;

export const BankFile = z.looseObject({ voices: z.array(BankVoiceRow) });
export type BankFile = z.infer<typeof BankFile>;

/** A page's ship record with the detail the set index carries beyond its state. */
export const ShipsView = z.looseObject({
  state: z.string(),
  reasons: words,
  since: z
    .array(
      z.looseObject({
        unit: text,
        version: z.union([z.string(), z.number()]),
        date: text,
        note: text,
      }),
    )
    .nullish()
    .transform((a) => a ?? []),
  // biome-ignore lint/suspicious/noThenProperty: the field is named by the server
  then: z
    .record(z.string(), z.union([z.string(), z.number()]))
    .nullish()
    .transform((o) => o ?? {}),
  rendered: z.string().nullish(),
  units: z.array(z.unknown()).nullish(),
});
export type ShipsView = z.infer<typeof ShipsView>;

/** The reference capture a page was rendered against. */
const Capture = z.looseObject({
  source_class: z.string().nullish(),
  product: z.string().nullish(),
  room: z.unknown().optional(),
  dry: z.boolean().nullish(),
});

/** What a page's manifest says about the voice's reference and aim. */
export const Provenance = z.looseObject({
  want: Target.nullish(),
  state: z.string().nullish(),
  capture: Capture.nullish(),
  declined: z
    .looseObject({
      names: z.string().nullish(),
      carries: z.string().nullish(),
      reason: z.string().nullish(),
    })
    .nullish(),
  approximated: Approximated.nullish(),
  answers: z.array(z.string()).nullish(),
  drum_names: z.record(z.string(), z.string()).nullish(),
});
export type Provenance = z.infer<typeof Provenance>;
