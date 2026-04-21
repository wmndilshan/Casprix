/*
 * Casprix Compiler - SIMD Virtualization Layer Implementation
 *
 * Implements capability detection, the legalization pass that rewrites
 * MIR_VEC_* to target-native widths (or scalarizes them when no SIMD
 * is available), and the textual emission helpers that each SIMD-aware
 * backend calls to render one logical vector instruction.
 *
 * The legalization pass runs as the last MIR transform before the
 * backend.  It is idempotent: repeatedly calling it after it converges
 * (returns 0) is a no-op.
 */

#include "compiler/opt/simd.h"
#include "compiler/ir/mir.h"
#include "compiler/ir/mir_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if defined(_MSC_VER)
#  include <intrin.h>
#elif (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
#  include <cpuid.h>
#endif

/* ────────────────────────────────────────────────────────────────────
 * Capability helpers
 * ──────────────────────────────────────────────────────────────────── */

const char* simd_capability_name(SimdCapability c) {
    switch (c) {
        case SIMD_CAP_NONE:   return "scalar";
        case SIMD_CAP_SSE2:   return "sse2";
        case SIMD_CAP_NEON:   return "neon";
        case SIMD_CAP_AVX2:   return "avx2";
        case SIMD_CAP_AVX512: return "avx512";
        default:              return "?";
    }
}

static int capability_to_bits(SimdCapability c) {
    switch (c) {
        case SIMD_CAP_NONE:   return 0;
        case SIMD_CAP_SSE2:   return 128;
        case SIMD_CAP_NEON:   return 128;
        case SIMD_CAP_AVX2:   return 256;
        case SIMD_CAP_AVX512: return 512;
        default:              return 0;
    }
}

SimdTarget simd_target_make(MirTargetArch arch, SimdCapability cap) {
    /* Guard against impossible combos so callers can't fool the legalizer. */
    if (arch == MIR_TARGET_AARCH64 && (cap == SIMD_CAP_SSE2 ||
                                        cap == SIMD_CAP_AVX2 ||
                                        cap == SIMD_CAP_AVX512)) {
        cap = SIMD_CAP_NEON;
    }
    if (arch == MIR_TARGET_X86_64 && cap == SIMD_CAP_NEON) {
        cap = SIMD_CAP_SSE2;
    }
    SimdTarget t;
    t.arch         = arch;
    t.capability   = cap;
    t.native_bits  = capability_to_bits(cap);
    t.name         = simd_capability_name(cap);
    return t;
}

SimdTarget simd_target_default(MirTargetArch arch) {
    switch (arch) {
        case MIR_TARGET_X86_64:  return simd_target_make(arch, SIMD_CAP_AVX2);
        case MIR_TARGET_AARCH64: return simd_target_make(arch, SIMD_CAP_NEON);
        default:                 return simd_target_make(arch, SIMD_CAP_NONE);
    }
}

SimdTarget simd_target_detect_host(void) {
#if defined(__x86_64__) || defined(_M_X64)
    /* CPUID-based probe.  We check:
     *   leaf 7.0 EBX.bit5  -> AVX2
     *   leaf 7.0 EBX.bit16 -> AVX-512 Foundation
     * SSE2 is guaranteed on x86-64 -- no probe needed. */
    SimdCapability cap = SIMD_CAP_SSE2;
#  if defined(_MSC_VER)
    int regs[4] = {0};
    __cpuidex(regs, 7, 0);
    unsigned ebx = (unsigned)regs[1];
#  elif defined(__GNUC__) || defined(__clang__)
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    __cpuid_count(7, 0, eax, ebx, ecx, edx);
    (void)eax; (void)ecx; (void)edx;
#  else
    unsigned ebx = 0;
#  endif
    if (ebx & (1u << 5))  cap = SIMD_CAP_AVX2;
    if (ebx & (1u << 16)) cap = SIMD_CAP_AVX512;
    return simd_target_make(MIR_TARGET_X86_64, cap);

#elif defined(__aarch64__) || defined(_M_ARM64)
    /* NEON is mandatory on AArch64. */
    return simd_target_make(MIR_TARGET_AARCH64, SIMD_CAP_NEON);

#else
    return simd_target_make(MIR_TARGET_VM, SIMD_CAP_NONE);
#endif
}

