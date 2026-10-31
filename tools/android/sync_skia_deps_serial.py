"""Synchronize Skia DEPS sequentially for memory-constrained Windows hosts."""

from __future__ import annotations

import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: sync_skia_deps_serial.py <skia-source>", file=sys.stderr)
        return 2

    skia = pathlib.Path(sys.argv[1]).resolve()
    script = skia / "tools" / "git-sync-deps"
    deps = skia / "DEPS"
    if not script.is_file() or not deps.is_file():
        print(f"invalid Skia source tree: {skia}", file=sys.stderr)
        return 2

    namespace: dict[str, object] = {
        "__name__": "skia_git_sync_deps",
        "__file__": str(script),
    }
    exec(compile(script.read_bytes(), str(script), "exec"), namespace)

    def serial(function, argument_lists):
        total = len(argument_lists)
        for index, arguments in enumerate(argument_lists, 1):
            arguments = list(arguments)
            googlesource_prefix = (
                "https://chromium.googlesource.com/external/github.com/"
            )
            if arguments[1].startswith(googlesource_prefix):
                arguments[1] = "https://github.com/" + arguments[1][
                    len(googlesource_prefix) :
                ]
            dependency = pathlib.Path(arguments[3])
            for lock_name in ("shallow.lock", "index.lock"):
                stale_lock = dependency / ".git" / lock_name
                if stale_lock.is_file():
                    print(f"[build-skia] removing stale {stale_lock}", flush=True)
                    stale_lock.unlink()
            print(f"[build-skia] dependency {index}/{total}", flush=True)
            function(*arguments)

    namespace["multithread"] = serial
    namespace["git_sync_deps"](str(deps), [], True, True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
