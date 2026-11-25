import { type WasmBindingSource, wasmBindingSources } from './_wasm_binding_sources';

interface FallbackSite {
  id: string;
  line: number;
  literal: string;
}

const READERS =
  'intProperty|uintProperty|int64Property|wordProperty|byteProperty|boolProperty|' +
  'floatProperty|doubleProperty|domainFloatProperty|optionalDomainDoubleProperty|stringProperty|stringPropertyStrict';
const LITERAL = String.raw`(?:0(?:\.0+)?[fFuUlL]*|false|"")`;
const FIELD = String.raw`(?:"([a-zA-Z_]\w*)"|(key))`;
const PROPERTY = new RegExp(
  String.raw`\b(?:${READERS})\s*\(\s*[^,;()]+,\s*${FIELD}\s*,\s*(${LITERAL})\s*(?=[,)])`,
  'g',
);
const OPTIONAL = new RegExp(
  String.raw`\boptional(?:Bool|Number|DomainNumber|DomainValue(?:<[^>]+>)?)\s*\(\s*[^;]*?,\s*"([a-zA-Z_]\w*)"(?:\s*,[^;]+?)?\s*\)\s*\.\s*value_or\s*\(\s*(${LITERAL})\s*\)`,
  'g',
);
const ABSENT_ARGUMENT = new RegExp(
  String.raw`(?:\babsent\(\w+\)|\b\w+\.isUndefined\(\))\s*\?\s*(${LITERAL})\s*:\s*\w+(?:<[^>]+>)?\(\s*\w+,\s*(?:\w+\(\s*)?"([a-zA-Z_]\w*)"`,
  'g',
);

const PRESENT_PROPERTY = new RegExp(
  String.raw`\bhasProperty\s*\(\s*\w+,\s*"([a-zA-Z_]\w*)"\s*\)\s*\?\s*[^?;]+?:\s*(${LITERAL})\s*(?=[,;)])`,
  'g',
);
const LOCAL_OPTIONAL = new RegExp(
  String.raw`\b([a-zA-Z_]\w*)\s*\.\s*value_or\s*\(\s*(${LITERAL})\s*\)`,
  'g',
);

export function zeroFallbackSites(
  sources: WasmBindingSource[] = wasmBindingSources(),
): FallbackSite[] {
  return sources.flatMap(({ file, text }) => {
    const source = text.replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, (comment) =>
      comment.replace(/[^\n]/g, ' '),
    );
    return [
      ...[...source.matchAll(PROPERTY)].map((match) => ({
        index: match.index,
        key: match[1] ?? '<key>',
        literal: match[3],
      })),
      ...[...source.matchAll(OPTIONAL)].map((match) => ({
        index: match.index,
        key: match[1],
        literal: match[2],
      })),
      ...[...source.matchAll(ABSENT_ARGUMENT)].map((match) => ({
        index: match.index,
        key: match[2],
        literal: match[1],
      })),
      ...[...source.matchAll(PRESENT_PROPERTY), ...source.matchAll(LOCAL_OPTIONAL)].map(
        (match) => ({ index: match.index, key: match[1], literal: match[2] }),
      ),
    ]
      .sort((a, b) => a.index - b.index)
      .map(({ index, key, literal }) => ({
        id: `${file}:${key}`,
        line: source.slice(0, index).split('\n').length,
        literal,
      }));
  });
}
