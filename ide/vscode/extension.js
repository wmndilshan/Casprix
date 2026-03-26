/**
 * extension.js — Casprix Language Extension for Visual Studio Code
 *
 * Features:
 *  - Error diagnostics: runs `casprix --check-only` on save / on type
 *  - Auto-complete:     keywords, types, built-ins, snippets
 *  - Hover info:        keyword documentation on hover
 */

const vscode = require('vscode');
const cp = require('child_process');
const path = require('path');
const fs = require('fs');
const os = require('os');

// ============================================================================
// Constants
// ============================================================================

const LANGUAGE_ID = 'casprix';

/** All keywords for completion */
const KEYWORDS = [
    'if', 'else', 'elif', 'for', 'while', 'return', 'break', 'continue',
    'match', 'each', 'in', 'to', 'async', 'await', 'spawn', 'select',
    'try', 'throw', 'catch', 'finally', 'import', 'from', 'as', 'yield',
    'func', 'class', 'struct', 'enum', 'union', 'trait', 'interface', 'impl',
    'new', 'type', 'module', 'package',
    'let', 'const', 'mut', 'field', 'static', 'pub', 'public', 'private',
    'protected', 'abstract', 'extern', 'extends', 'implements', 'uses',
    'move', 'copy', 'where', 'unsafe', 'override', 'virtual', 'inline', 'comptime',
    'true', 'false', 'null', 'nil', 'this', 'self', 'super',
    'or', 'and', 'not', 'is',
];

const BUILTIN_TYPES = [
    'i8', 'i16', 'i32', 'i64', 'i128',
    'u8', 'u16', 'u32', 'u64', 'u128',
    'f16', 'f32', 'f64', 'bf16',
    'int', 'uint', 'float', 'double',
    'string', 'strbuf', 'bool', 'void', 'char', 'byte',
    'array', 'slice', 'ptr', 'rawptr', 'ref',
    'tensor', 'vec2', 'vec3', 'vec4', 'vec8', 'vec16',
    'mat2', 'mat3', 'mat4', 'channel', 'Any',
];

const BUILTIN_FUNCTIONS = [
    'print', 'println', 'printf', 'sprintf',
    'len', 'sizeof', 'typeof',
    'assert', 'panic', 'exit',
    'alloc', 'free', 'copy', 'move', 'drop', 'clone', 'cast', 'transmute',
];

/** Simple hover documentation for key concepts */
const HOVER_DOCS = {
    'let': '**`let`** — Declares an immutable binding.\n```cpx\nlet x: int = 42\n```',
    'const': '**`const`** — Compile-time constant value.',
    'mut': '**`mut`** — Marks a binding or parameter as mutable.',
    'func': '**`func`** — Declares a function.\n```cpx\nfunc add(a: int, b: int) -> int {\n    return a + b\n}\n```',
    'class': '**`class`** — Declares a class with fields and methods.',
    'struct': '**`struct`** — Declares a product type (C-like struct).',
    'trait': '**`trait`** — Declares an interface/trait for polymorphism.',
    'impl': '**`impl`** — Implements a trait for a type.',
    'enum': '**`enum`** — Declares an enumeration.',
    'match': '**`match`** — Pattern-matching expression.\n```cpx\nmatch x {\n    0 => "zero",\n    _ => "other"\n}\n```',
    'async': '**`async`** — Marks a function as asynchronous.',
    'await': '**`await`** — Awaits an async value/future.',
    'spawn': '**`spawn`** — Spawns a concurrent task.',
    'unsafe': '**`unsafe`** — Opts out of memory safety checks in this scope.',
    'move': '**`move`** — Transfers ownership of a value.',
    'copy': '**`copy`** — Creates a bitwise copy of a value.',
    'extern': '**`extern`** — Declares an external (C FFI) symbol.',
    'print': '**`print()`** — Prints a string to stdout.',
    'println': '**`println()`** — Prints a string to stdout with newline.',
    'len': '**`len()`** — Returns the length of an array, slice, or string.',
    'assert': '**`assert()`** — Panics if the condition is false.',
    'typeof': '**`typeof()`** — Returns the type of an expression as a string.',
    'sizeof': '**`sizeof()`** — Returns the byte size of a type.',
};

