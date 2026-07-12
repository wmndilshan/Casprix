/*
 * MIR corpus verification — golden .cpx programs through parse → sema → MIR,
 * then structural verification, textual MIR dump, opcode lowerability contract,
 * and x86-64 SIMD-backend assembly emission (hybrid pipeline; scalar ops are
 * annotated for asmgen in the emitted text).
 */

#include "compiler/frontend/lexer.h"
#include "compiler/frontend/parser.h"
#include "compiler/sema/semantic.h"
#include "compiler/ir/mir.h"
#include "compiler/ir/mir_lower.h"
#include "compiler/ir/mir_mem2reg.h"
#include "compiler/ir/mir_opt.h"
#include "compiler/ir/mir_backend.h"
#include "compiler/opt/simd.h"
#include "driver/io.h"
#include "support/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

static int mkdir_one(const char* path) {
    if (!path || !*path) return 0;
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

/* Create all parent segments of `path` (path is modified in-place). */
static void ensure_dirs(char* path) {
#ifdef _WIN32
    if (path[0] && path[1] == ':') {
        for (char* p = path + 2; *p; p++) {
            if (*p == '/' || *p == '\\') {
                *p = '\0';
                (void)mkdir_one(path);
                *p = '/';
            }
        }
        (void)mkdir_one(path);
        return;
    }
#endif
    for (char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            if (*path) (void)mkdir_one(path);
            *p = '/';
        }
    }
    (void)mkdir_one(path);
}

