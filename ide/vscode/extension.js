/**
 * extension.js — Casprix Language Extension for Visual Studio Code
 *
 * Features:
 *  - LSP:              live diagnostics + document symbols + go-to-definition
 *                      via the `casprix-lsp` language server (preferred).
 *  - Fallback:         `casprix --check-only` diagnostics when the LSP binary
 *                      is unavailable or `casprix.enableLsp` is false.
 *  - Auto-complete:     keywords, types, built-ins, snippets
 *  - Hover info:        keyword documentation on hover
 */

const vscode = require('vscode');
const cp = require('child_process');
const path = require('path');
const fs = require('fs');
const os = require('os');

let lsClient = null; // vscode-languageclient LanguageClient, when running

// ============================================================================
// Constants
// ============================================================================

const LANGUAGE_ID = 'casprix';

/** Keywords accepted by the lexer. Keep this aligned with src/compiler/frontend/lexer.h. */
const KEYWORDS = [
    'if', 'else', 'elif', 'for', 'while', 'return', 'break', 'continue',
    'match', 'in', 'async', 'await', 'spawn',
    'try', 'throw', 'catch', 'finally', 'import',
    'func', 'class', 'struct', 'enum', 'union', 'trait', 'impl',
    'new',
    'let', 'const', 'mut', 'static', 'public', 'private',
    'protected', 'abstract', 'extern', 'extends', 'implements',
    'move', 'copy', 'where', 'unsafe',
    'true', 'false', 'this', 'super',
];

const BUILTIN_TYPES = [
    'i8', 'i16', 'i32', 'i64', 'i128',
    'u8', 'u16', 'u32', 'u64', 'u128',
    'f16', 'f32', 'f64', 'bf16',
    'int', 'float',
    'string', 'strbuf', 'bool', 'void', 'char',
    'array', 'slice', 'ptr', 'rawptr', 'ref',
    'tensor', 'dyn', 'lambda',
    'vec2', 'vec3', 'vec4', 'vec8', 'vec16',
    'mat2', 'mat3', 'mat4',
];

const BUILTIN_FUNCTIONS = [
    'print',
    'len', 'sizeof', 'typeof',
    'assert', 'panic',
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
    'await': '**`await`** — Planned async syntax. The current v1 parser does not fully accept await expressions yet.',
    'spawn': '**`spawn`** — Planned concurrency syntax. The current v1 parser does not fully accept spawn statements yet.',
    'unsafe': '**`unsafe`** — Opts out of memory safety checks in this scope.',
    'move': '**`move`** — Transfers ownership of a value.',
    'copy': '**`copy`** — Closure capture mode keyword.',
    'extern': '**`extern`** — Declares an external (C FFI) symbol.',
    'print': '**`print()`** — Prints a string to stdout.',
    'len': '**`len()`** — Returns the length of an array, slice, or string.',
    'assert': '**`assert()`** — Panics if the condition is false.',
    'typeof': '**`typeof()`** — Returns the type of an expression as a string.',
    'dyn': '**`dyn`** — Marks a type for dynamic dispatch (trait objects).\n```cpx\nlet x: dyn[Printable] = ...\n```',
    'lambda': '**`lambda`** — Explicit function/closure type annotation.',
};

const UNSUPPORTED_OR_PLANNED = [
    {
        regex: /\bvar\s+[A-Za-z_][A-Za-z0-9_]*/g,
        message: '`var` is not part of Casprix v1. Use `let`, `mut`, `const`, or `name := value`.',
        severity: vscode.DiagnosticSeverity.Error,
    },
    {
        regex: /\basync\s+func\b/g,
        message: '`async func` is planned, but the current parser does not accept it yet.',
        severity: vscode.DiagnosticSeverity.Warning,
    },
    {
        regex: /\bawait\b/g,
        message: '`await` is tokenized but not fully accepted by the current parser.',
        severity: vscode.DiagnosticSeverity.Warning,
    },
    {
        regex: /\bspawn\b/g,
        message: '`spawn` is planned, but the current parser does not accept it yet.',
        severity: vscode.DiagnosticSeverity.Warning,
    },
    {
        regex: /\bimport\s+"[^"]+"\s+as\s+[A-Za-z_][A-Za-z0-9_]*/g,
        message: 'Import aliases are not accepted by the current parser. Use `import "module";`.',
        severity: vscode.DiagnosticSeverity.Warning,
    },
    {
        regex: /\?\s*;/g,
        message: '`?` try-propagation is tokenized but not parsed yet.',
        severity: vscode.DiagnosticSeverity.Warning,
    },
];

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

