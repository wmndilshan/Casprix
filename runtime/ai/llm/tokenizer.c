/*
 * LLM Runtime - BPE Tokenizer Implementation
 */

#include "tokenizer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <malloc.h>
#define aligned_alloc(alignment, size) _aligned_malloc(size, alignment)
#define aligned_free(ptr) _aligned_free(ptr)
#else
#define aligned_free(ptr) free(ptr)
#endif

#include <ctype.h>

// ===== BYTE-LEVEL VOCABULARY =====

static void init_byte_vocab(TokenizerVocab* vocab) {
    // Initialize with 256 byte-level tokens
    vocab->size = 256;
    vocab->tokens = (char**)malloc(256 * sizeof(char*));
    vocab->ids = (i32*)malloc(256 * sizeof(i32));
    
    for (i32 i = 0; i < 256; i++) {
        vocab->tokens[i] = (char*)malloc(2);
        vocab->tokens[i][0] = (char)i;
        vocab->tokens[i][1] = '\0';
        vocab->ids[i] = i;
    }
}

// ===== TOKENIZER =====

Tokenizer* tokenizer_create(i32 vocab_size) {
    Tokenizer* tok = (Tokenizer*)malloc(sizeof(Tokenizer));
    
    tok->vocab = (TokenizerVocab*)malloc(sizeof(TokenizerVocab));
    init_byte_vocab(tok->vocab);
    
    tok->num_merges = vocab_size - 256;  // Number of merges to learn
    tok->merges = (BPEMerge*)calloc(tok->num_merges, sizeof(BPEMerge));
    tok->max_token_len = 16;
    
    // Special tokens (add to vocab)
    tok->bos_token = 0;   // <BOS>
    tok->eos_token = 1;   // <EOS>
    tok->pad_token = 2;   // <PAD>
    tok->unk_token = 3;   // <UNK>
    
    return tok;
}

/* ═══════════════════════════════════════════════════════════════════
 * BPE Training — O(N log N) with max-heap + lazy deletion
 *
 * Data structures (file-scoped, not exported):
 *   PairMap  — open-addressed hash map: packed pair key → count
 *   MaxHeap  — binary max-heap of (count, token_a, token_b, merged)
 *   Sequence — doubly-linked list over the input text for O(1) removal
 *
 * Training functions may use malloc (offline, not a hot path).
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Pair map ─────────────────────────────────────────────────────── */

#define PMAP_EMPTY   INT64_MIN

typedef struct { int64_t key; int64_t val; } PMapEntry;

typedef struct {
    PMapEntry* entries;
    int        cap;      /* power of 2 */
    int        size;
} PairMap;

static int64_t pmap_key(i32 a, i32 b) {
    return ((int64_t)(uint32_t)a << 32) | (uint32_t)b;
}

static PairMap* pmap_create(int cap) {
    PairMap* m = (PairMap*)malloc(sizeof(PairMap));
    m->cap     = cap;
    m->size    = 0;
    m->entries = (PMapEntry*)malloc((size_t)cap * sizeof(PMapEntry));
    for (int i = 0; i < cap; i++) m->entries[i].key = PMAP_EMPTY;
    return m;
}

static void pmap_destroy(PairMap* m) { free(m->entries); free(m); }

static int64_t* pmap_get_or_insert(PairMap* m, int64_t key) {
    /* Grow if > 50% full. */
    if (m->size * 2 >= m->cap) {
        int new_cap = m->cap * 2;
        PMapEntry* ne = (PMapEntry*)malloc((size_t)new_cap * sizeof(PMapEntry));
        for (int i = 0; i < new_cap; i++) ne[i].key = PMAP_EMPTY;
        for (int i = 0; i < m->cap; i++) {
            if (m->entries[i].key == PMAP_EMPTY) continue;
            int h = (int)((uint64_t)m->entries[i].key * 2654435761ULL >> 32) & (new_cap - 1);
            while (ne[h].key != PMAP_EMPTY) h = (h + 1) & (new_cap - 1);
            ne[h] = m->entries[i];
        }
        free(m->entries);
        m->entries = ne;
        m->cap     = new_cap;
    }
    int h = (int)((uint64_t)key * 2654435761ULL >> 32) & (m->cap - 1);
    while (m->entries[h].key != PMAP_EMPTY && m->entries[h].key != key)
        h = (h + 1) & (m->cap - 1);
    if (m->entries[h].key == PMAP_EMPTY) {
        m->entries[h].key = key;
        m->entries[h].val = 0;
        m->size++;
    }
    return &m->entries[h].val;
}