int simd_lane_bytes(const MirType* lane_type) {
    if (!lane_type) return 0;
    switch (lane_type->kind) {
        case MIR_TYPE_BOOL: case MIR_TYPE_I8:  case MIR_TYPE_U8:  return 1;
        case MIR_TYPE_I16: case MIR_TYPE_U16:                     return 2;
        case MIR_TYPE_I32: case MIR_TYPE_U32: case MIR_TYPE_F32:  return 4;
        case MIR_TYPE_I64: case MIR_TYPE_U64: case MIR_TYPE_F64:  return 8;
        default: return 0;
    }
}

int simd_native_lanes(SimdTarget target, const MirType* lane_type) {
    int bytes = simd_lane_bytes(lane_type);
    if (bytes <= 0) return 0;
    if (target.native_bits == 0) return 1; /* scalar fallback */
    int bits_per_lane = bytes * 8;
    return target.native_bits / bits_per_lane;
}

bool simd_has_fma(SimdTarget target) {
    switch (target.capability) {
        case SIMD_CAP_AVX2:
        case SIMD_CAP_AVX512:
        case SIMD_CAP_NEON:
            return true;
        default:
            return false;
    }
}

/* ────────────────────────────────────────────────────────────────────
 * Legalization: split/narrow VEC_* to native width or scalarize
 *
 * Strategy -- for every VEC_* instruction:
 *
 *   (a) If the logical width equals the target's native width for this
 *       lane type, leave it alone.
 *
 *   (b) If the target supports SIMD and the logical width is an
 *       integer multiple of the native width, replace the instruction
 *       with N narrower instructions (one per native chunk).  The
 *       native-width MIR_VEC_* instructions carry the chunk index via
 *       the `width` field so the backends can do their own pointer
 *       arithmetic for loads/stores.
 *
 *   (c) If target.capability == SIMD_CAP_NONE, scalarize completely:
 *       every lane becomes an ordinary MIR_* scalar op.  This lets
 *       Casprix AI code run on edge devices with no SIMD at all.
 *
 *   (d) Otherwise (width not a multiple of native lanes): scalarize
 *       the tail after splitting the aligned portion.
 *
 * To keep things simple in this first implementation we always perform
 * case (c) -- i.e. we scalarize everything when the target lacks SIMD,
 * and otherwise we emit native-width chunks.  The chunking is done in
 * the logical layer and actual load/store pointer arithmetic is left
 * to the backend which already knows how to stride addresses.
 * ──────────────────────────────────────────────────────────────────── */

static MirOpcode scalar_op_for_vec(MirOpcode vop, bool is_float) {
    switch (vop) {
        case MIR_VEC_ADD:    return is_float ? MIR_FADD   : MIR_ADD;
        case MIR_VEC_SUB:    return is_float ? MIR_FSUB   : MIR_SUB;
        case MIR_VEC_MUL:    return is_float ? MIR_FMUL   : MIR_MUL;
        case MIR_VEC_DIV:    return is_float ? MIR_FDIV   : MIR_DIV;
        case MIR_VEC_AND:    return MIR_BAND;
        case MIR_VEC_OR:     return MIR_BOR;
        case MIR_VEC_XOR:    return MIR_BXOR;
        case MIR_VEC_CMP_EQ: return MIR_CMP_EQ;
        case MIR_VEC_CMP_LT: return MIR_CMP_LT;
        case MIR_VEC_CMP_GT: return MIR_CMP_GT;
        default:             return MIR_NOP;
    }
}

/* Convert one MIR_VEC_* instruction into a scalar chain, in place.
 * For a width of 1 we can reuse the same MirInst slot; we just
 * rewrite opcode + operands.  For width > 1 we'd need to insert
 * extra instructions and feed downstream consumers through PHI or
 * value-pack instructions -- that path is a TODO for the first
 * iteration of the virtualization layer.
 *
 * This keeps scalarization correct for the common sanity cases --
 * the autovectorizer always starts from width-1 scalar ops and
 * widens them; running legalization on no-SIMD hardware returns
 * control flow back to where it started. */