// ============================================================================
// Diagnostic Engine
// ============================================================================

/**
 * Parse one line of casprix compiler error output.
 * Expected formats:
 *   file.cpx:line:col: error: message
 *   file.cpx:line:col: warning: message
 *   file.cpx:line: error: message   (no column)
 */
function parseDiagnosticLine(line, filePath) {
    // Match: path:line:col: severity: message
    const reFull = /^([^:]+):(\d+):(\d+):\s*(error|warning|note|hint):\s*(.+)$/;
    // Match: path:line: severity: message
    const reLine = /^([^:]+):(\d+):\s*(error|warning|note|hint):\s*(.+)$/;

    let m = reFull.exec(line);
    if (m) {
        const lineNo = parseInt(m[2], 10) - 1;
        const colNo = parseInt(m[3], 10) - 1;
        const severity = m[4];
        const message = m[5];
        return { lineNo, colNo, endCol: colNo + 1, severity, message };
    }

    m = reLine.exec(line);
    if (m) {
        const lineNo = parseInt(m[2], 10) - 1;
        const severity = m[3];
        const message = m[4];
        return { lineNo, colNo: 0, endCol: 1, severity, message };
    }

    return null;
}

function severityToDiag(severity) {
    switch (severity) {
        case 'error': return vscode.DiagnosticSeverity.Error;
        case 'warning': return vscode.DiagnosticSeverity.Warning;
        case 'note': return vscode.DiagnosticSeverity.Information;
        case 'hint': return vscode.DiagnosticSeverity.Hint;
        default: return vscode.DiagnosticSeverity.Error;
    }
}

/**
 * Run `casprix --check-only <file>` and populate the diagnostics collection.
 */
function checkDocument(document, diagnosticCollection) {
    if (document.languageId !== LANGUAGE_ID) return;

    const config = vscode.workspace.getConfiguration('casprix');
    let compilerPath = config.get('compilerPath', 'casprix');

    // Try to resolve a workspace-relative binary if the setting is a relative path
    if (!path.isAbsolute(compilerPath)) {
        const wsFolder = vscode.workspace.getWorkspaceFolder(document.uri);
        if (wsFolder) {
            const candidate = path.join(wsFolder.uri.fsPath, 'build',
                compilerPath + (os.platform() === 'win32' ? '.exe' : ''));
            if (fs.existsSync(candidate)) compilerPath = candidate;
        }
    }

    // Write to a temp file if the document is unsaved
    let filePath = document.uri.fsPath;
    let tempFile = null;
    if (document.isDirty) {
        const ext = path.extname(filePath) || '.cpx';
        tempFile = path.join(os.tmpdir(), `casprix_check_${Date.now()}${ext}`);
        fs.writeFileSync(tempFile, document.getText(), 'utf8');
        filePath = tempFile;
    }

    const args = ['--check-only', '--diag-format=minimal', filePath];

    try {
        cp.execFile(compilerPath, args, { timeout: 10000 }, (err, stdout, stderr) => {
            if (tempFile) {
                try { fs.unlinkSync(tempFile); } catch (_) { }
            }

            const output = (stderr || '') + (stdout || '');
            const diags = [];

            for (const line of output.split('\n')) {
                const parsed = parseDiagnosticLine(line.trim(), document.uri.fsPath);
                if (!parsed) continue;

                const startLine = Math.max(0, parsed.lineNo);
                const startChar = Math.max(0, parsed.colNo);
                const endLine = startLine;
                const endChar = Math.min(
                    parsed.endCol,
                    document.lineAt(Math.min(startLine, document.lineCount - 1)).text.length
                );

                const range = new vscode.Range(startLine, startChar, endLine, endChar);
                const diag = new vscode.Diagnostic(range, parsed.message,
                    severityToDiag(parsed.severity));
                diag.source = 'casprix';
                diags.push(diag);
            }

            diagnosticCollection.set(document.uri, diags);
        });
    } catch (execErr) {
        // Compiler not found — clear diagnostics silently
        diagnosticCollection.set(document.uri, []);
    }
}

