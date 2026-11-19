import ts from 'typescript';

/**
 * Lowers standard class decorators in sources a test imports directly. The test
 * transform leaves `@decorator` syntax in place, which Node cannot parse; the
 * published build runs through tsc and is unaffected.
 */
export function lowerDecorators() {
  return {
    name: 'sonare-lower-decorators',
    enforce: 'pre',
    transform(code, id) {
      if (!id.endsWith('.ts') || !code.includes('@entryClass(')) {
        return null;
      }
      const out = ts.transpileModule(code, {
        fileName: id,
        compilerOptions: {
          target: ts.ScriptTarget.ES2022,
          module: ts.ModuleKind.ESNext,
          sourceMap: true,
        },
      });
      return { code: out.outputText, map: out.sourceMapText };
    },
  };
}
