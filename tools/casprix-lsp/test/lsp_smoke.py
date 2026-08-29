#!/usr/bin/env python3
"""Manual protocol driver for casprix-lsp.

Feeds a scripted sequence of framed JSON-RPC messages to the server binary and
prints every framed message it emits. Exit code mirrors the assertions.
"""
import json
import subprocess
import sys
import os

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/casprix-lsp"

VALID_CPX = """\
class Point {
    mut x: int
    mut y: int
}

func distance(a: Point, b: Point) -> int {
    let dx = 0
    return dx
}

func main() -> int {
    let p = new Point()
    return distance(p, p)
}
"""

# Exactly one deliberate error: `1 +` with no right operand on the
# `let n = 1 +` line inside broken(). Everything else is valid, so panic-mode
# recovery must still surface Widget / broken / ok as symbols and resolve
# go-to-definition on the valid parts.
ERR_CPX = """\
class Widget {
    mut label: int
}

func broken(w: Widget) -> int {
    let n = 1 +
    return n
}

func ok(w: Widget) -> int {
    return 7
}
"""


def frame(obj):
    body = json.dumps(obj).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n%s" % (len(body), body)


def read_frames(data):
    """Yield decoded JSON objects from a stream of Content-Length frames."""
    i = 0
    while True:
        hdr_end = data.find(b"\r\n\r\n", i)
        if hdr_end < 0:
            return
        header = data[i:hdr_end].decode("latin1")
        clen = None
        for line in header.split("\r\n"):
            if line.lower().startswith("content-length:"):
                clen = int(line.split(":", 1)[1].strip())
        if clen is None:
            return
        start = hdr_end + 4
        body = data[start:start + clen]
        i = start + clen
        yield json.loads(body.decode("utf-8"))