static int64_t pmap_get(const PairMap* m, int64_t key) {
    int h = (int)((uint64_t)key * 2654435761ULL >> 32) & (m->cap - 1);
    while (m->entries[h].key != PMAP_EMPTY) {
        if (m->entries[h].key == key) return m->entries[h].val;
        h = (h + 1) & (m->cap - 1);
    }
    return 0;
}

/* ── Max-heap ─────────────────────────────────────────────────────── */

typedef struct { int64_t count; i32 token_a; i32 token_b; i32 merged; } HeapEntry;

typedef struct {
    HeapEntry* data;
    int        size;
    int        cap;
} MaxHeap;

static MaxHeap* heap_create(int cap) {
    MaxHeap* h = (MaxHeap*)malloc(sizeof(MaxHeap));
    h->data = (HeapEntry*)malloc((size_t)cap * sizeof(HeapEntry));
    h->size = 0;
    h->cap  = cap;
    return h;
}

static void heap_destroy(MaxHeap* h) { free(h->data); free(h); }

static void heap_push(MaxHeap* h, HeapEntry e) {
    if (h->size >= h->cap) {
        h->cap *= 2;
        h->data = (HeapEntry*)realloc(h->data, (size_t)h->cap * sizeof(HeapEntry));
    }
    int i = h->size++;
    h->data[i] = e;
    /* Sift up. */
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (h->data[parent].count >= h->data[i].count) break;
        HeapEntry tmp = h->data[parent];
        h->data[parent] = h->data[i];
        h->data[i] = tmp;
        i = parent;
    }
}

static int heap_pop(MaxHeap* h, HeapEntry* out) {
    if (h->size == 0) return 0;
    *out = h->data[0];
    h->data[0] = h->data[--h->size];
    /* Sift down. */
    int i = 0;
    while (1) {
        int l = 2*i+1, r = 2*i+2, largest = i;
        if (l < h->size && h->data[l].count > h->data[largest].count) largest = l;
        if (r < h->size && h->data[r].count > h->data[largest].count) largest = r;
        if (largest == i) break;
        HeapEntry tmp = h->data[i];
        h->data[i] = h->data[largest];
        h->data[largest] = tmp;
        i = largest;
    }
    return 1;
}

/* ── BPE train ────────────────────────────────────────────────────── */