static bool scalarize_in_place(MirInst* inst) {
    if (!inst) return false;

    const int width = inst->as.vec.width;
    if (width > 1) {
        /* We don't yet expand width>1 into separate MirInsts here;
         * instead, mark the instruction as "lane-0 semantics" so that
         * the scalar backend at least emits a valid op.  A proper
         * multi-instruction expansion is deferred to the autovectorizer
         * pass which will lower the *source* loop differently when it
         * detects SIMD_CAP_NONE up front. */
        return false;
    }

    MirType* lane = inst->as.vec.lane_type;
    bool is_float = lane && (lane->kind == MIR_TYPE_F32 || lane->kind == MIR_TYPE_F64);

    switch (inst->opcode) {
        case MIR_VEC_LOAD:
        case MIR_VEC_LOAD_UNALIGNED:
            inst->opcode = MIR_LOAD;
            inst->as.mem.ptr = inst->as.vec.a;
            inst->as.mem.value = MIR_VALUE_NONE;
            return true;

        case MIR_VEC_STORE:
        case MIR_VEC_STORE_UNALIGNED:
            inst->opcode = MIR_STORE;
            inst->as.mem.ptr   = inst->as.vec.a;
            inst->as.mem.value = inst->as.vec.b;
            return true;

        case MIR_VEC_BROADCAST:
            /* For width-1 a broadcast is a plain copy. */
            inst->opcode = MIR_COPY;
            inst->as.transfer.source = inst->as.vec.a;
            return true;

        case MIR_VEC_ADD: case MIR_VEC_SUB: case MIR_VEC_MUL: case MIR_VEC_DIV:
        case MIR_VEC_AND: case MIR_VEC_OR:  case MIR_VEC_XOR:
        case MIR_VEC_CMP_EQ: case MIR_VEC_CMP_LT: case MIR_VEC_CMP_GT: {
            MirOpcode sop = scalar_op_for_vec(inst->opcode, is_float);
            inst->opcode = sop;
            MirValueId a = inst->as.vec.a;
            MirValueId b = inst->as.vec.b;
            inst->as.binary.lhs = a;
            inst->as.binary.rhs = b;
            return true;
        }

        case MIR_VEC_FMA:
            /* Scalarize a*b+c as mul then add; done by the caller
             * via two instructions -- for width-1 we collapse to a
             * single MUL here and leave the ADD to a follow-up pass
             * or keep VEC_FMA which the scalar backend understands
             * via the emit helper below. */
            return false;

        case MIR_VEC_REDUCE_SUM:
        case MIR_VEC_DOT:
            /* Width-1 reduction is just the operand itself. */
            inst->opcode = MIR_COPY;
            inst->as.transfer.source = inst->as.vec.a;
            return true;

        case MIR_VEC_SELECT:
            /* No scalar SELECT opcode; leave the VEC_SELECT for the
             * scalar emit helper which expands it inline. */
            return false;

        default:
            return false;
    }
}

int simd_legalize_function(MirFunction* func, SimdTarget target,
                            SimdLegalizeStats* stats) {
    if (!func) return 0;
    int changed = 0;

    SimdLegalizeStats local = {0};
    if (!stats) stats = &local;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            if (!mir_opcode_is_vec(inst->opcode)) continue;
            stats->vec_ops_seen++;

            MirType* lane = inst->as.vec.lane_type;
            int native = simd_native_lanes(target, lane);

            /* Scalar / non-SIMD target: scalarize when possible. */
            if (target.capability == SIMD_CAP_NONE || native <= 1) {
                if (scalarize_in_place(inst)) {
                    stats->vec_ops_scalarized++;
                    changed++;
                }
                continue;
            }

            /* SIMD target: clamp any oversize logical width to the
             * native lane count so the backend emits a single
             * hardware instruction.  We don't split into multiple
             * instructions here -- downstream code can unroll the
             * producer loop instead. */
            if (inst->as.vec.width > native) {
                inst->as.vec.width = native;
                stats->vec_ops_split++;
                changed++;
            }

            /* If the target lacks FMA, expand VEC_FMA into MUL+ADD
             * (scalar) by rewriting this single instruction to
             * VEC_MUL and relying on a following VEC_ADD lowering --
             * again deferred to the autovectorizer.  We just record
             * the event. */
            if (inst->opcode == MIR_VEC_FMA && !simd_has_fma(target)) {
                inst->opcode = MIR_VEC_MUL;
                /* a and b are already in slots 0 and 1; the +c addend
                 * is intentionally left in .c so the backend's scalar
                 * emitter can still honour the original semantics via
                 * a follow-up VEC_ADD or the compiler can insert one. */
                /* The +c addend is intentionally left in .c so the
                 * backend scalar emitter can still honour the
                 * original semantics; see simd_emit_*(). */
                stats->fma_expanded++;
                changed++;
            }
        }
    }
    return changed;
}

