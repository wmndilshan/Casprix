# Casprix LLM Training — TinyStories
## Directory layout

```
examples/llm_training/
├── tokenizer.cpx      # BPE tokenizer (train, encode, decode)
├── transformer.cpx    # GPT-2 style transformer + AdamW optimizer
├── dataloader.cpx     # Binary shard DataLoader
├── train_llm.cpx      # Main entry point — orchestrates all phases
├── llm_backend.c      # C backend (all FFI implementations)
└── llm_backend.so     # Compiled shared library (generated)
```

## Casprix Syntax Reference (correct modern syntax used)

| Construct | Correct syntax |
|---|---|
| Immutable binding | `let name: type = value` |
| Mutable binding | `mut name: type = value` |
| Type-inferred binding | `name := expr` |
| C-style for loop | `for (i: i32 = 0; i < n; i = i + 1) { }` |
| For-in loop | `for item in collection { }` |
| Increment | `i = i + 1` (no `++`) |
| Class field (immutable) | `public let field: type` |
| Class field (mutable) | `public mut field: type` |
| Function | `func name(p: type) -> returntype { }` |
| Extern C function | `extern func name(p: type) -> type` |
| Null pointer | `let p: rawptr = 0` |
| Object creation | `let obj: Class = new Class()` |
| Method call | `obj.method(args)` |
| Import module | `import "path/to/module"` |

## Training Pipeline

### Phase 1 — BPE Tokenizer Training
Trains a 32K-vocab BPE tokenizer on raw TinyStories text:
```
BpeTokenizer.train("data/tinystories_train.txt", 32000)
```
- Starts from 256 byte-level tokens
- Iteratively merges the most frequent adjacent pair
- Saves vocab + merge rules to `data/tok32k.bin`

### Phase 2 — Dataset Preparation
Encodes the raw corpus into binary shards:
```
data/train_shard_0000.bin   ← flat i32 token arrays
data/val_shard_0000.bin
```

### Phase 3 — Model Initialization
Creates a small GPT-2 style transformer (Tiny config: ~15M params):

| Hyperparameter | Value |
|---|---|
| Vocabulary | 32,000 |
| Hidden dim | 288 |
| Layers | 6 |
| Attention heads | 6 |
| FFN dim | 768 |
| Max seq len | 256 |

### Phase 4 — Pre-Training Loop
Full cosine LR schedule with linear warmup:
- `max_lr = 3e-4`, `min_lr = 3e-5`, warmup = 700 steps
- AdamW: β₁=0.9, β₂=0.95, ε=1e-8, weight_decay=0.1
- Batch: 64 × 256 = 16,384 tokens/step
- Total: ~19,073 steps (≈1 epoch over TinyStories at 300M tokens)
- Checkpoints saved every 1,000 steps

## Build and Run

### 1. Build the C backend
```bash
cd /path/to/Casprix
gcc -O2 -mavx2 -mfma -shared -fPIC \
    -o examples/llm_training/llm_backend.so \
    examples/llm_training/llm_backend.c \
    -lm -lpthread
```

### 2. Download TinyStories data
```bash
mkdir -p data
# Training split (~300M tokens)
wget -O data/tinystories_train.txt \
  "https://huggingface.co/datasets/roneneldan/TinyStories/resolve/main/TinyStoriesV2-GPT4-train.txt"
# Validation split
wget -O data/tinystories_val.txt \
  "https://huggingface.co/datasets/roneneldan/TinyStories/resolve/main/TinyStoriesV2-GPT4-valid.txt"
```

### 3. Run (once Casprix codegen supports full FFI linking)
```bash
cpx run examples/llm_training/train_llm.cpx
```

### 4. Quick smoke test (10 steps, synthetic data)
Edit `train_llm.cpx` and uncomment:
```cpx
cfg.make_tiny_run()
```

## Memory Safety
All `.cpx` files are verified by `bin/casprix --check-only`:
- No use-after-move errors
- No dangling reference returns  
- Ownership checker active on all class instances
- ARC reference counting on all heap objects
