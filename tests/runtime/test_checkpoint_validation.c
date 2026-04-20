/*
 * Regression test: Checkpoint SHA-256 validation
 *
 * Tests:
 *   1. checkpoint_sha256() against a known-good test vector (NIST FIPS 180-4).
 *   2. checkpoint_save() → checkpoint_load() round-trip succeeds.
 *   3. checkpoint_load() rejects a file whose payload byte has been flipped.
 *   4. checkpoint_load() rejects a file with version > CHECKPOINT_CURRENT_VERSION.
 *
 * Build:
 *   gcc -O2 -I runtime/ai/llm \
 *       tests/runtime/test_checkpoint_validation.c \
 *       runtime/ai/llm/checkpoint.c runtime/ai/llm/tensor.c \
 *       runtime/ai/llm/transformer.c runtime/ai/llm/ops.c \
 *       -lm -o test_checkpoint_validation
 */

#include "checkpoint.h"
#include "tensor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── SHA-256 known-answer test ─────────────────────────────────────── */
static void test_sha256_kat(void) {
    /* NIST FIPS 180-4 example: SHA-256("abc") */
    static const u8 expected[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    u8 digest[32];
    checkpoint_sha256("abc", 3, digest);
    assert(memcmp(digest, expected, 32) == 0);
    printf("  sha256_kat: PASS\n");

    /* Empty string */
    static const u8 empty_expected[32] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
        0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,
        0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
    };
    checkpoint_sha256("", 0, digest);
    assert(memcmp(digest, empty_expected, 32) == 0);
    printf("  sha256_empty: PASS\n");
}

/* ── Minimal model helper ──────────────────────────────────────────── */
static TransformerModel* make_tiny_model(void) {
    /* vocab=16, hidden=8, layers=1, heads=2, ffn=16, max_seq=4 */
    return transformer_create(16, 8, 1, 2, 16, 4);
}

/* ── Round-trip test ───────────────────────────────────────────────── */
static void test_roundtrip(void) {
    const char* path = "test_ckpt_tmp.bin";
    TransformerModel* model = make_tiny_model();

    CheckpointHeader hdr = {0};
    hdr.magic         = 0x4D4F4445;
    hdr.version       = CHECKPOINT_CURRENT_VERSION;
    hdr.vocab_size    = 16;
    hdr.hidden_dim    = 8;
    hdr.num_layers    = 1;
    hdr.num_heads     = 2;
    hdr.ffn_dim       = 16;
    hdr.max_seq_len   = 4;
    hdr.training_step = 42;
    hdr.last_loss     = 3.14f;

    int ok = checkpoint_save(model, &hdr, path);
    assert(ok);

    CheckpointHeader hdr_out = {0};
    TransformerModel* loaded = checkpoint_load(path, &hdr_out);
    assert(loaded != NULL);
    assert(hdr_out.magic   == 0x4D4F4445);
    assert(hdr_out.version == CHECKPOINT_CURRENT_VERSION);
    assert(hdr_out.training_step == 42);

    transformer_destroy(model);
    transformer_destroy(loaded);
    remove(path);
    printf("  roundtrip: PASS\n");
}

/* ── Corruption detection test ─────────────────────────────────────── */
static void test_corruption(void) {
    const char* path = "test_ckpt_corrupt.bin";
    TransformerModel* model = make_tiny_model();

    CheckpointHeader hdr = {0};
    hdr.magic         = 0x4D4F4445;
    hdr.version       = CHECKPOINT_CURRENT_VERSION;
    hdr.vocab_size    = 16;
    hdr.hidden_dim    = 8;
    hdr.num_layers    = 1;
    hdr.num_heads     = 2;
    hdr.ffn_dim       = 16;
    hdr.max_seq_len   = 4;

    checkpoint_save(model, &hdr, path);
    transformer_destroy(model);

    /* Flip one byte in the payload (past the header). */
    FILE* f = fopen(path, "r+b");
    assert(f);
    fseek(f, (long)sizeof(CheckpointHeader) + 16, SEEK_SET);
    u8 byte;
    fread(&byte, 1, 1, f);
    byte ^= 0xFF;
    fseek(f, (long)sizeof(CheckpointHeader) + 16, SEEK_SET);
    fwrite(&byte, 1, 1, f);
    fclose(f);

    TransformerModel* loaded = checkpoint_load(path, NULL);
    assert(loaded == NULL);  /* must reject corrupted file */
    remove(path);
    printf("  corruption_detected: PASS\n");
}

/* ── Bad version test ──────────────────────────────────────────────── */
static void test_bad_version(void) {
    const char* path = "test_ckpt_badver.bin";
    TransformerModel* model = make_tiny_model();

    CheckpointHeader hdr = {0};
    hdr.magic   = 0x4D4F4445;
    hdr.version = CHECKPOINT_CURRENT_VERSION;
    hdr.vocab_size = 16; hdr.hidden_dim = 8;
    hdr.num_layers = 1;  hdr.num_heads  = 2;
    hdr.ffn_dim = 16;    hdr.max_seq_len = 4;
    checkpoint_save(model, &hdr, path);
    transformer_destroy(model);

    /* Overwrite version field with a future version number. */
    FILE* f = fopen(path, "r+b");
    assert(f);
    u32 bad_ver = CHECKPOINT_CURRENT_VERSION + 99;
    fseek(f, (long)offsetof(CheckpointHeader, version), SEEK_SET);
    fwrite(&bad_ver, sizeof(u32), 1, f);
    fclose(f);

    TransformerModel* loaded = checkpoint_load(path, NULL);
    assert(loaded == NULL);
    remove(path);
    printf("  bad_version_rejected: PASS\n");
}

int main(void) {
    printf("=== test_checkpoint_validation ===\n");
    test_sha256_kat();
    test_roundtrip();
    test_corruption();
    test_bad_version();
    printf("All checkpoint validation tests passed.\n");
    return 0;
}
