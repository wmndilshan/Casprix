# TinyStories training pipeline

CPU-oriented transformer training examples for the TinyStories dataset, implemented as Casprix (`.cpx`) programs that call into the C runtime under `runtime/ai/llm/`.

## Model snapshot (documentation)

Rough target shape (adjust in source as needed):

```
Architecture: Transformer decoder
Parameters:   ~15M (order of magnitude)
Vocab:        BPE (e.g. 8K–32K range depending on tokenizer step)
```

## Quick start

### 1. Build the compiler and runtime

From the **repository root**:

```bash
scripts/build.sh          # Unix
# or
scripts\build.bat         # Windows (cmd)
```

This produces `build/casprix` (or `build/Release/casprix.exe` on VS multi-config) and links against `libcasprix_runtime`.

### 2. Run pipeline steps

Use the path to your built compiler (examples assume `build/casprix` on Unix):

```bash
# Download dataset (writes under data/ — large download)
build/casprix examples/tinystories/download_dataset.cpx -o build/tinystories_dl

# Train BPE tokenizer → data/tokenizer.bin (paths per script)
build/casprix examples/tinystories/train_tokenizer.cpx -o build/tinystories_tok

# Preprocess tokenized shards
build/casprix examples/tinystories/preprocess_data.cpx -o build/tinystories_pre

# Train (long-running)
build/casprix examples/tinystories/train_model.cpx -o build/tinystories_train

# Generate text from a checkpoint
build/casprix examples/tinystories/generate_text.cpx -o build/tinystories_gen
```

`pipeline.cpx` orchestrates the flow; individual scripts exist for each phase.

## Repository layout (relevant paths)

```
runtime/ai/llm/            # C tensors, transformer, tokenizer, training, …
lib/llm/                   # Casprix-facing helpers (e.g. Tokenizer.cpx)
examples/tinystories/      # This directory — download, tokenizer, train, generate
data/                      # Created locally (git-ignored) — corpus, tokenizer.bin, shards
checkpoints/               # Optional — model checkpoints (git-ignored)
```

## Performance notes

Throughput depends on CPU (AVX2/FMA), batch size, and sequence length. Treat any GFLOPS or wall-clock numbers in old docs as **non-binding** unless you re-benchmark on your machine.

## Further reading

- [Feature summary](../../docs/FEATURES.md)
- [ML / inference architecture](../../docs/ML_INFERENCE_BACKEND_ARCHITECTURE.md) (high-level)

## License

Part of the Casprix project.
