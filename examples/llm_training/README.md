# Casprix LLM training — TinyStories-style example

Casprix sources plus a small **C FFI shim** (`llm_backend.c`) for heavy numeric work. Layout:

```
examples/llm_training/
├── tokenizer.cpx      # BPE-oriented tokenizer logic
├── transformer.cpx    # GPT-style transformer + optimizer hooks
├── dataloader.cpx     # Binary shard loading
├── train_llm.cpx      # Orchestrates training
├── llm_backend.c      # C implementations called via extern
└── (generated)        # Shared library / DLL built manually — see below
```

## Syntax reference (illustrative)

The training `.cpx` files aim to follow modern surface rules used elsewhere in the repo. Prefer `let` / `mut`, `func`, `extern func` for C entry points, and `import "…"` for modules — see [CASPRIX_SYNTAX_EBNF.md](../../docs/CASPRIX_SYNTAX_EBNF.md) for the authoritative grammar.

## Training phases (conceptual)

1. **Tokenizer** — build or load a BPE table; output paths like `data/tok32k.bin` depending on the script.
2. **Dataset** — encode text into binary shards (`data/train_shard_*.bin`, etc.).
3. **Model** — construct a small transformer (hyperparameters are in the `.cpx` sources).
4. **Loop** — AdamW, LR schedule, checkpoints (filenames depend on `train_llm.cpx`).

## Build the C backend

Linux / macOS (shared object):

```bash
cd /path/to/casprix
gcc -O2 -mavx2 -mfma -shared -fPIC \
    -o examples/llm_training/llm_backend.so \
    examples/llm_training/llm_backend.c \
    -lm -lpthread
```

Windows (DLL, from a VS or MinGW environment that matches your toolchain):

```bat
gcc -O2 -mavx2 -mfma -shared -o examples/llm_training/llm_backend.dll examples/llm_training/llm_backend.c -lm
```

## Fetch TinyStories (example)

```bash
mkdir -p data
wget -O data/tinystories_train.txt \
  "https://huggingface.co/datasets/roneneldan/TinyStories/resolve/main/TinyStoriesV2-GPT4-train.txt"
wget -O data/tinystories_val.txt \
  "https://huggingface.co/datasets/roneneldan/TinyStories/resolve/main/TinyStoriesV2-GPT4-valid.txt"
```

## Run (once your toolchain can link the backend)

Invoke the compiler you built from the repo root, for example:

```bash
build/casprix examples/llm_training/train_llm.cpx -o build/train_llm
```

FFI loading details vary by platform; use `casprix --help` for backend flags (`--emit-c`, `--mir`, etc.) as your checkout supports them.

### Quick smoke run

If `train_llm.cpx` exposes a tiny debug configuration (e.g. `make_tiny_run()`), enable it for a short CPU-friendly test.

## Static checks

Use semantic-only mode while iterating:

```bash
build/casprix examples/llm_training/train_llm.cpx --check-only
```

## Memory and ownership

Sources are expected to pass the MIR borrow checker when `--safe-mode` / related flags are enabled; exact coverage depends on compiler revision.
