// Shared helper: compile a TypeScript module on the fly and load it as CommonJS,
// matching the transpile-on-load pattern used by the other node:test suites
// (see audio-effects-executor.test.js). Lets us unit-test the pure config-*.ts
// modules without a build step or an electron runtime.
const fs = require('node:fs');
const path = require('node:path');
const Module = require('node:module');
const ts = require('typescript');

const ROOT = path.join(__dirname, '..');

// Let a loaded .ts module require sibling .ts modules (e.g. lease-bridge.ts
// → ./lease-registry). Registering the extension also makes Node's resolver
// consider .ts files for extension-less requires.
if (!Module._extensions['.ts']) {
    Module._extensions['.ts'] = (mod, filename) => {
        const source = fs.readFileSync(filename, 'utf8');
        const compiled = ts.transpileModule(source, {
            compilerOptions: {
                module: ts.ModuleKind.CommonJS,
                target: ts.ScriptTarget.ES2022,
                esModuleInterop: true,
            },
            fileName: filename,
        }).outputText;
        mod._compile(compiled, filename);
    };
}

function loadTs(relPath) {
    const file = path.join(ROOT, relPath);
    const source = fs.readFileSync(file, 'utf8');
    const compiled = ts.transpileModule(source, {
        compilerOptions: {
            module: ts.ModuleKind.CommonJS,
            target: ts.ScriptTarget.ES2022,
            esModuleInterop: true,
        },
        fileName: file,
    }).outputText;
    const mod = new Module(file, module);
    mod.filename = file;
    mod.paths = Module._nodeModulePaths(path.dirname(file));
    mod._compile(compiled, file);
    return mod.exports;
}

module.exports = { loadTs, ROOT };
