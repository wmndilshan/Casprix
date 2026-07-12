/*
 * Casprix Compiler - SIMD Virtualization Layer Test Suite
 *
 * Exercises every moving part of the vector pipeline end-to-end:
 *
 *   1.  MIR_VEC_* construction via the builder API.
 *   2.  simd_legalize_* -- native-width clamping and scalarization.
 *   3.  Backend text emission -- x86-64 (AVX2 / AVX-512), AArch64
 *       (NEON) and the pure-scalar fallback.
 *   4.  Host capability probing.
 *
 * The test asserts on the *text* emitted by each backend so the suite
 * is independent of any assembler -- we simply look for the expected
 * mnemonics inside the backend's buffered output.
 */

#include "compiler/ir/mir.h"
#include "compiler/ir/mir_backend.h"
#include "compiler/ir/mir_opt.h"
#include "compiler/opt/simd.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define REQUIRE(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; \
           fprintf(stderr, "  FAIL  %s  (line %d)\n", #cond, __LINE__); } \
} while (0)

#define REQUIRE_MSG(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; \
           fprintf(stderr, "  FAIL  %s  : %s  (line %d)\n", #cond, msg, __LINE__); } \
} while (0)

/* ──────────────────────────────────────────────────────────────────
 * Small helpers
 * ────────────────────────────────────────────────────────────────── */

static char* slurp_buffer(FILE* buf) {
    if (!buf) return NULL;
    fflush(buf);
    rewind(buf);
    size_t cap = 4096, n = 0;
    char* mem = (char*)malloc(cap);
    int c;
    while ((c = fgetc(buf)) != EOF) {
        if (n + 1 >= cap) { cap *= 2; mem = (char*)realloc(mem, cap); }
        mem[n++] = (char)c;
    }
    mem[n] = '\0';
    return mem;
}

static bool buffer_contains(FILE* buf, const char* needle) {
    char* text = slurp_buffer(buf);
    if (!text) return false;
    bool r = strstr(text, needle) != NULL;
    free(text);
    return r;
}

/* Build a tiny module with one "f32 * f32 -> f32" vector kernel:
 *
 *     fn kernel(a: ptr<f32>, b: ptr<f32>, out: ptr<f32>) {
 *         v1 = vec.load.4x f32 a
 *         v2 = vec.load.4x f32 b
 *         v3 = vec.mul.4x f32 v1, v2
 *         vec.store.4x f32 out, v3
 *         ret
 *     }
 */
static MirFunction* build_mul_kernel(MirModule* m, int logical_width) {
    MirType* f32 = mir_type_f32(m);
    MirType* p_f32 = mir_type_ptr(m, f32);

    MirParam params[3] = {
        { .name = "a",   .type = p_f32 },
        { .name = "b",   .type = p_f32 },
        { .name = "out", .type = p_f32 },
    };
    MirFunction* f = mir_module_add_function(m, "kernel",
                                              mir_type_void(m), params, 3);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirBuilder b;
    mir_builder_init(&b, m, f);
    mir_builder_set_block(&b, entry);

    MirValueId pa   = f->params[0].value_id;
    MirValueId pb   = f->params[1].value_id;
    MirValueId pout = f->params[2].value_id;

    MirValueId v1 = mir_build_vec_load(&b, pa, f32, logical_width, true);
    MirValueId v2 = mir_build_vec_load(&b, pb, f32, logical_width, true);
    MirValueId v3 = mir_build_vec_binop(&b, MIR_VEC_MUL, v1, v2, f32, logical_width);
    mir_build_vec_store(&b, pout, v3, f32, logical_width, true);
    mir_build_ret_void(&b);
    return f;
}

/* ──────────────────────────────────────────────────────────────────
 * 1) Capability model sanity
 * ────────────────────────────────────────────────────────────────── */