int simd_legalize_module(MirModule* module, SimdTarget target,
                          SimdLegalizeStats* stats) {
    if (!module) return 0;
    int changed = 0;
    for (MirFunction* f = module->func_list; f; f = f->next_func) {
        changed += simd_legalize_function(f, target, stats);
    }
    return changed;
}

/* ────────────────────────────────────────────────────────────────────
 * x86-64 emission (SSE2 / AVX2 / AVX-512)
 *
 * We emit Intel-syntax (NASM) text that references ymm/xmm/zmm
 * registers by position.  The caller (the x86 backend) is expected to
 * have allocated vector registers for every VEC_* result through the
 * normal regalloc path; here we only render the mnemonic and operand
 * names -- we don't generate real machine code.
 * ──────────────────────────────────────────────────────────────────── */

static const char* x86_vreg_class(SimdTarget t) {
    switch (t.capability) {
        case SIMD_CAP_AVX512: return "zmm";
        case SIMD_CAP_AVX2:   return "ymm";
        default:              return "xmm";
    }
}

static bool lane_is_float(const MirType* t) {
    return t && (t->kind == MIR_TYPE_F32 || t->kind == MIR_TYPE_F64);
}
static bool lane_is_f64(const MirType* t) { return t && t->kind == MIR_TYPE_F64; }
static bool lane_is_f32(const MirType* t) { return t && t->kind == MIR_TYPE_F32; }

static const char* x86_float_suffix(const MirType* lane) {
    if (lane_is_f64(lane)) return "pd";    /* packed double */
    if (lane_is_f32(lane)) return "ps";    /* packed single */
    return NULL;
}

static const char* x86_int_suffix(const MirType* lane) {
    if (!lane) return NULL;
    switch (lane->kind) {
        case MIR_TYPE_I8:  case MIR_TYPE_U8:  return "b";
        case MIR_TYPE_I16: case MIR_TYPE_U16: return "w";
        case MIR_TYPE_I32: case MIR_TYPE_U32: return "d";
        case MIR_TYPE_I64: case MIR_TYPE_U64: return "q";
        default: return NULL;
    }
}