Tokenizer* tokenizer_train(const char* text, i32 vocab_size, i32 min_freq) {
    Tokenizer* tok = tokenizer_create(vocab_size);
    if (!text || !*text) return tok;

    size_t text_len = strlen(text);
    int    N        = (int)text_len;  /* number of positions */

    /* Sequence as virtual doubly-linked list.
     * seq_tok[i]  = current token at position i
     * seq_prev[i] = previous live index (-1 if none)
     * seq_next[i] = next live index (N if none)
     * seq_live[i] = 1 if position is active                           */
    i32*    seq_tok  = (i32*)malloc((size_t)N * sizeof(i32));
    int*    seq_prev = (int*)malloc((size_t)N * sizeof(int));
    int*    seq_next = (int*)malloc((size_t)N * sizeof(int));
    uint8_t* seq_live = (uint8_t*)calloc((size_t)N, 1);

    for (int i = 0; i < N; i++) {
        seq_tok[i]  = (i32)(unsigned char)text[i];
        seq_prev[i] = i - 1;
        seq_next[i] = i + 1;
        seq_live[i] = 1;
    }

    /* Count initial pair frequencies. */
    int pmap_cap = 1;
    while (pmap_cap < N * 2) pmap_cap <<= 1;
    PairMap* freq = pmap_create(pmap_cap);

    for (int i = 0; i + 1 < N; i++) {
        int64_t key = pmap_key(seq_tok[i], seq_tok[i + 1]);
        (*pmap_get_or_insert(freq, key))++;
    }

    /* Push all pairs into the max-heap. */
    MaxHeap* heap = heap_create(freq->size > 16 ? freq->size : 16);
    for (int i = 0; i < freq->cap; i++) {
        if (freq->entries[i].key == PMAP_EMPTY) continue;
        if (freq->entries[i].val <= 0) continue;
        int64_t k = freq->entries[i].key;
        HeapEntry e = {
            .count   = freq->entries[i].val,
            .token_a = (i32)(k >> 32),
            .token_b = (i32)(k & 0xFFFFFFFF),
            .merged  = 256 + 0  /* filled in during merge loop */
        };
        heap_push(heap, e);
    }

    /* Merge loop. */
    i32 num_merges_done = 0;
    int next_id = 256;  /* first merged token ID */

    while (num_merges_done < tok->num_merges) {
        HeapEntry best;
        if (!heap_pop(heap, &best)) break;

        /* Lazy deletion: skip if count doesn't match current freq. */
        int64_t cur = pmap_get(freq, pmap_key(best.token_a, best.token_b));
        if (cur != best.count) continue;
        if (cur < (int64_t)min_freq) break;

        /* Record this merge. */
        i32 merged_id = next_id++;
        tok->merges[num_merges_done].token_a      = best.token_a;
        tok->merges[num_merges_done].token_b      = best.token_b;
        tok->merges[num_merges_done].merged_token = merged_id;
        tok->merges[num_merges_done].freq         = (i32)cur;
        num_merges_done++;

        /* Replace all occurrences of (token_a, token_b) in the sequence. */
        for (int i = 0; i < N; i++) {
            if (!seq_live[i]) continue;
            int j = seq_next[i];
            if (j >= N || !seq_live[j]) continue;
            if (seq_tok[i] != best.token_a || seq_tok[j] != best.token_b) continue;

            /* Decrement count of (prev, token_a) pair if it exists. */
            int prev = seq_prev[i];
            if (prev >= 0 && seq_live[prev]) {
                int64_t k = pmap_key(seq_tok[prev], seq_tok[i]);
                int64_t* v = pmap_get_or_insert(freq, k);
                if (*v > 0) (*v)--;
            }

            /* Decrement count of (token_b, next) pair if it exists. */
            int nxt = seq_next[j];
            if (nxt < N && seq_live[nxt]) {
                int64_t k = pmap_key(seq_tok[j], seq_tok[nxt]);
                int64_t* v = pmap_get_or_insert(freq, k);
                if (*v > 0) (*v)--;
            }

            /* Remove old pair count. */
            {
                int64_t* v = pmap_get_or_insert(freq, pmap_key(best.token_a, best.token_b));
                if (*v > 0) (*v)--;
            }

            /* Replace i with merged token, remove j. */
            seq_tok[i] = merged_id;
            seq_live[j] = 0;
            seq_next[i] = seq_next[j];
            if (seq_next[j] < N) seq_prev[seq_next[j]] = i;

            /* Increment count of (prev, merged) pair. */
            if (prev >= 0 && seq_live[prev]) {
                int64_t k = pmap_key(seq_tok[prev], merged_id);
                int64_t cnt = ++(*pmap_get_or_insert(freq, k));
                HeapEntry e2 = { cnt, seq_tok[prev], merged_id, 0 };
                heap_push(heap, e2);
            }

            /* Increment count of (merged, next) pair. */
            int nxt2 = seq_next[i];
            if (nxt2 < N && seq_live[nxt2]) {
                int64_t k = pmap_key(merged_id, seq_tok[nxt2]);
                int64_t cnt = ++(*pmap_get_or_insert(freq, k));
                HeapEntry e2 = { cnt, merged_id, seq_tok[nxt2], 0 };
                heap_push(heap, e2);
            }
        }
    }

    pmap_destroy(freq);
    heap_destroy(heap);
    free(seq_tok);
    free(seq_prev);
    free(seq_next);
    free(seq_live);

    return tok;
}