function isCommentLine(lineText) {
    return /^\s*(\/\/|#)/.test(lineText);
}

function collectLocalDiagnostics(document) {
    const diagnostics = [];
    for (let lineNo = 0; lineNo < document.lineCount; lineNo++) {
        const text = document.lineAt(lineNo).text;
        if (isCommentLine(text)) continue;

        for (const rule of UNSUPPORTED_OR_PLANNED) {
            rule.regex.lastIndex = 0;
            let match;
            while ((match = rule.regex.exec(text)) !== null) {
                const start = match.index;
                const end = Math.max(start + 1, start + match[0].length);
                const diag = new vscode.Diagnostic(
                    new vscode.Range(lineNo, start, lineNo, end),
                    match[0].includes('async') || match[0].includes('await') || match[0].includes('spawn')
                        ? rule.message
                        : rule.message,
                    rule.severity
                );
                diag.source = 'casprix-v1';
                diagnostics.push(diag);
            }
        }
    }
    return diagnostics;
}

/**
 * Run `casprix --check-only <file>` and populate the diagnostics collection.
 */
function checkDocument(document, diagnosticCollection) {
    if (document.languageId !== LANGUAGE_ID) return;

    const localDiags = collectLocalDiagnostics(document);

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
            const diags = [...localDiags];

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
        // Compiler not found — still keep lightweight parser-surface diagnostics.
        diagnosticCollection.set(document.uri, localDiags);
    }
}

// ============================================================================
// Completion Provider
// ============================================================================