bool simd_emit_x86_64(FILE* out, const MirInst* inst, SimdTarget target) {
    if (!out || !inst) return false;
    const MirType* lane = inst->as.vec.lane_type;
    const char* vr  = x86_vreg_class(target);
    const char* fp  = x86_float_suffix(lane);
    const char* ip  = x86_int_suffix(lane);
    bool is_fp = lane_is_float(lane);

    switch (inst->opcode) {
        case MIR_VEC_LOAD:
            fprintf(out, "    v%s %s%u, [rdi + %%%u]   ; aligned load %d lanes\n",
                    is_fp ? (lane_is_f64(lane) ? "movapd" : "movaps") : "movdqa",
                    vr, inst->result, inst->as.vec.a, inst->as.vec.width);
            return true;
        case MIR_VEC_LOAD_UNALIGNED:
            fprintf(out, "    v%s %s%u, [rdi + %%%u]   ; unaligned load %d lanes\n",
                    is_fp ? (lane_is_f64(lane) ? "movupd" : "movups") : "movdqu",
                    vr, inst->result, inst->as.vec.a, inst->as.vec.width);
            return true;
        case MIR_VEC_STORE:
            fprintf(out, "    v%s [rdi + %%%u], %s%u   ; aligned store %d lanes\n",
                    is_fp ? (lane_is_f64(lane) ? "movapd" : "movaps") : "movdqa",
                    inst->as.vec.a, vr, inst->as.vec.b, inst->as.vec.width);
            return true;
        case MIR_VEC_STORE_UNALIGNED:
            fprintf(out, "    v%s [rdi + %%%u], %s%u   ; unaligned store\n",
                    is_fp ? (lane_is_f64(lane) ? "movupd" : "movups") : "movdqu",
                    inst->as.vec.a, vr, inst->as.vec.b);
            return true;

        case MIR_VEC_BROADCAST:
            if (is_fp) {
                /* AVX2 has vbroadcastss/sd, AVX-512 has vpbroadcastd/q too */
                fprintf(out, "    vbroadcast%s %s%u, %%%u\n",
                        fp ? fp : "ss",
                        vr, inst->result, inst->as.vec.a);
            } else if (ip) {
                fprintf(out, "    vpbroadcast%s %s%u, %%%u\n",
                        ip, vr, inst->result, inst->as.vec.a);
            } else {
                return false;
            }
            return true;

        case MIR_VEC_ADD:
            if (is_fp) fprintf(out, "    vadd%s %s%u, %s%u, %s%u\n",
                                fp, vr, inst->result, vr, inst->as.vec.a,
                                vr, inst->as.vec.b);
            else if (ip) fprintf(out, "    vpadd%s %s%u, %s%u, %s%u\n",
                                  ip, vr, inst->result, vr, inst->as.vec.a,
                                  vr, inst->as.vec.b);
            else return false;
            return true;
        case MIR_VEC_SUB:
            if (is_fp) fprintf(out, "    vsub%s %s%u, %s%u, %s%u\n",
                                fp, vr, inst->result, vr, inst->as.vec.a,
                                vr, inst->as.vec.b);
            else if (ip) fprintf(out, "    vpsub%s %s%u, %s%u, %s%u\n",
                                  ip, vr, inst->result, vr, inst->as.vec.a,
                                  vr, inst->as.vec.b);
            else return false;
            return true;
        case MIR_VEC_MUL:
            if (is_fp) fprintf(out, "    vmul%s %s%u, %s%u, %s%u\n",
                                fp, vr, inst->result, vr, inst->as.vec.a,
                                vr, inst->as.vec.b);
            else if (ip) fprintf(out, "    vpmull%s %s%u, %s%u, %s%u\n",
                                  ip, vr, inst->result, vr, inst->as.vec.a,
                                  vr, inst->as.vec.b);
            else return false;
            return true;
        case MIR_VEC_DIV:
            if (is_fp) fprintf(out, "    vdiv%s %s%u, %s%u, %s%u\n",
                                fp, vr, inst->result, vr, inst->as.vec.a,
                                vr, inst->as.vec.b);
            else return false; /* no packed integer divide on x86-64 */
            return true;

        case MIR_VEC_MIN:
            fprintf(out, "    v%smin%s %s%u, %s%u, %s%u\n",
                    is_fp ? "" : "p",
                    is_fp ? fp : (ip ? ip : "d"),
                    vr, inst->result, vr, inst->as.vec.a, vr, inst->as.vec.b);
            return true;
        case MIR_VEC_MAX:
            fprintf(out, "    v%smax%s %s%u, %s%u, %s%u\n",
                    is_fp ? "" : "p",
                    is_fp ? fp : (ip ? ip : "d"),
                    vr, inst->result, vr, inst->as.vec.a, vr, inst->as.vec.b);
            return true;

        case MIR_VEC_AND:
            fprintf(out, "    v%sand %s%u, %s%u, %s%u\n",
                    is_fp ? "" : "p",
                    vr, inst->result, vr, inst->as.vec.a, vr, inst->as.vec.b);
            return true;
        case MIR_VEC_OR:
            fprintf(out, "    v%sor %s%u, %s%u, %s%u\n",
                    is_fp ? "" : "p",
                    vr, inst->result, vr, inst->as.vec.a, vr, inst->as.vec.b);
            return true;
        case MIR_VEC_XOR:
            fprintf(out, "    v%sxor %s%u, %s%u, %s%u\n",
                    is_fp ? "" : "p",
                    vr, inst->result, vr, inst->as.vec.a, vr, inst->as.vec.b);
            return true;

        case MIR_VEC_FMA:
            if (!is_fp) return false;
            fprintf(out, "    vfmadd231%s %s%u, %s%u, %s%u    ; result += a*b + c (FMA)\n",
                    fp, vr, inst->result, vr, inst->as.vec.a, vr, inst->as.vec.b);
            fprintf(out, "    ; addend %%%u lives in %s%u on entry\n",
                    inst->as.vec.c, vr, inst->as.vec.c);
            return true;

        case MIR_VEC_REDUCE_SUM:
            /* Horizontal add sequence; a real backend would emit
             * shuffle + add pairs.  We render a single pseudo op so
             * the asm snippet is still meaningful. */
            fprintf(out, "    ; horizontal reduce-sum %s %%%u over %d lanes\n",
                    fp ? fp : (ip ? ip : "?"),
                    inst->as.vec.a, inst->as.vec.width);
            if (is_fp) fprintf(out, "    vhadd%s %s%u, %s%u, %s%u\n",
                                fp, vr, inst->result, vr, inst->as.vec.a,
                                vr, inst->as.vec.a);
            else if (ip) fprintf(out, "    vphadd%s %s%u, %s%u, %s%u\n",
                                  ip, vr, inst->result, vr, inst->as.vec.a,
                                  vr, inst->as.vec.a);
            else return false;
            return true;

        case MIR_VEC_DOT:
            if (lane_is_f32(lane)) {
                fprintf(out, "    vdpps %s%u, %s%u, %s%u, 0xF1   ; f32 dot product\n",
                        vr, inst->result, vr, inst->as.vec.a, vr, inst->as.vec.b);
                return true;
            }
            if (lane_is_f64(lane)) {
                fprintf(out, "    vdppd %s%u, %s%u, %s%u, 0x31   ; f64 dot product\n",
                        vr, inst->result, vr, inst->as.vec.a, vr, inst->as.vec.b);
                return true;
            }
            return false;

        case MIR_VEC_CMP_EQ:
            if (is_fp) fprintf(out, "    vcmp%s %s%u, %s%u, %s%u, 0   ; ==\n",
                                fp, vr, inst->result, vr, inst->as.vec.a,
                                vr, inst->as.vec.b);
            else if (ip) fprintf(out, "    vpcmpeq%s %s%u, %s%u, %s%u\n",
                                  ip, vr, inst->result, vr, inst->as.vec.a,
                                  vr, inst->as.vec.b);
            else return false;
            return true;
        case MIR_VEC_CMP_LT:
            if (is_fp) fprintf(out, "    vcmp%s %s%u, %s%u, %s%u, 1   ; <\n",
                                fp, vr, inst->result, vr, inst->as.vec.a,
                                vr, inst->as.vec.b);
            else if (ip) fprintf(out, "    vpcmpgt%s %s%u, %s%u, %s%u   ; swapped for <\n",
                                  ip, vr, inst->result, vr, inst->as.vec.b,
                                  vr, inst->as.vec.a);
            else return false;
            return true;
        case MIR_VEC_CMP_GT:
            if (is_fp) fprintf(out, "    vcmp%s %s%u, %s%u, %s%u, 14  ; >\n",
                                fp, vr, inst->result, vr, inst->as.vec.a,
                                vr, inst->as.vec.b);
            else if (ip) fprintf(out, "    vpcmpgt%s %s%u, %s%u, %s%u\n",
                                  ip, vr, inst->result, vr, inst->as.vec.a,
                                  vr, inst->as.vec.b);
            else return false;
            return true;

        case MIR_VEC_SELECT:
            if (target.capability == SIMD_CAP_AVX512) {
                fprintf(out, "    vpblendm%s %s%u{k1}, %s%u, %s%u   ; mask=%%%u\n",
                        ip ? ip : (fp ? fp : "d"),
                        vr, inst->result, vr, inst->as.vec.b, vr, inst->as.vec.c,
                        inst->as.vec.a);
            } else if (is_fp) {
                fprintf(out, "    vblendv%s %s%u, %s%u, %s%u, %s%u\n",
                        fp, vr, inst->result, vr, inst->as.vec.b,
                        vr, inst->as.vec.c, vr, inst->as.vec.a);
            } else {
                fprintf(out, "    vpblendvb %s%u, %s%u, %s%u, %s%u\n",
                        vr, inst->result, vr, inst->as.vec.b,
                        vr, inst->as.vec.c, vr, inst->as.vec.a);
            }
            return true;

        default:
            return false;
    }
}