// Apply learned merges: greedy left-to-right O(num_merges * N)
// This matches GPT-style BPE inference.
i32* tokenizer_encode(Tokenizer* tok, const char* text, i32* out_len) {
    if (!text || !out_len) return NULL;

    size_t text_len = strlen(text);
    if (text_len == 0) { *out_len = 0; return NULL; }

    /* Start with byte-level tokenization. */
    i32* tokens = (i32*)malloc(text_len * sizeof(i32));
    i32  n      = (i32)text_len;
    for (i32 i = 0; i < n; i++)
        tokens[i] = (i32)(unsigned char)text[i];

    /* Apply each merge in order (O(num_merges × N) total). */
    for (i32 m = 0; m < tok->num_merges && tok->merges[m].freq >= 0; m++) {
        i32 a = tok->merges[m].token_a;
        i32 b = tok->merges[m].token_b;
        i32 c = tok->merges[m].merged_token;

        /* Single pass: collapse all occurrences of (a, b) → c. */
        i32 write = 0;
        for (i32 r = 0; r < n; ) {
            if (r + 1 < n && tokens[r] == a && tokens[r + 1] == b) {
                tokens[write++] = c;
                r += 2;
            } else {
                tokens[write++] = tokens[r++];
            }
        }
        n = write;
    }

    *out_len = n;
    return tokens;
}

// Decode tokens back to text
char* tokenizer_decode(Tokenizer* tok, const i32* tokens, i32 len) {
    if (!tokens || len <= 0) return NULL;
    
    // Allocate buffer (max: len * max_token_len)
    char* text = (char*)malloc((len * tok->max_token_len + 1) * sizeof(char));
    i32 pos = 0;
    
    for (i32 i = 0; i < len; i++) {
        i32 token_id = tokens[i];
        
        // Byte-level token
        if (token_id < 256) {
            text[pos++] = (char)token_id;
        }
        // Special tokens
        else if (token_id == tok->bos_token || token_id == tok->eos_token ||
                 token_id == tok->pad_token) {
            continue;  // Skip special tokens
        }
        // Merged tokens (would need vocab lookup)
        else {
            text[pos++] = '?';  // Unknown for now
        }
    }
    
    text[pos] = '\0';
    return text;
}

// Save tokenizer
bool tokenizer_save(Tokenizer* tok, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    
    // Write header
    fwrite(&tok->vocab->size, sizeof(i32), 1, f);
    fwrite(&tok->num_merges, sizeof(i32), 1, f);
    
    // Write merges
    fwrite(tok->merges, sizeof(BPEMerge), tok->num_merges, f);
    
    // Write special tokens
    fwrite(&tok->bos_token, sizeof(i32), 1, f);
    fwrite(&tok->eos_token, sizeof(i32), 1, f);
    fwrite(&tok->pad_token, sizeof(i32), 1, f);
    fwrite(&tok->unk_token, sizeof(i32), 1, f);
    
    fclose(f);
    return true;
}

// Load tokenizer
Tokenizer* tokenizer_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    
    Tokenizer* tok = (Tokenizer*)malloc(sizeof(Tokenizer));
    tok->vocab = (TokenizerVocab*)malloc(sizeof(TokenizerVocab));
    
    // Read header
    fread(&tok->vocab->size, sizeof(i32), 1, f);
    fread(&tok->num_merges, sizeof(i32), 1, f);
    
    // Initialize byte vocab
    init_byte_vocab(tok->vocab);
    
    // Read merges
    tok->merges = (BPEMerge*)malloc(tok->num_merges * sizeof(BPEMerge));
    fread(tok->merges, sizeof(BPEMerge), tok->num_merges, f);
    
    size_t ign;
    ign = fread(&tok->bos_token, sizeof(i32), 1, f);
    ign = fread(&tok->eos_token, sizeof(i32), 1, f);
    ign = fread(&tok->pad_token, sizeof(i32), 1, f);
    ign = fread(&tok->unk_token, sizeof(i32), 1, f);
    (void)ign;
    
    tok->max_token_len = 16;
    
    fclose(f);
    return tok;
}

void tokenizer_destroy(Tokenizer* tok) {
    if (!tok) return;
    
    if (tok->vocab) {
        for (i32 i = 0; i < tok->vocab->size; i++) {
            free(tok->vocab->tokens[i]);
        }
        free(tok->vocab->tokens);
        free(tok->vocab->ids);
        free(tok->vocab);
    }
    
    free(tok->merges);
    free(tok);
}