/** Convert a keyword string into a CompletionItem */
function makeKeywordItem(kw) {
    const item = new vscode.CompletionItem(kw, vscode.CompletionItemKind.Keyword);
    item.detail = 'keyword';
    if (HOVER_DOCS[kw]) item.documentation = new vscode.MarkdownString(HOVER_DOCS[kw]);
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
        if (/^\s*(\/\/|#)/.test(linePrefix)) return undefined;
        if (/["']/.test(linePrefix) && linePrefix.split(/["']/).length % 2 === 0) return undefined;

        const items = [
            ...KEYWORDS.map(makeKeywordItem),
            ...BUILTIN_TYPES.map(makeTypeItem),
            ...BUILTIN_FUNCTIONS.map(makeFuncItem),
        ];

        if (/\bimport\s+$/.test(linePrefix)) {
            const item = new vscode.CompletionItem('import module path', vscode.CompletionItemKind.Module);
            item.insertText = new vscode.SnippetString('"${1:lib/module}";');
            item.detail = 'canonical import syntax';
            item.documentation = 'The current parser accepts string module imports: `import "std/io";`';
            items.unshift(item);
        }

        if (/:\s*&?$/.test(linePrefix) || /->\s*&?$/.test(linePrefix)) {
            items.unshift(...BUILTIN_TYPES.map(makeTypeItem));
        }

        if (/\bimpl\s+[A-Za-z_][A-Za-z0-9_]*\s+$/.test(linePrefix)) {
            const item = new vscode.CompletionItem('for', vscode.CompletionItemKind.Keyword);
            item.insertText = 'for ';
            item.detail = 'trait implementation syntax';
            items.unshift(item);
        }

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
// Language Server client (casprix-lsp)
// ============================================================================

/** Resolve the casprix-lsp executable: explicit setting → workspace build/ →
 *  PATH. Returns an absolute path, or null if nothing usable is found. */
function resolveLspBinary(config) {
    const exe = os.platform() === 'win32' ? '.exe' : '';
    const configured = config.get('lspPath', 'casprix-lsp');

    if (path.isAbsolute(configured) && fs.existsSync(configured)) return configured;

    const folders = vscode.workspace.workspaceFolders || [];
    for (const f of folders) {
        for (const rel of ['build/casprix-lsp' + exe,
                           'build/bin/casprix-lsp' + exe,
                           'casprix-lsp' + exe]) {
            const cand = path.join(f.uri.fsPath, rel);
            if (fs.existsSync(cand)) return cand;
        }
    }

    // Search PATH for a bare command name, plus a few common install dirs the
    // GUI process may not have on PATH.
    const name = configured + exe;
    const home = os.homedir();
    const searchDirs = [
        ...(process.env.PATH || '').split(path.delimiter),
        path.join(home, '.local', 'bin'),
        path.join(home, 'bin'),
        '/usr/local/bin',
        '/usr/bin',
        '/opt/homebrew/bin',
    ];
    for (const d of searchDirs) {
        if (!d) continue;
        const cand = path.join(d, name);
        try { if (fs.existsSync(cand)) return cand; } catch (_) { /* ignore */ }
    }
    return null;
}

/** Start the LSP client. Returns true on success. */
async function startLanguageServer(context, config) {
    let lc;
    try {
        lc = require('vscode-languageclient/node');
    } catch (_) {
        // Dependency not bundled — silently fall back to --check-only.
        return false;
    }

    const bin = resolveLspBinary(config);
    if (!bin) {
        vscode.window.setStatusBarMessage(
            '$(warning) casprix-lsp not found — using compiler --check-only', 6000);
        return false;
    }

    const serverOptions = {
        run:   { command: bin, args: [], transport: lc.TransportKind.stdio },
        debug: { command: bin, args: [], transport: lc.TransportKind.stdio },
    };
    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: LANGUAGE_ID }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.cpx'),
        },
        outputChannelName: 'Casprix Language Server',
    };

    lsClient = new lc.LanguageClient(
        'casprixLsp', 'Casprix Language Server', serverOptions, clientOptions);

    try {
        await lsClient.start();
        context.subscriptions.push({ dispose: () => lsClient && lsClient.stop() });
        vscode.window.setStatusBarMessage('$(check) Casprix LSP active', 4000);
        console.log('[Casprix] LSP started:', bin);
        return true;
    } catch (err) {
        console.error('[Casprix] LSP failed to start:', err);
        vscode.window.setStatusBarMessage(
            '$(error) casprix-lsp failed to start — using --check-only', 6000);
        lsClient = null;
        return false;
    }
}

// ============================================================================
// Activate / Deactivate
// ============================================================================

/** @param {vscode.ExtensionContext} context */
async function activate(context) {
    const diagnosticCollection = vscode.languages.createDiagnosticCollection('casprix');
    context.subscriptions.push(diagnosticCollection);

    const config = vscode.workspace.getConfiguration('casprix');

    // --- Language Server (preferred). Diagnostics come from the server;
    //     the --check-only path below is only used as a fallback. ---
    let lspRunning = false;
    if (config.get('enableLsp', true)) {
        lspRunning = await startLanguageServer(context, config);
    }
    const useFallbackDiagnostics = !lspRunning;

    // Restart command
    context.subscriptions.push(
        vscode.commands.registerCommand('casprix.restartLsp', async () => {
            if (lsClient) { try { await lsClient.stop(); } catch (_) {} lsClient = null; }
            const ok = await startLanguageServer(
                context, vscode.workspace.getConfiguration('casprix'));
            vscode.window.showInformationMessage(
                ok ? 'Casprix LSP restarted.' : 'Casprix LSP could not start.');
        })
    );

    // --- On-save diagnostics (fallback only; the LSP pushes live) ---
    context.subscriptions.push(
        vscode.workspace.onDidSaveTextDocument(doc => {
            if (useFallbackDiagnostics && config.get('checkOnSave', true)) {
                checkDocument(doc, diagnosticCollection);
            }
        })
    );

    // --- On-open diagnostics (fallback only) ---
    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument(doc => {
            if (useFallbackDiagnostics && doc.languageId === LANGUAGE_ID) {
                checkDocument(doc, diagnosticCollection);
            }
        })
    );

    // --- On-type diagnostics (fallback only, debounced) ---
    let debounceTimer = null;
    context.subscriptions.push(
        vscode.workspace.onDidChangeTextDocument(event => {
            if (!useFallbackDiagnostics) return;
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

    // Run check on all already-open .cpx documents (fallback only)
    vscode.workspace.textDocuments.forEach(doc => {
        if (useFallbackDiagnostics && doc.languageId === LANGUAGE_ID) {
            checkDocument(doc, diagnosticCollection);
        }
    });

    console.log('[Casprix] Extension activated');
}

function deactivate() {
    if (lsClient) {
        const p = lsClient.stop();
        lsClient = null;
        return p;
    }
    return undefined;
}

module.exports = { activate, deactivate };
