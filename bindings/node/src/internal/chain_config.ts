import type { MasteringChainConfig } from '../types.js';

/**
 * Serialize a chain config to the document the native chain reader takes. The
 * core refuses the document's content by path; only a config that is not an
 * object is refused here. An absent config is `null`, which native reads as none.
 *
 * @throws TypeError when `config` is given and is not an object
 */
export function chainConfigJson(fnName: string, field: string, config: unknown): string | null {
  if (config === undefined || config === null) {
    return null;
  }
  if (typeof config !== 'object' || Array.isArray(config)) {
    throw new TypeError(`${fnName}: ${field} must be an object`);
  }
  return JSON.stringify({ params: config });
}

/** Read a chain config document the native layer wrote, as text. */
export function chainConfigFromJson(document: string): MasteringChainConfig {
  return chainConfigFromDocument(JSON.parse(document) as { params: Record<string, unknown> });
}

/**
 * Read a parsed chain config document the native layer wrote. Its `params`
 * keys are dotted paths, which are nested here; every value is kept as written.
 */
export function chainConfigFromDocument(document: {
  params: Record<string, unknown>;
}): MasteringChainConfig {
  const { params } = document;
  const root: Record<string, unknown> = {};
  for (const [path, value] of Object.entries(params)) {
    const parts = path.split('.');
    let node = root;
    for (const part of parts.slice(0, -1)) {
      node[part] ??= {};
      node = node[part] as Record<string, unknown>;
    }
    node[parts[parts.length - 1]] = value;
  }
  return root as MasteringChainConfig;
}