static void test_capability_model(void) {
    printf("\n[1] capability model\n");

    SimdTarget host = simd_target_detect_host();
    REQUIRE(host.name != NULL);
    printf("    host target: %s (%s, %d-bit)\n",
           mir_target_name(host.arch),
           simd_capability_name(host.capability),
           host.native_bits);

    SimdTarget x64 = simd_target_default(MIR_TARGET_X86_64);
    REQUIRE(x64.arch == MIR_TARGET_X86_64);
    REQUIRE(x64.capability == SIMD_CAP_AVX2);
    REQUIRE(x64.native_bits == 256);

    SimdTarget arm = simd_target_default(MIR_TARGET_AARCH64);
    REQUIRE(arm.arch == MIR_TARGET_AARCH64);
    REQUIRE(arm.capability == SIMD_CAP_NEON);
    REQUIRE(arm.native_bits == 128);

    /* Hybrid cross-arch requests must be coerced to a sane capability. */
    SimdTarget bad1 = simd_target_make(MIR_TARGET_AARCH64, SIMD_CAP_AVX2);
    REQUIRE(bad1.capability == SIMD_CAP_NEON);
    SimdTarget bad2 = simd_target_make(MIR_TARGET_X86_64, SIMD_CAP_NEON);
    REQUIRE(bad2.capability == SIMD_CAP_SSE2);

    /* Lane-count math. */
    MirModule* m = mir_module_create("capmod");
    MirType* f32 = mir_type_f32(m);
    MirType* i32 = mir_type_i32(m);
    MirType* f64 = mir_type_f64(m);

    REQUIRE(simd_native_lanes(x64, f32) == 8);   /* 256/32 */
    REQUIRE(simd_native_lanes(x64, f64) == 4);   /* 256/64 */
    REQUIRE(simd_native_lanes(arm, f32) == 4);   /* 128/32 */
    REQUIRE(simd_native_lanes(arm, i32) == 4);
    REQUIRE(simd_has_fma(x64));
    REQUIRE(simd_has_fma(arm));

    SimdTarget scalar = simd_target_make(MIR_TARGET_VM, SIMD_CAP_NONE);
    REQUIRE(simd_native_lanes(scalar, f32) == 1);
    REQUIRE(!simd_has_fma(scalar));

    mir_module_destroy(m);
}

/* ──────────────────────────────────────────────────────────────────
 * 2) Builder + printer smoke
 * ────────────────────────────────────────────────────────────────── */
static void test_builder_roundtrip(void) {
    printf("\n[2] builder + printer\n");
    MirModule* m = mir_module_create("bmod");
    MirFunction* f = build_mul_kernel(m, 8);
    REQUIRE(f != NULL);

    /* Collect every VEC_* instruction in the function. */
    int vec_ops = 0;
    int found_load = 0, found_mul = 0, found_store = 0;
    for (MirBlock* bb = f->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            if (mir_opcode_is_vec(inst->opcode)) vec_ops++;
            if (inst->opcode == MIR_VEC_LOAD)   found_load++;
            if (inst->opcode == MIR_VEC_MUL)    found_mul++;
            if (inst->opcode == MIR_VEC_STORE)  found_store++;
        }
    }
    REQUIRE(vec_ops == 4);   /* 2 loads + mul + store */
    REQUIRE(found_load == 2);
    REQUIRE(found_mul == 1);
    REQUIRE(found_store == 1);

    /* Printer must emit vec.* lines. */
    FILE* tmp = tmpfile();
    REQUIRE(tmp != NULL);
    mir_print_function(f, tmp);
    REQUIRE(buffer_contains(tmp, "vec.load"));
    REQUIRE(buffer_contains(tmp, "vec.mul"));
    REQUIRE(buffer_contains(tmp, "vec.store"));
    fclose(tmp);

    mir_module_destroy(m);
}

/* ──────────────────────────────────────────────────────────────────
 * 3) Legalization: AVX2, NEON, scalar
 * ────────────────────────────────────────────────────────────────── */