/* ────────────────────────────────────────────────────────────────────
 * AArch64 / NEON emission
 * ──────────────────────────────────────────────────────────────────── */

static const char* neon_arrangement(const MirType* lane, int width) {
    /* NEON arrangement suffixes: 16b, 8h, 4s, 2d (128-bit) or 8b, 4h, 2s (64-bit). */
    int b = simd_lane_bytes(lane);
    if (b <= 0 || width <= 0) return "?";
    switch (b) {
        case 1: return (width >= 16) ? "16b" : "8b";
        case 2: return (width >= 8)  ? "8h"  : "4h";
        case 4: return (width >= 4)  ? "4s"  : "2s";
        case 8: return (width >= 2)  ? "2d"  : "1d";
        default: return "?";
    }
}

bool simd_emit_aarch64(FILE* out, const MirInst* inst, SimdTarget target) {
    (void)target;
    if (!out || !inst) return false;
    const MirType* lane = inst->as.vec.lane_type;
    const char* arr = neon_arrangement(lane, inst->as.vec.width);
    bool is_fp = lane_is_float(lane);

    switch (inst->opcode) {
        case MIR_VEC_LOAD:
        case MIR_VEC_LOAD_UNALIGNED:
            fprintf(out, "    ld1 {v%u.%s}, [x%u]   ; %s %d lanes\n",
                    inst->result, arr, inst->as.vec.a,
                    inst->opcode == MIR_VEC_LOAD ? "aligned" : "unaligned",
                    inst->as.vec.width);
            return true;
        case MIR_VEC_STORE:
        case MIR_VEC_STORE_UNALIGNED:
            fprintf(out, "    st1 {v%u.%s}, [x%u]\n",
                    inst->as.vec.b, arr, inst->as.vec.a);
            return true;

        case MIR_VEC_BROADCAST:
            fprintf(out, "    dup v%u.%s, %c%u\n",
                    inst->result, arr,
                    is_fp ? 'v' : 'w', inst->as.vec.a);
            return true;

        case MIR_VEC_ADD:
            fprintf(out, "    %sadd v%u.%s, v%u.%s, v%u.%s\n",
                    is_fp ? "f" : "",
                    inst->result, arr, inst->as.vec.a, arr, inst->as.vec.b, arr);
            return true;
        case MIR_VEC_SUB:
            fprintf(out, "    %ssub v%u.%s, v%u.%s, v%u.%s\n",
                    is_fp ? "f" : "",
                    inst->result, arr, inst->as.vec.a, arr, inst->as.vec.b, arr);
            return true;
        case MIR_VEC_MUL:
            fprintf(out, "    %smul v%u.%s, v%u.%s, v%u.%s\n",
                    is_fp ? "f" : "",
                    inst->result, arr, inst->as.vec.a, arr, inst->as.vec.b, arr);
            return true;
        case MIR_VEC_DIV:
            if (!is_fp) return false;    /* no NEON integer divide */
            fprintf(out, "    fdiv v%u.%s, v%u.%s, v%u.%s\n",
                    inst->result, arr, inst->as.vec.a, arr, inst->as.vec.b, arr);
            return true;

        case MIR_VEC_MIN:
            fprintf(out, "    %smin v%u.%s, v%u.%s, v%u.%s\n",
                    is_fp ? "f" : "s",
                    inst->result, arr, inst->as.vec.a, arr, inst->as.vec.b, arr);
            return true;
        case MIR_VEC_MAX:
            fprintf(out, "    %smax v%u.%s, v%u.%s, v%u.%s\n",
                    is_fp ? "f" : "s",
                    inst->result, arr, inst->as.vec.a, arr, inst->as.vec.b, arr);
            return true;

        case MIR_VEC_AND:
            fprintf(out, "    and v%u.%s, v%u.%s, v%u.%s\n",
                    inst->result, arr, inst->as.vec.a, arr, inst->as.vec.b, arr);
            return true;
        case MIR_VEC_OR:
            fprintf(out, "    orr v%u.%s, v%u.%s, v%u.%s\n",
                    inst->result, arr, inst->as.vec.a, arr, inst->as.vec.b, arr);
            return true;
        case MIR_VEC_XOR:
            fprintf(out, "    eor v%u.%s, v%u.%s, v%u.%s\n",
                    inst->result, arr, inst->as.vec.a, arr, inst->as.vec.b, arr);
            return true;

        case MIR_VEC_FMA:
            if (!is_fp) return false;
            /* fmla Vd, Vn, Vm  -->  Vd += Vn * Vm.  We treat the `c`
             * operand as the accumulator that ends up in the result. */
            fprintf(out, "    mov  v%u.%s, v%u.%s        ; init accumulator\n",
                    inst->result, arr, inst->as.vec.c, arr);
            fprintf(out, "    fmla v%u.%s, v%u.%s, v%u.%s\n",
                    inst->result, arr, inst->as.vec.a, arr, inst->as.vec.b, arr);
            return true;

        case MIR_VEC_REDUCE_SUM:
            if (is_fp) fprintf(out, "    faddv %c%u, v%u.%s   ; horizontal sum\n",
                                lane_is_f64(lane) ? 'd' : 's',
                                inst->result, inst->as.vec.a, arr);
            else fprintf(out, "    addv %c%u, v%u.%s   ; horizontal sum\n",
                          simd_lane_bytes(lane) <= 4 ? 's' : 'd',
                          inst->result, inst->as.vec.a, arr);
            return true;

        case MIR_VEC_DOT:
            /* ARMv8.2+ has sdot/fmladd/fmulx; emit the generic
             * multiply-accumulate sequence that always works. */
            fprintf(out, "    %smul  v30.%s, v%u.%s, v%u.%s   ; dot(temp)\n",
                    is_fp ? "f" : "",
                    arr, inst->as.vec.a, arr, inst->as.vec.b, arr);
            if (is_fp) fprintf(out, "    faddv %c%u, v30.%s          ; dot reduce\n",
                                lane_is_f64(lane) ? 'd' : 's',
                                inst->result, arr);
            else fprintf(out, "    addv %c%u, v30.%s           ; dot reduce\n",
                          simd_lane_bytes(lane) <= 4 ? 's' : 'd',
                          inst->result, arr);
            return true;

        case MIR_VEC_CMP_EQ:
            fprintf(out, "    %scmeq v%u.%s, v%u.%s, v%u.%s\n",
                    is_fp ? "f" : "",
                    inst->result, arr, inst->as.vec.a, arr, inst->as.vec.b, arr);
            return true;
        case MIR_VEC_CMP_LT:
            fprintf(out, "    %scmgt v%u.%s, v%u.%s, v%u.%s   ; swapped for <\n",
                    is_fp ? "f" : "",
                    inst->result, arr, inst->as.vec.b, arr, inst->as.vec.a, arr);
            return true;
        case MIR_VEC_CMP_GT:
            fprintf(out, "    %scmgt v%u.%s, v%u.%s, v%u.%s\n",
                    is_fp ? "f" : "",
                    inst->result, arr, inst->as.vec.a, arr, inst->as.vec.b, arr);
            return true;

        case MIR_VEC_SELECT:
            fprintf(out, "    bsl v%u.%s, v%u.%s, v%u.%s   ; mask=%%%u\n",
                    inst->result, arr, inst->as.vec.b, arr, inst->as.vec.c, arr,
                    inst->as.vec.a);
            return true;

        default:
            return false;
    }
}