// ============================================================================
// Completion Provider
// ============================================================================

/** Convert a keyword string into a CompletionItem */
function makeKeywordItem(kw) {
    const item = new vscode.CompletionItem(kw, vscode.CompletionItemKind.Keyword);
    item.detail = 'keyword';
    return item;
}

function makeTypeItem(t) {
    const item = new vscode.CompletionItem(t, vscode.CompletionItemKind.TypeParameter);
    item.detail = 'built-in type';
    return item;
}

function makeFuncItem(fn) {
    const item = new vscode.CompletionItem(fn, vscode.CompletionItemKind.Function);
    item.detail = 'built-in function';
    item.insertText = new vscode.SnippetString(`${fn}($0)`);
    return item;
}

const completionProvider = {
    provideCompletionItems(document, position) {
        const linePrefix = document.lineAt(position).text.slice(0, position.character);

        // Don't complete inside comments or strings
        if (/\/\//.test(linePrefix)) return undefined;
        if (/["']/.test(linePrefix) && linePrefix.split(/["']/).length % 2 === 0) return undefined;

        const items = [
            ...KEYWORDS.map(makeKeywordItem),
            ...BUILTIN_TYPES.map(makeTypeItem),
            ...BUILTIN_FUNCTIONS.map(makeFuncItem),
        ];
        return items;
    },
};

// ============================================================================
// Hover Provider
// ============================================================================

const hoverProvider = {
    provideHover(document, position) {
        const wordRange = document.getWordRangeAtPosition(position, /[a-zA-Z_][a-zA-Z0-9_]*/);
        if (!wordRange) return null;
        const word = document.getText(wordRange);
        const doc = HOVER_DOCS[word];
        if (!doc) return null;
        const md = new vscode.MarkdownString(doc);
        md.isTrusted = true;
        return new vscode.Hover(md, wordRange);
    },
};

// ============================================================================
// Activate / Deactivate
// ============================================================================

/** @param {vscode.ExtensionContext} context */
function activate(context) {
    const diagnosticCollection = vscode.languages.createDiagnosticCollection('casprix');
    context.subscriptions.push(diagnosticCollection);

    const config = vscode.workspace.getConfiguration('casprix');

    // --- On-save diagnostics ---
    context.subscriptions.push(
        vscode.workspace.onDidSaveTextDocument(doc => {
            if (config.get('checkOnSave', true)) {
                checkDocument(doc, diagnosticCollection);
            }
        })
    );

    // --- On-open diagnostics ---
    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument(doc => {
            if (doc.languageId === LANGUAGE_ID) {
                checkDocument(doc, diagnosticCollection);
            }
        })
    );

    // --- On-type diagnostics (debounced) ---
    let debounceTimer = null;
    context.subscriptions.push(
        vscode.workspace.onDidChangeTextDocument(event => {
            if (!config.get('checkOnType', false)) return;
            if (event.document.languageId !== LANGUAGE_ID) return;
            clearTimeout(debounceTimer);
            const delay = config.get('checkDelay', 800);
            debounceTimer = setTimeout(() => {
                checkDocument(event.document, diagnosticCollection);
            }, delay);
        })
    );

    // --- Clear diagnostics when file is closed ---
    context.subscriptions.push(
        vscode.workspace.onDidCloseTextDocument(doc => {
            diagnosticCollection.delete(doc.uri);
        })
    );

    // --- Completion provider ---
    context.subscriptions.push(
        vscode.languages.registerCompletionItemProvider(
            { language: LANGUAGE_ID, scheme: 'file' },
            completionProvider,
            '.', ':', '<'          // trigger characters
        )
    );

    // --- Hover provider ---
    context.subscriptions.push(
        vscode.languages.registerHoverProvider(
            { language: LANGUAGE_ID, scheme: 'file' },
            hoverProvider
        )
    );

    // Run check on all already-open .cpx documents
    vscode.workspace.textDocuments.forEach(doc => {
        if (doc.languageId === LANGUAGE_ID) {
            checkDocument(doc, diagnosticCollection);
        }
    });

    console.log('[Casprix] Extension activated');
}

function deactivate() { }

module.exports = { activate, deactivate };