static int count_vec_ops(MirFunction* f) {
    int n = 0;
    for (MirBlock* bb = f->block_list; bb; bb = bb->next_block)
        for (MirInst* i = bb->first; i; i = i->next)
            if (mir_opcode_is_vec(i->opcode)) n++;
    return n;
}

static void test_legalize_clamp(void) {
    printf("\n[3a] legalize: clamp to native width\n");
    MirModule* m = mir_module_create("clamp");
    /* Ask for 32 logical lanes -- AVX2 should clamp to 8 for f32. */
    MirFunction* f = build_mul_kernel(m, 32);

    SimdLegalizeStats stats = {0};
    SimdTarget avx2 = simd_target_default(MIR_TARGET_X86_64);
    int changes = simd_legalize_function(f, avx2, &stats);
    REQUIRE(changes > 0);
    REQUIRE(stats.vec_ops_seen == 4);
    REQUIRE(stats.vec_ops_split == 4); /* every op was clamped */

    for (MirBlock* bb = f->block_list; bb; bb = bb->next_block) {
        for (MirInst* i = bb->first; i; i = i->next) {
            if (mir_opcode_is_vec(i->opcode)) {
                REQUIRE(i->as.vec.width == 8);
            }
        }
    }

    /* NEON clamps f32 logical-32 to 4. */
    MirModule* m2 = mir_module_create("clamp2");
    MirFunction* f2 = build_mul_kernel(m2, 32);
    SimdTarget neon = simd_target_default(MIR_TARGET_AARCH64);
    simd_legalize_function(f2, neon, NULL);
    for (MirBlock* bb = f2->block_list; bb; bb = bb->next_block) {
        for (MirInst* i = bb->first; i; i = i->next) {
            if (mir_opcode_is_vec(i->opcode))
                REQUIRE(i->as.vec.width == 4);
        }
    }

    mir_module_destroy(m);
    mir_module_destroy(m2);
}

static void test_legalize_scalar(void) {
    printf("\n[3b] legalize: scalarize on no-SIMD target\n");
    MirModule* m = mir_module_create("scalar");
    /* Use width=1 so in-place scalarization is legal. */
    MirFunction* f = build_mul_kernel(m, 1);

    SimdLegalizeStats stats = {0};
    SimdTarget scalar = simd_target_make(MIR_TARGET_VM, SIMD_CAP_NONE);
    int changes = simd_legalize_function(f, scalar, &stats);

    REQUIRE(changes >= 3);              /* 2 loads + mul + store */
    REQUIRE(stats.vec_ops_scalarized >= 3);
    REQUIRE(count_vec_ops(f) == 0);     /* no VEC_* should remain */

    /* The mul should have become MIR_FMUL. */
    bool found_fmul = false;
    for (MirBlock* bb = f->block_list; bb; bb = bb->next_block)
        for (MirInst* i = bb->first; i; i = i->next)
            if (i->opcode == MIR_FMUL) found_fmul = true;
    REQUIRE(found_fmul);

    /* The optimizer must still accept the legalized function. */
    MirOptStats opt_stats = {0};
    mir_optimize_function(f, MIR_OPT_STANDARD, &opt_stats);
    REQUIRE(count_vec_ops(f) == 0); /* still no vec ops after -O2 */

    mir_module_destroy(m);
}

/* ──────────────────────────────────────────────────────────────────
 * 4) Backend text emission
 * ────────────────────────────────────────────────────────────────── */
static void emit_through_backend(MirBackend* back, MirModule* m) {
    REQUIRE(back != NULL);
    bool ok = mir_backend_emit_module(back, m);
    /* finalize only runs if config.output_path is set -- we intentionally
     * leave it NULL so the buffer stays in memory for assertions. */
    REQUIRE(ok);
}