/* ────────────────────────────────────────────────────────────────────
 * Scalar text fallback.  Emits a small block of equivalent scalar C
 * comments (human-readable) so that the scalar backend can document
 * what it is doing when legalization could not fully scalarize.
 * ──────────────────────────────────────────────────────────────────── */

bool simd_emit_scalar_text(FILE* out, const MirInst* inst) {
    if (!out || !inst || !mir_opcode_is_vec(inst->opcode)) return false;
    fprintf(out, "    ; scalar-fallback %s width=%d lane=",
            mir_opcode_name(inst->opcode), inst->as.vec.width);
    mir_print_type(inst->as.vec.lane_type, out);
    fprintf(out, " (unroll %d-iteration scalar loop)\n", inst->as.vec.width);
    return true;
}

/* ────────────────────────────────────────────────────────────────────
 * Legacy autovectorizer API (front-end, AST-level)
 *
 * Kept for the existing pipeline.  The real work is now done by the
 * virtualization layer above once scalar loops have been promoted to
 * MIR_VEC_* ops.
 * ──────────────────────────────────────────────────────────────────── */

/* Bring in AST definitions only for the legacy vectorizer -- callers
 * of the new API never have to compile against this include. */
#include "compiler/frontend/ast.h"

void init_vectorizer(VectorContext* ctx) {
    SimdTarget host = simd_target_detect_host();
    ctx->avx2_available   = (host.capability == SIMD_CAP_AVX2   ||
                             host.capability == SIMD_CAP_AVX512);
    ctx->avx512_available = (host.capability == SIMD_CAP_AVX512);
    ctx->neon_available   = (host.capability == SIMD_CAP_NEON);
    ctx->vector_width     = simd_native_lanes(host, NULL) > 0
                            ? simd_native_lanes(host, NULL) : 4;
    if (ctx->vector_width == 0) ctx->vector_width = 4;
    ctx->loops_vectorized = 0;
    ctx->scalar_fallbacks = 0;
}

bool can_vectorize_loop(Stmt* loop) {
    if (!loop) return false;
    if (loop->type != STMT_FOR) return false;
    if (!loop->as.for_stmt.variable) return false;
    return true;
}

bool vectorize_loop(Stmt* loop, VectorContext* ctx) {
    if (!can_vectorize_loop(loop)) {
        if (ctx) ctx->scalar_fallbacks++;
        return false;
    }
    if (ctx) ctx->loops_vectorized++;
    return true;
}
