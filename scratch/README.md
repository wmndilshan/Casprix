# scratch/

Designated home for **throwaway, local experiments** — one-off `.cpx` snippets,
hand-compiled C, quick debug binaries, captured logs.

## Rules

- **Everything in this directory is git-ignored** except this `README.md`
  (see the `/scratch/*` + `!/scratch/README.md` rules in the root `.gitignore`).
- Nothing here is built by CMake or the Makefile, and nothing here is a test.
  Real tests live in [`tests/`](../tests/).
- Because the whole directory is ignored, scratch files never show up in
  `git status` and never need individual cleanup — delete them whenever.

## Why it exists

Manual compiler-testing sessions used to drop stray files (`fib`, `my_hello.c.c`,
`debug_stringview_ok.cpx`, `ret42.cpxjit`, `build_log.txt`, …) directly in the
repo root, where they polluted `git status` and risked being committed. Put such
files here instead.

## Convention for ad-hoc compiles

```sh
# from the repo root
./bin/casprix scratch/my_experiment.cpx -o scratch/my_experiment
./scratch/my_experiment
```

If you must work in the repo root, note that `.gitignore` also catches the
common stray names there (`test_*`, `debug_*`, `*_test`, `*_bench`, `*.c.c`,
`*.cpxbc`, `*.cpxjit`, `build_log*`, …) — but `scratch/` is the supported path.