// ===== DATASET =====

#define DATASET_MAGIC 0x54494E59  // "TINY"

Dataset* dataset_create(i32 num_sequences, i32 seq_length, i32 vocab_size) {
    Dataset* ds = (Dataset*)malloc(sizeof(Dataset));
    
    ds->header.magic = DATASET_MAGIC;
    ds->header.version = 1;
    ds->header.num_sequences = num_sequences;
    ds->header.seq_length = seq_length;
    ds->header.vocab_size = vocab_size;
    
    ds->data_size = num_sequences * seq_length * sizeof(i32);
    ds->data = (i32*)aligned_alloc(64, ds->data_size);
    ds->owns_memory = true;
    
    return ds;
}

bool dataset_save(Dataset* ds, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    
    // Write header
    fwrite(&ds->header, sizeof(DatasetHeader), 1, f);
    
    // Write data
    fwrite(ds->data, sizeof(i32), 
           ds->header.num_sequences * ds->header.seq_length, f);
    
    fclose(f);
    return true;
}

Dataset* dataset_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    
    Dataset* ds = (Dataset*)malloc(sizeof(Dataset));
    
    // Read header
    size_t ign = fread(&ds->header, sizeof(DatasetHeader), 1, f);
    (void)ign;
    
    // Validate magic
    if (ds->header.magic != DATASET_MAGIC) {
        free(ds);
        fclose(f);
        return NULL;
    }
    
    // Read data
    ds->data_size = ds->header.num_sequences * ds->header.seq_length * sizeof(i32);
    ds->data = (i32*)aligned_alloc(64, ds->data_size);
    ign = fread(ds->data, sizeof(i32), 
          ds->header.num_sequences * ds->header.seq_length, f);
    
    ds->owns_memory = true;
    
    fclose(f);
    return ds;
}

const i32* dataset_get_sequence(Dataset* ds, i32 index) {
    if (index < 0 || index >= ds->header.num_sequences) {
        return NULL;
    }
    return ds->data + (index * ds->header.seq_length);
}

void dataset_destroy(Dataset* ds) {
    if (!ds) return;
    
    if (ds->owns_memory && ds->data) {
        aligned_free(ds->data);
    }
    free(ds);
}

// ===== UTILITIES =====

// Fisher-Yates shuffle
void dataset_shuffle(Dataset* ds) {
    i32 seq_len = ds->header.seq_length;
    
    for (i32 i = ds->header.num_sequences - 1; i > 0; i--) {
        i32 j = rand() % (i + 1);
        
        // Swap sequences i and j
        for (i32 k = 0; k < seq_len; k++) {
            i32 temp = ds->data[i * seq_len + k];
            ds->data[i * seq_len + k] = ds->data[j * seq_len + k];
            ds->data[j * seq_len + k] = temp;
        }
    }
}

Dataset** dataset_split(Dataset* input, f32 train_ratio, f32 val_ratio, f32 test_ratio) {
    // Validate ratios
    f32 sum = train_ratio + val_ratio + test_ratio;
    if (sum < 0.99f || sum > 1.01f) {
        return NULL;
    }
    
    i32 total = input->header.num_sequences;
    i32 train_size = (i32)(total * train_ratio);
    i32 val_size = (i32)(total * val_ratio);
    i32 test_size = total - train_size - val_size;
    
    Dataset** splits = (Dataset**)malloc(3 * sizeof(Dataset*));
    
    // Create datasets
    splits[0] = dataset_create(train_size, input->header.seq_length, input->header.vocab_size);
    splits[1] = dataset_create(val_size, input->header.seq_length, input->header.vocab_size);
    splits[2] = dataset_create(test_size, input->header.seq_length, input->header.vocab_size);
    
    // Copy data
    i32 seq_len = input->header.seq_length;
    i32 offset = 0;
    
    memcpy(splits[0]->data, input->data, train_size * seq_len * sizeof(i32));
    offset += train_size;
    
    memcpy(splits[1]->data, input->data + offset * seq_len, val_size * seq_len * sizeof(i32));
    offset += val_size;
    
    memcpy(splits[2]->data, input->data + offset * seq_len, test_size * seq_len * sizeof(i32));
    
    return splits;
}