static void test_emit_x86_64(void) {
    printf("\n[4a] emit: x86-64 AVX2\n");
    MirModule* m = mir_module_create("x86mod");
    (void)build_mul_kernel(m, 8);

    SimdTarget avx2 = simd_target_default(MIR_TARGET_X86_64);
    simd_legalize_module(m, avx2, NULL);

    MirBackendConfig cfg = {
        .target = MIR_TARGET_X86_64,
        .output_format = MIR_OUTPUT_ASM,
        .opt_level = 2,
        .debug_info = false,
        .pic = false,
        .size_opt = false,
        .output_path = NULL,
    };
    MirBackend* back = mir_backend_create_x86_64(cfg);
    emit_through_backend(back, m);

    FILE* buf = mir_backend_get_text_buffer(back);
    REQUIRE(buf != NULL);
    REQUIRE_MSG(buffer_contains(buf, "ymm"),   "uses AVX2 ymm registers");
    REQUIRE_MSG(buffer_contains(buf, "vmulps") ||
                buffer_contains(buf, "vmulpd"), "emits packed-multiply mnemonic");
    REQUIRE_MSG(buffer_contains(buf, "vmovap") ||
                buffer_contains(buf, "vmovup") ||
                buffer_contains(buf, "vmovdq"), "emits vector move");

    back->destroy(back);
    mir_module_destroy(m);
}

static void test_emit_x86_64_avx512(void) {
    printf("\n[4b] emit: x86-64 AVX-512 broadcast/fma/dot\n");
    MirModule* m = mir_module_create("x86_512");
    MirType* f32 = mir_type_f32(m);
    MirType* p_f32 = mir_type_ptr(m, f32);
    MirType* void_ty = mir_type_void(m);

    MirParam params[1] = { { .name = "p", .type = p_f32 } };
    MirFunction* f = mir_module_add_function(m, "kernel", void_ty, params, 1);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirBuilder b;
    mir_builder_init(&b, m, f);
    mir_builder_set_block(&b, entry);

    MirValueId p = f->params[0].value_id;

    MirValueId scalar = mir_build_const_float(&b, 3.14, f32);
    MirValueId v1 = mir_build_vec_load(&b, p, f32, 16, true);
    MirValueId v2 = mir_build_vec_broadcast(&b, scalar, f32, 16);
    MirValueId v3 = mir_build_vec_fma(&b, v1, v2, v1, f32, 16);
    MirValueId v4 = mir_build_vec_dot(&b, v1, v2, f32, 16);
    MirValueId v5 = mir_build_vec_reduce_sum(&b, v3, f32, 16);
    (void)v4; (void)v5;
    mir_build_vec_store(&b, p, v3, f32, 16, false); /* unaligned store */
    mir_build_ret_void(&b);

    SimdTarget avx512 = simd_target_make(MIR_TARGET_X86_64, SIMD_CAP_AVX512);
    simd_legalize_module(m, avx512, NULL);

    MirBackendConfig cfg = { .target = MIR_TARGET_X86_64,
                              .output_format = MIR_OUTPUT_ASM };
    MirBackend* back = mir_backend_create_x86_64(cfg);
    mir_backend_set_simd_capability(back, SIMD_CAP_AVX512);
    (void)avx512;

    emit_through_backend(back, m);
    FILE* buf = mir_backend_get_text_buffer(back);
    REQUIRE(buf != NULL);
    REQUIRE_MSG(buffer_contains(buf, "zmm"),         "emits zmm on AVX-512");
    REQUIRE_MSG(buffer_contains(buf, "vfmadd231"),   "emits FMA mnemonic");
    REQUIRE_MSG(buffer_contains(buf, "vbroadcast"),  "emits broadcast mnemonic");
    REQUIRE_MSG(buffer_contains(buf, "vdpps") ||
                buffer_contains(buf, "vhadd"),       "emits dot/hadd");
    REQUIRE_MSG(buffer_contains(buf, "vmovups"),     "emits unaligned store");

    back->destroy(back);
    mir_module_destroy(m);
}