static int verify_module_lower_mapping(MirModule* m) {
    for (MirFunction* f = m->func_list; f; f = f->next_func) {
        if (f->is_extern) continue;
        for (MirBlock* bb = f->block_list; bb; bb = bb->next_block) {
            for (MirInst* i = bb->first; i; i = i->next) {
                if (!mir_opcode_has_native_lower_mapping(i->opcode, MIR_TARGET_X86_64)) {
                    fprintf(stderr, "MIR_VERIFY: opcode %d has no lower mapping\n",
                            (int)i->opcode);
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int run_backend_asm(MirModule* m, const char* asm_path) {
    MirBackendConfig cfg = {
        .target        = MIR_TARGET_X86_64,
        .output_format = MIR_OUTPUT_ASM,
        .opt_level     = 1,
        .debug_info    = false,
        .pic           = false,
        .size_opt      = false,
        .output_path   = asm_path,
    };
    MirBackend* be = mir_backend_create_x86_64(cfg);
    if (!be) return 1;
    if (!mir_backend_emit_module(be, m)) {
        be->destroy(be);
        return 1;
    }
    be->destroy(be);
    return 0;
}

static int verify_corpus_file(const char* cpx_path, const char* out_dir) {
    char mir_path[1024];
    char asm_path[1024];
    const char* base = strrchr(cpx_path, '/');
    if (!base) base = strrchr(cpx_path, '\\');
    base = base ? base + 1 : cpx_path;
    char stem[256];
    snprintf(stem, sizeof(stem), "%s", base);
    char* dot = strrchr(stem, '.');
    if (dot) *dot = '\0';

    snprintf(mir_path, sizeof(mir_path), "%s/%s.mir", out_dir, stem);
    snprintf(asm_path, sizeof(asm_path), "%s/%s.asm", out_dir, stem);

    char* src = driver_read_file(cpx_path);
    if (!src) {
        fprintf(stderr, "MIR_VERIFY: cannot read %s\n", cpx_path);
        return 1;
    }

    had_error = false;
    Lexer lexer;
    init_lexer(&lexer, src);
    Parser parser;
    init_parser(&parser, &lexer);
    int stmt_count = 0;
    Stmt** stmts = parse(&parser, &stmt_count);
    if (!stmts || parser.had_error) {
        fprintf(stderr, "MIR_VERIFY: parse failed for %s\n", cpx_path);
        free(src);
        return 1;
    }

    SemanticAnalyzer sema;
    init_semantic_analyzer(&sema);
    if (!analyze_program(&sema, stmts, stmt_count) || had_error) {
        fprintf(stderr, "MIR_VERIFY: semantic analysis failed for %s\n", cpx_path);
        free_semantic_analyzer(&sema);
        free(stmts);
        free(src);
        return 1;
    }

    MirModule* mod = mir_lower_program(stmts, stmt_count, sema.symbols, "corpus");
    if (!mod) {
        fprintf(stderr, "MIR_VERIFY: MIR lowering failed for %s\n", cpx_path);
        free_semantic_analyzer(&sema);
        free(stmts);
        free(src);
        return 1;
    }

    MirMem2RegStats m2r = {0};
    mir_mem2reg_module(mod, &m2r);
    MirOptStats opt = {0};
    mir_optimize_module(mod, MIR_OPT_BASIC, &opt);

    if (mir_verify_module(mod, stderr) != 0) {
        fprintf(stderr, "MIR_VERIFY: mir_verify_module failed for %s\n", cpx_path);
        mir_module_destroy(mod);
        free_semantic_analyzer(&sema);
        free(stmts);
        free(src);
        return 1;
    }

    FILE* mf = fopen(mir_path, "wb");
    if (!mf) {
        fprintf(stderr, "MIR_VERIFY: cannot write %s\n", mir_path);
        mir_module_destroy(mod);
        free_semantic_analyzer(&sema);
        free(stmts);
        free(src);
        return 1;
    }
    mir_print_module(mod, mf);
    fclose(mf);

    if (verify_module_lower_mapping(mod) != 0) {
        mir_module_destroy(mod);
        free_semantic_analyzer(&sema);
        free(stmts);
        free(src);
        return 1;
    }

    if (run_backend_asm(mod, asm_path) != 0) {
        fprintf(stderr, "MIR_VERIFY: backend emit failed for %s\n", cpx_path);
        mir_module_destroy(mod);
        free_semantic_analyzer(&sema);
        free(stmts);
        free(src);
        return 1;
    }

    mir_module_destroy(mod);
    free_semantic_analyzer(&sema);
    free(stmts);
    free(src);
    return 0;
}

/* Inline MIR_VEC_ADD kernel — exercises SIMD virtualization + verifier. */
static int verify_vec_add_fixture(const char* out_dir) {
    MirModule* m = mir_module_create("vec_fixture");
    MirType* f32 = mir_type_f32(m);
    MirType* pf32 = mir_type_ptr(m, f32);
    MirParam params[3] = {
        { .name = "a", .type = pf32 },
        { .name = "b", .type = pf32 },
        { .name = "out", .type = pf32 },
    };
    MirFunction* f = mir_module_add_function(m, "vec_add4", mir_type_void(m), params, 3);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirBuilder b;
    mir_builder_init(&b, m, f);
    mir_builder_set_block(&b, entry);
    MirValueId pa = f->params[0].value_id;
    MirValueId pb = f->params[1].value_id;
    MirValueId po = f->params[2].value_id;
    MirValueId v1 = mir_build_vec_load(&b, pa, f32, 4, true);
    MirValueId v2 = mir_build_vec_load(&b, pb, f32, 4, true);
    MirValueId v3 = mir_build_vec_binop(&b, MIR_VEC_ADD, v1, v2, f32, 4);
    mir_build_vec_store(&b, po, v3, f32, 4, true);
    mir_build_ret_void(&b);

    SimdTarget t = simd_target_default(MIR_TARGET_X86_64);
    simd_legalize_function(f, t, NULL);

    if (mir_verify_module(m, stderr) != 0) {
        mir_module_destroy(m);
        return 1;
    }
    char asm_path[1024];
    snprintf(asm_path, sizeof(asm_path), "%s/vec_add4_fixture.asm", out_dir);
    if (run_backend_asm(m, asm_path) != 0) {
        mir_module_destroy(m);
        return 1;
    }
    mir_module_destroy(m);
    return 0;
}

int main(void) {
    const char* root = getenv("CASPRIX_SOURCE_ROOT");
    if (!root || !*root) root = ".";

    char out_buf[1024];
    const char* env_out = getenv("MIR_VERIFY_OUT");
    if (env_out && *env_out) {
        snprintf(out_buf, sizeof(out_buf), "%s", env_out);
    } else {
        snprintf(out_buf, sizeof(out_buf), "%s/mir_verify_staging", root);
    }
    ensure_dirs(out_buf);
    const char* out = out_buf;

    static const char* corpus_rel[] = {
        "tests/corpus/cfg_regex_dfa.cpx",
        "tests/corpus/stringview_linear.cpx",
        "tests/corpus/float_pipeline.cpx",
    };

    char path[1024];
    for (size_t i = 0; i < sizeof(corpus_rel) / sizeof(corpus_rel[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", root, corpus_rel[i]);
        printf("MIR_VERIFY: %s\n", path);
        if (verify_corpus_file(path, out) != 0) return 1;
    }

    printf("MIR_VERIFY: vec_add fixture\n");
    if (verify_vec_add_fixture(out) != 0) return 1;

    printf("All MIR corpus verification tests passed.\n");
    return 0;
}
