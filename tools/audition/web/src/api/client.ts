/* The one way the page talks to serve.py: every reply is parsed by its schema
 * where it arrives. */

import type { z } from 'zod';

export class HttpError extends Error {
  constructor(
    readonly status: number,
    readonly body: unknown,
  ) {
    super(String(status));
  }
}

async function readBody(res: Response): Promise<unknown> {
  const text = await res.text();
  if (!text) {
    return null;
  }
  try {
    return JSON.parse(text);
  } catch {
    return text;
  }
}

export async function getJSON<S extends z.ZodType>(url: string, schema: S): Promise<z.infer<S>> {
  const res = await fetch(url);
  if (!res.ok) {
    throw new HttpError(res.status, await readBody(res));
  }
  return schema.parse(await res.json());
}

/** POST a body that has already been checked against its own schema; the reply is parsed by `reply`. */
export async function postJSON<S extends z.ZodType>(
  url: string,
  body: unknown,
  reply: S,
): Promise<z.infer<S>> {
  const res = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  if (!res.ok) {
    throw new HttpError(res.status, await readBody(res));
  }
  return reply.parse(await res.json());
}