static void test_emit_aarch64(void) {
    printf("\n[4c] emit: AArch64 NEON\n");
    MirModule* m = mir_module_create("armmod");
    (void)build_mul_kernel(m, 4);

    SimdTarget neon = simd_target_default(MIR_TARGET_AARCH64);
    simd_legalize_module(m, neon, NULL);

    MirBackendConfig cfg = {
        .target = MIR_TARGET_AARCH64,
        .output_format = MIR_OUTPUT_ASM,
    };
    MirBackend* back = mir_backend_create_aarch64(cfg);
    emit_through_backend(back, m);

    FILE* buf = mir_backend_get_text_buffer(back);
    REQUIRE(buf != NULL);
    REQUIRE_MSG(buffer_contains(buf, "v0.4s") ||
                buffer_contains(buf, ".4s"),   "uses NEON 4s arrangement");
    REQUIRE_MSG(buffer_contains(buf, "fmul"),  "emits fmul");
    REQUIRE_MSG(buffer_contains(buf, "ld1"),   "emits ld1");
    REQUIRE_MSG(buffer_contains(buf, "st1"),   "emits st1");

    back->destroy(back);
    mir_module_destroy(m);
}

static void test_emit_scalar_fallback(void) {
    printf("\n[4d] emit: scalar fallback (SIMD_CAP_NONE)\n");
    MirModule* m = mir_module_create("scamod");
    MirFunction* f = build_mul_kernel(m, 1);

    SimdTarget scalar = simd_target_make(MIR_TARGET_VM, SIMD_CAP_NONE);
    int changed = simd_legalize_function(f, scalar, NULL);
    REQUIRE(changed > 0);
    REQUIRE(count_vec_ops(f) == 0);

    MirBackendConfig cfg = {
        .target = MIR_TARGET_VM,
        .output_format = MIR_OUTPUT_ASM,
    };
    MirBackend* back = mir_backend_create_scalar(cfg);
    emit_through_backend(back, m);
    FILE* buf = mir_backend_get_text_buffer(back);
    REQUIRE(buf != NULL);
    /* Scalar backend mustn't emit any SIMD mnemonics. */
    REQUIRE_MSG(!buffer_contains(buf, "ymm"),  "no AVX2 ymm in scalar output");
    REQUIRE_MSG(!buffer_contains(buf, "zmm"),  "no AVX-512 zmm in scalar output");
    REQUIRE_MSG(!buffer_contains(buf, ".4s"),  "no NEON 4s in scalar output");
    REQUIRE(buffer_contains(buf, "scalar"));

    back->destroy(back);
    mir_module_destroy(m);
}

/* ──────────────────────────────────────────────────────────────────
 * 5) Host detection never produces nonsense
 * ────────────────────────────────────────────────────────────────── */
static void test_host_detection(void) {
    printf("\n[5] host detection sanity\n");
    SimdTarget h = simd_target_detect_host();
    REQUIRE(h.capability >= SIMD_CAP_NONE && h.capability <= SIMD_CAP_AVX512);
    REQUIRE(h.native_bits == 0 || h.native_bits == 128 ||
            h.native_bits == 256 || h.native_bits == 512);
    /* Capability / bits must agree. */
    if (h.capability == SIMD_CAP_NONE)   REQUIRE(h.native_bits == 0);
    if (h.capability == SIMD_CAP_SSE2)   REQUIRE(h.native_bits == 128);
    if (h.capability == SIMD_CAP_NEON)   REQUIRE(h.native_bits == 128);
    if (h.capability == SIMD_CAP_AVX2)   REQUIRE(h.native_bits == 256);
    if (h.capability == SIMD_CAP_AVX512) REQUIRE(h.native_bits == 512);
}

int main(void) {
    printf("=== Casprix Compiler: SIMD virtualization test suite ===\n");
    test_capability_model();
    test_builder_roundtrip();
    test_legalize_clamp();
    test_legalize_scalar();
    test_emit_x86_64();
    test_emit_x86_64_avx512();
    test_emit_aarch64();
    test_emit_scalar_fallback();
    test_host_detection();
    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0) printf("All SIMD virtualization tests passed.\n");
    return g_fail == 0 ? 0 : 1;
}