def main():
    uri_valid = "file:///tmp/valid.cpx"
    uri_err = "file:///tmp/err.cpx"

    script = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"processId": None, "rootUri": None, "capabilities": {}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},

        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {"uri": uri_valid, "languageId": "casprix",
                                     "version": 1, "text": VALID_CPX}}},

        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {"uri": uri_err, "languageId": "casprix",
                                     "version": 1, "text": ERR_CPX}}},

        # documentSymbol on the erroring file (panic-mode recovery test)
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/documentSymbol",
         "params": {"textDocument": {"uri": uri_err}}},

        # documentSymbol on the valid file
        {"jsonrpc": "2.0", "id": 3, "method": "textDocument/documentSymbol",
         "params": {"textDocument": {"uri": uri_valid}}},

        # definition: cursor on `Point` in the parameter list of distance()
        #   line 5 (0-based) = "func distance(a: Point, b: Point) -> int {"
        #   char 19 is inside the first `Point` (P=17,o=18,i=19,...)
        {"jsonrpc": "2.0", "id": 4, "method": "textDocument/definition",
         "params": {"textDocument": {"uri": uri_valid},
                    "position": {"line": 5, "character": 19}}},

        # definition: cursor on `distance` in the call inside main()
        #   line 12 (0-based) = "    return distance(p, p)"
        {"jsonrpc": "2.0", "id": 5, "method": "textDocument/definition",
         "params": {"textDocument": {"uri": uri_valid},
                    "position": {"line": 12, "character": 13}}},

        # definition: local variable `dx` used on `return dx` line in distance()
        #   line 7 (0-based) = "    return dx"
        {"jsonrpc": "2.0", "id": 6, "method": "textDocument/definition",
         "params": {"textDocument": {"uri": uri_valid},
                    "position": {"line": 7, "character": 11}}},

        # definition on the erroring file: `Widget` in ok()'s param list
        #   line 9 (0-based) = "func ok(w: Widget) -> int {"
        {"jsonrpc": "2.0", "id": 7, "method": "textDocument/definition",
         "params": {"textDocument": {"uri": uri_err},
                    "position": {"line": 9, "character": 11}}},

        {"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": None},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]

    payload = b"".join(frame(m) for m in script)
    proc = subprocess.run([BIN], input=payload, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=30)

    frames = list(read_frames(proc.stdout))
    print(f"--- server exit code: {proc.returncode} ---")
    if proc.stderr.strip():
        print("--- server stderr (first 400 bytes) ---")
        print(proc.stderr[:400].decode("utf-8", "replace"))
    print(f"--- {len(frames)} framed message(s) received ---\n")
    for f in frames:
        print(json.dumps(f, indent=2))
        print()

    # ── assertions ──────────────────────────────────────────────────────────
    fails = []

    def find_response(rid):
        return next((f for f in frames if f.get("id") == rid and "result" in f), None)

    def find_notifs(method):
        return [f for f in frames if f.get("method") == method]

    init = find_response(1)
    if not init or "capabilities" not in init.get("result", {}):
        fails.append("initialize did not return capabilities")
    else:
        caps = init["result"]["capabilities"]
        for k in ("documentSymbolProvider", "definitionProvider"):
            if not caps.get(k):
                fails.append(f"capability {k} not advertised")

    diags = find_notifs("textDocument/publishDiagnostics")
    dv = next((d for d in diags if d["params"]["uri"] == uri_valid), None)
    de = next((d for d in diags if d["params"]["uri"] == uri_err), None)
    if dv is None:
        fails.append("no publishDiagnostics for the valid file")
    elif dv["params"]["diagnostics"]:
        fails.append(f"valid file has non-empty diagnostics: {dv['params']['diagnostics']}")
    if de is None:
        fails.append("no publishDiagnostics for the erroring file")
    elif not de["params"]["diagnostics"]:
        fails.append("erroring file has NO diagnostics (expected a parse error)")

    sym_err = find_response(2)
    if not sym_err or not isinstance(sym_err.get("result"), list):
        fails.append("documentSymbol (err file) returned no list")
    else:
        names = {s["name"] for s in sym_err["result"]}
        for want in ("Widget", "broken", "ok"):
            if want not in names:
                fails.append(f"documentSymbol (err file) missing '{want}' "
                             f"(panic-mode recovery) — got {sorted(names)}")

    sym_ok = find_response(3)
    if not sym_ok or not isinstance(sym_ok.get("result"), list):
        fails.append("documentSymbol (valid file) returned no list")
    else:
        names = {s["name"] for s in sym_ok["result"]}
        for want in ("Point", "distance", "main"):
            if want not in names:
                fails.append(f"documentSymbol (valid) missing '{want}' — got {sorted(names)}")
        kinds = {s["name"]: s["kind"] for s in sym_ok["result"]}
        if kinds.get("Point") != 5:
            fails.append(f"Point kind should be 5 (Class), got {kinds.get('Point')}")
        if kinds.get("distance") != 12:
            fails.append(f"distance kind should be 12 (Function), got {kinds.get('distance')}")

    def_point = find_response(4)
    if not def_point or not def_point.get("result"):
        fails.append("definition on 'Point' returned null")
    else:
        r = def_point["result"]
        if r["range"]["start"]["line"] != 0:
            fails.append(f"'Point' definition should be line 0, got {r['range']['start']['line']}")

    def_fn = find_response(5)
    if not def_fn or not def_fn.get("result"):
        fails.append("definition on 'distance' call returned null")
    else:
        if def_fn["result"]["range"]["start"]["line"] != 5:
            fails.append(f"'distance' def should be line 5, got "
                         f"{def_fn['result']['range']['start']['line']}")

    def_local = find_response(6)
    if not def_local or not def_local.get("result"):
        fails.append("definition on local 'dx' returned null (AST fallback failed)")
    else:
        if def_local["result"]["range"]["start"]["line"] != 6:
            fails.append(f"local 'dx' def should be line 6, got "
                         f"{def_local['result']['range']['start']['line']}")

    def_err = find_response(7)
    if not def_err or not def_err.get("result"):
        fails.append("definition on 'Widget' in erroring file returned null "
                     "(recovery should keep it usable)")
    else:
        if def_err["result"]["range"]["start"]["line"] != 0:
            fails.append(f"'Widget' def should be line 0, got "
                         f"{def_err['result']['range']['start']['line']}")

    if proc.returncode != 0:
        fails.append(f"server exit code {proc.returncode} (expected 0 after shutdown+exit)")

    print("=" * 60)
    if fails:
        for f in fails:
            print("  [FAIL]", f)
        print(f"\n{len(fails)} assertion(s) failed")
        sys.exit(1)
    print("  ALL ASSERTIONS PASSED")


if __name__ == "__main__":
    main()
