; ===========================================================================
; LLM Runtime - AVX2 NASM Kernels (Optimized)
; Target: x86-64, Windows x64 calling convention
;
; Performance optimizations applied:
;   - 2x loop unrolling on element-wise ops (16 floats/iter, better ILP)
;   - Dual accumulator reductions (hides vaddps/vfmadd latency)
;   - Software prefetching for streaming memory access patterns
;   - 6x16 GEMM micro-kernel using all 16 YMM registers (12 FMAs/K-iter)
;   - New GEMV and dot product kernels for attention/inference
;   - Fixed GELU scalar remainder bug
; ===========================================================================

; ===========================================================================
; CONSTANTS (consolidated)
; ===========================================================================
section .rodata
align 32
gelu_const_1702:    times 8 dd 1.702
gelu_const_half:    times 8 dd 0.5
gelu_const_one:     times 8 dd 1.0
const_three:        times 8 dd 3.0
exp_const_log2e:    times 8 dd 1.4426950408889634  ; log2(e)
exp_const_ln2:      times 8 dd 0.6931471805599453  ; ln(2)
exp_const_c1:       times 8 dd 1.0
exp_const_c2:       times 8 dd 0.5
exp_const_c3:       times 8 dd 0.16666666666666666 ; 1/6
exp_const_c4:       times 8 dd 0.041666666666666664 ; 1/24
exp_clamp_hi:       times 8 dd 88.0
exp_clamp_lo:       times 8 dd -88.0
magic_bias:         times 8 dd 12582912.0          ; 2^23 + 2^22
magic_exp_127:      times 8 dd 1065353216          ; 127 << 23

section .text

; ===========================================================================
;                   FORWARD PASS - ELEMENT-WISE OPERATIONS
; ===========================================================================

; ===========================================================================
; void vec_add_f32_avx2(const f32* a, const f32* b, f32* out, i32 n)
; Windows x64: RCX=a, RDX=b, R8=out, R9=n
;
; 2x unrolled: processes 16 floats per iteration for better ILP.
; ===========================================================================
global vec_add_f32_avx2
vec_add_f32_avx2:
    xor rax, rax
    mov r10, r9
    shr r10, 4                      ; n / 16
    test r10, r10
    jz .vec8

.loop16:
    vmovups ymm0, [rcx + rax*4]
    vmovups ymm2, [rcx + rax*4 + 32]
    vmovups ymm1, [rdx + rax*4]
    vmovups ymm3, [rdx + rax*4 + 32]
    vaddps  ymm0, ymm0, ymm1
    vaddps  ymm2, ymm2, ymm3
    vmovups [r8 + rax*4], ymm0
    vmovups [r8 + rax*4 + 32], ymm2
    add rax, 16
    dec r10
    jnz .loop16

.vec8:
    ; Handle 8-element chunk if present
    lea r10, [rax + 8]
    cmp r10, r9
    jg .scalar

    vmovups ymm0, [rcx + rax*4]
    vmovups ymm1, [rdx + rax*4]
    vaddps  ymm0, ymm0, ymm1
    vmovups [r8 + rax*4], ymm0
    add rax, 8

.scalar:
    cmp rax, r9
    jge .done
    movss xmm0, [rcx + rax*4]
    movss xmm1, [rdx + rax*4]
    addss xmm0, xmm1
    movss [r8 + rax*4], xmm0
    inc rax
    jmp .scalar

.done:
    vzeroupper
    ret

; ===========================================================================
; void vec_mul_f32_avx2(const f32* a, const f32* b, f32* out, i32 n)
; 2x unrolled element-wise multiply.
; ===========================================================================
global vec_mul_f32_avx2
vec_mul_f32_avx2:
    xor rax, rax
    mov r10, r9
    shr r10, 4
    test r10, r10
    jz .vec8

.loop16:
    vmovups ymm0, [rcx + rax*4]
    vmovups ymm2, [rcx + rax*4 + 32]
    vmovups ymm1, [rdx + rax*4]
    vmovups ymm3, [rdx + rax*4 + 32]
    vmulps  ymm0, ymm0, ymm1
    vmulps  ymm2, ymm2, ymm3
    vmovups [r8 + rax*4], ymm0
    vmovups [r8 + rax*4 + 32], ymm2
    add rax, 16
    dec r10
    jnz .loop16

.vec8:
    lea r10, [rax + 8]
    cmp r10, r9
    jg .scalar

    vmovups ymm0, [rcx + rax*4]
    vmovups ymm1, [rdx + rax*4]
    vmulps  ymm0, ymm0, ymm1
    vmovups [r8 + rax*4], ymm0
    add rax, 8

.scalar:
    cmp rax, r9
    jge .done
    movss xmm0, [rcx + rax*4]
    movss xmm1, [rdx + rax*4]
    mulss xmm0, xmm1
    movss [r8 + rax*4], xmm0
    inc rax
    jmp .scalar

.done:
    vzeroupper
    ret

; ===========================================================================
; void vec_fma_f32_avx2(const f32* a, const f32* b, const f32* c, f32* out, i32 n)
; Windows x64: RCX=a, RDX=b, R8=c, R9=out, [rsp+40]=n
; FMA: out[i] = a[i] * b[i] + c[i]
; 2x unrolled.
; ===========================================================================
global vec_fma_f32_avx2
vec_fma_f32_avx2:
    push rbx
    mov rbx, [rsp + 48]             ; n (push + 40)
    xor rax, rax
    mov r10, rbx
    shr r10, 4                       ; n / 16
    test r10, r10
    jz .vec8

.loop16:
    vmovups ymm0, [rcx + rax*4]
    vmovups ymm1, [rdx + rax*4]
    vmovups ymm2, [r8 + rax*4]
    vfmadd231ps ymm2, ymm0, ymm1    ; c + a*b
    vmovups [r9 + rax*4], ymm2

    vmovups ymm3, [rcx + rax*4 + 32]
    vmovups ymm4, [rdx + rax*4 + 32]
    vmovups ymm5, [r8 + rax*4 + 32]
    vfmadd231ps ymm5, ymm3, ymm4
    vmovups [r9 + rax*4 + 32], ymm5

    add rax, 16
    dec r10
    jnz .loop16

.vec8:
    lea r10, [rax + 8]
    cmp r10, rbx
    jg .scalar

    vmovups ymm0, [rcx + rax*4]
    vmovups ymm1, [rdx + rax*4]
    vmovups ymm2, [r8 + rax*4]
    vfmadd231ps ymm2, ymm0, ymm1
    vmovups [r9 + rax*4], ymm2
    add rax, 8

.scalar:
    cmp rax, rbx
    jge .done
    movss xmm0, [rcx + rax*4]
    movss xmm1, [rdx + rax*4]
    movss xmm2, [r8 + rax*4]
    mulss xmm0, xmm1
    addss xmm2, xmm0
    movss [r9 + rax*4], xmm2
    inc rax
    jmp .scalar

.done:
    vzeroupper
    pop rbx
    ret

; ===========================================================================
; void relu_f32_avx2(const f32* x, f32* out, i32 n)
; ReLU: out[i] = max(0, x[i])
; 2x unrolled.
; ===========================================================================
global relu_f32_avx2
relu_f32_avx2:
    xor rax, rax
    mov r10, r8
    shr r10, 4
    vxorps ymm5, ymm5, ymm5         ; Zero vector

    test r10, r10
    jz .vec8

.loop16:
    vmovups ymm0, [rcx + rax*4]
    vmovups ymm2, [rcx + rax*4 + 32]
    vmaxps  ymm1, ymm0, ymm5
    vmaxps  ymm3, ymm2, ymm5
    vmovups [rdx + rax*4], ymm1
    vmovups [rdx + rax*4 + 32], ymm3
    add rax, 16
    dec r10
    jnz .loop16

.vec8:
    lea r10, [rax + 8]
    cmp r10, r8
    jg .scalar

    vmovups ymm0, [rcx + rax*4]
    vmaxps  ymm1, ymm0, ymm5
    vmovups [rdx + rax*4], ymm1
    add rax, 8

.scalar:
    cmp rax, r8
    jge .done
    movss xmm0, [rcx + rax*4]
    maxss xmm0, xmm5
    movss [rdx + rax*4], xmm0
    inc rax
    jmp .scalar

.done:
    vzeroupper
    ret

; ===========================================================================
;                    FORWARD PASS - REDUCTIONS
; ===========================================================================

; ===========================================================================
; f32 vec_sum_f32_avx2(const f32* x, i32 n)
; Sum reduction with dual accumulators to hide vaddps latency.
; Return: XMM0
; ===========================================================================
global vec_sum_f32_avx2
vec_sum_f32_avx2:
    vxorps ymm0, ymm0, ymm0         ; Accumulator 0
    vxorps ymm1, ymm1, ymm1         ; Accumulator 1
    xor rax, rax
    mov r10, rdx
    shr r10, 4                       ; n / 16

    test r10, r10
    jz .vec8

.loop16:
    vaddps  ymm0, ymm0, [rcx + rax*4]
    vaddps  ymm1, ymm1, [rcx + rax*4 + 32]
    add rax, 16
    dec r10
    jnz .loop16

    vaddps ymm0, ymm0, ymm1         ; Merge accumulators

.vec8:
    lea r10, [rax + 8]
    cmp r10, rdx
    jg .scalar

    vaddps ymm0, ymm0, [rcx + rax*4]
    add rax, 8

.scalar:
    cmp rax, rdx
    jge .hsum
    movss xmm1, [rcx + rax*4]
    addss xmm0, xmm1
    inc rax
    jmp .scalar

.hsum:
    ; Horizontal sum of YMM0
    vextractf128 xmm1, ymm0, 1
    vaddps xmm0, xmm0, xmm1
    vhaddps xmm0, xmm0, xmm0
    vhaddps xmm0, xmm0, xmm0
    vzeroupper
    ret

; ===========================================================================
; void vec_scale_f32_avx2(const f32* a, f32 scalar, f32* out, i32 n)
; Windows x64: RCX=a, XMM1=scalar, R8=out, R9=n
; 2x unrolled.
; ===========================================================================
global vec_scale_f32_avx2
vec_scale_f32_avx2:
    vbroadcastss ymm1, xmm1
    xor rax, rax
    mov r10, r9
    shr r10, 4
    test r10, r10
    jz .vec8

.loop16:
    vmovups ymm0, [rcx + rax*4]
    vmovups ymm2, [rcx + rax*4 + 32]
    vmulps  ymm0, ymm0, ymm1
    vmulps  ymm2, ymm2, ymm1
    vmovups [r8 + rax*4], ymm0
    vmovups [r8 + rax*4 + 32], ymm2
    add rax, 16
    dec r10
    jnz .loop16

.vec8:
    lea r10, [rax + 8]
    cmp r10, r9
    jg .scalar

    vmovups ymm0, [rcx + rax*4]
    vmulps  ymm0, ymm0, ymm1
    vmovups [r8 + rax*4], ymm0
    add rax, 8

.scalar:
    cmp rax, r9
    jge .done
    movss xmm0, [rcx + rax*4]
    mulss xmm0, xmm1
    movss [r8 + rax*4], xmm0
    inc rax
    jmp .scalar

.done:
    vzeroupper
    ret

; ===========================================================================
; void vec_add_scalar_f32_avx2(const f32* a, f32 scalar, f32* out, i32 n)
; 2x unrolled.
; ===========================================================================
global vec_add_scalar_f32_avx2
vec_add_scalar_f32_avx2:
    vbroadcastss ymm1, xmm1
    xor rax, rax
    mov r10, r9
    shr r10, 4
    test r10, r10
    jz .vec8

.loop16:
    vmovups ymm0, [rcx + rax*4]
    vmovups ymm2, [rcx + rax*4 + 32]
    vaddps  ymm0, ymm0, ymm1
    vaddps  ymm2, ymm2, ymm1
    vmovups [r8 + rax*4], ymm0
    vmovups [r8 + rax*4 + 32], ymm2
    add rax, 16
    dec r10
    jnz .loop16

.vec8:
    lea r10, [rax + 8]
    cmp r10, r9
    jg .scalar

    vmovups ymm0, [rcx + rax*4]
    vaddps  ymm0, ymm0, ymm1
    vmovups [r8 + rax*4], ymm0
    add rax, 8

.scalar:
    cmp rax, r9
    jge .done
    movss xmm0, [rcx + rax*4]
    addss xmm0, xmm1
    movss [r8 + rax*4], xmm0
    inc rax
    jmp .scalar

.done:
    vzeroupper
    ret

; ===========================================================================
; f32 vec_max_f32_avx2(const f32* x, i32 n)
; Max reduction with dual tracking for pipelining.
; ===========================================================================
global vec_max_f32_avx2
vec_max_f32_avx2:
    vbroadcastss ymm0, [rcx]        ; Initialize max with first element
    vmovaps ymm1, ymm0              ; Second max tracker
    xor rax, rax
    mov r10, rdx
    shr r10, 4

    test r10, r10
    jz .vec8

.loop16:
    vmaxps  ymm0, ymm0, [rcx + rax*4]
    vmaxps  ymm1, ymm1, [rcx + rax*4 + 32]
    add rax, 16
    dec r10
    jnz .loop16

    vmaxps ymm0, ymm0, ymm1         ; Merge

.vec8:
    lea r10, [rax + 8]
    cmp r10, rdx
    jg .scalar

    vmaxps ymm0, ymm0, [rcx + rax*4]
    add rax, 8

.scalar:
    cmp rax, rdx
    jge .hmax
    movss xmm1, [rcx + rax*4]
    maxss xmm0, xmm1
    inc rax
    jmp .scalar

.hmax:
    vextractf128 xmm1, ymm0, 1
    vmaxps xmm0, xmm0, xmm1
    vshufps xmm1, xmm0, xmm0, 0x4E
    vmaxps xmm0, xmm0, xmm1
    vshufps xmm1, xmm0, xmm0, 0xB1
    vmaxps xmm0, xmm0, xmm1
    vzeroupper
    ret

; ===========================================================================
; f32 vec_mean_f32_avx2(const f32* x, i32 n)
; Mean = sum(x) / n, using dual accumulator sum.
; ===========================================================================
global vec_mean_f32_avx2
vec_mean_f32_avx2:
    push rdx                        ; Save n

    vxorps ymm0, ymm0, ymm0
    vxorps ymm1, ymm1, ymm1
    xor rax, rax
    mov r10, rdx
    shr r10, 4

    test r10, r10
    jz .vec8

.loop16:
    vaddps  ymm0, ymm0, [rcx + rax*4]
    vaddps  ymm1, ymm1, [rcx + rax*4 + 32]
    add rax, 16
    dec r10
    jnz .loop16

    vaddps ymm0, ymm0, ymm1

.vec8:
    pop r11                         ; Restore n
    push r11
    lea r10, [rax + 8]
    cmp r10, r11
    jg .scalar

    vaddps ymm0, ymm0, [rcx + rax*4]
    add rax, 8

.scalar:
    pop r11
    push r11
    cmp rax, r11
    jge .hsum
    movss xmm1, [rcx + rax*4]
    addss xmm0, xmm1
    inc rax
    jmp .scalar

.hsum:
    vextractf128 xmm1, ymm0, 1
    vaddps xmm0, xmm0, xmm1
    vhaddps xmm0, xmm0, xmm0
    vhaddps xmm0, xmm0, xmm0

    pop rax                         ; n
    cvtsi2ss xmm1, eax
    divss xmm0, xmm1
    vzeroupper
    ret

; ===========================================================================
;                  FORWARD PASS - ACTIVATION FUNCTIONS
; ===========================================================================

; ===========================================================================
; void gelu_f32_avx2(const f32* x, f32* out, i32 n)
; GELU using fast sigmoid approximation: gelu(x) = x * sigmoid(1.702 * x)
;
; FIXED: scalar remainder now correctly iterates all remaining elements.
; ===========================================================================
global gelu_f32_avx2
gelu_f32_avx2:
    push rbx
    push r12
    push r13

    mov r12, rcx                    ; x
    mov r13, rdx                    ; out
    mov rbx, r8                     ; n

    xor rax, rax
    mov r10, rbx
    shr r10, 3

    ; Load constants
    vmovaps ymm6, [rel gelu_const_1702]
    vmovaps ymm7, [rel gelu_const_one]

.loop_vec:
    test r10, r10
    jz .remainder

    vmovups ymm0, [r12 + rax*4]     ; x
    vmulps  ymm1, ymm0, ymm6        ; 1.702 * x

    ; Clamp for numerical stability
    vmovaps ymm2, [rel exp_clamp_lo]
    vmaxps  ymm1, ymm1, ymm2
    vmovaps ymm2, [rel exp_clamp_hi]
    vminps  ymm1, ymm1, ymm2

    ; Fast exp: exp(x) = 2^(x * log2(e))
    vmovaps ymm2, [rel exp_const_log2e]
    vmulps  ymm3, ymm1, ymm2        ; t = x * log2(e)

    ; Split into integer and fractional parts
    vroundps ymm4, ymm3, 1          ; floor(t) = n
    vsubps  ymm3, ymm3, ymm4        ; f = t - n (fractional)

    ; 2^f polynomial: 1 + f + f^2/2 + f^3/6 + f^4/24
    vmovaps ymm5, [rel exp_const_c4]
    vfmadd213ps ymm5, ymm3, [rel exp_const_c3]
    vfmadd213ps ymm5, ymm3, [rel exp_const_c2]
    vfmadd213ps ymm5, ymm3, [rel exp_const_c1]
    vfmadd213ps ymm5, ymm3, ymm7

    ; 2^n via integer exponent manipulation
    vcvtps2dq ymm4, ymm4
    vpslld  ymm4, ymm4, 23
    vpaddd  ymm4, ymm4, [rel magic_exp_127]
    vmulps  ymm3, ymm5, ymm4        ; exp(1.702*x)

    ; sigmoid = exp(x) / (1 + exp(x))
    vaddps  ymm4, ymm3, ymm7
    vdivps  ymm3, ymm3, ymm4

    ; GELU = x * sigmoid(1.702*x)
    vmulps  ymm0, ymm0, ymm3
    vmovups [r13 + rax*4], ymm0

    add rax, 8
    dec r10
    jmp .loop_vec

.remainder:
    ; Scalar fallback for remaining elements (FIXED: proper loop)
    cmp rax, rbx
    jge .done

    movss xmm0, [r12 + rax*4]       ; x
    movss xmm1, [rel gelu_const_1702]
    mulss xmm1, xmm0                ; 1.702 * x

    ; Clamp
    movss xmm2, [rel exp_clamp_lo]
    maxss xmm1, xmm2
    movss xmm2, [rel exp_clamp_hi]
    minss xmm1, xmm2

    ; exp approximation
    movss xmm2, [rel exp_const_log2e]
    movaps xmm3, xmm1
    mulss xmm3, xmm2
    roundss xmm4, xmm3, 1
    subss xmm3, xmm4

    movss xmm5, [rel exp_const_c4]
    mulss xmm5, xmm3
    addss xmm5, [rel exp_const_c3]
    mulss xmm5, xmm3
    addss xmm5, [rel exp_const_c2]
    mulss xmm5, xmm3
    addss xmm5, [rel exp_const_c1]
    mulss xmm5, xmm3
    movss xmm6, [rel gelu_const_one]
    addss xmm5, xmm6

    ; 2^n
    cvtss2si r10d, xmm4
    add r10d, 127
    shl r10d, 23
    movd xmm4, r10d
    mulss xmm5, xmm4                ; exp(1.702*x)

    ; Sigmoid and GELU
    movss xmm6, [rel gelu_const_one]
    addss xmm6, xmm5                ; 1 + exp
    divss xmm5, xmm6                ; sigmoid
    mulss xmm0, xmm5                ; x * sigmoid

    movss [r13 + rax*4], xmm0

    inc rax
    jmp .remainder                   ; Loop back (was bug: jumped to end)

.done:
    pop r13
    pop r12
    pop rbx
    vzeroupper
    ret

; ===========================================================================
; void silu_f32_avx2(const f32* x, f32* out, i32 n)
; SiLU(x) = x * sigmoid(x)
; ===========================================================================
global silu_f32_avx2
silu_f32_avx2:
    push rbx
    push r12
    push r13

    mov r12, rcx
    mov r13, rdx
    mov rbx, r8

    xor rax, rax
    mov r10, rbx
    shr r10, 3

    vmovaps ymm14, [rel exp_const_log2e]
    vmovaps ymm15, [rel gelu_const_one]

.silu_loop:
    test r10, r10
    jz .silu_remainder

    vmovups ymm0, [r12 + rax*4]     ; x

    ; Clamp for stability
    vmovaps ymm1, [rel exp_clamp_lo]
    vmaxps ymm2, ymm0, ymm1
    vmovaps ymm1, [rel exp_clamp_hi]
    vminps ymm2, ymm2, ymm1

    ; exp(x)
    vmulps ymm3, ymm2, ymm14
    vroundps ymm4, ymm3, 1
    vsubps ymm3, ymm3, ymm4

    vmovaps ymm5, [rel exp_const_c4]
    vfmadd213ps ymm5, ymm3, [rel exp_const_c3]
    vfmadd213ps ymm5, ymm3, [rel exp_const_c2]
    vfmadd213ps ymm5, ymm3, [rel exp_const_c1]
    vfmadd213ps ymm5, ymm3, ymm15

    vcvtps2dq ymm4, ymm4
    vpslld ymm4, ymm4, 23
    vpaddd ymm4, ymm4, [rel magic_exp_127]
    vmulps ymm5, ymm5, ymm4         ; exp(x)

    ; sigmoid = exp(x) / (1 + exp(x))
    vaddps ymm6, ymm5, ymm15
    vdivps ymm5, ymm5, ymm6

    ; SiLU = x * sigmoid(x)
    vmulps ymm0, ymm0, ymm5
    vmovups [r13 + rax*4], ymm0

    add rax, 8
    dec r10
    jmp .silu_loop

.silu_remainder:
    cmp rax, rbx
    jge .silu_done

    ; Scalar SiLU
    movss xmm0, [r12 + rax*4]

    movss xmm1, [rel exp_clamp_lo]
    movaps xmm2, xmm0
    maxss xmm2, xmm1
    movss xmm1, [rel exp_clamp_hi]
    minss xmm2, xmm1

    movss xmm3, [rel exp_const_log2e]
    mulss xmm3, xmm2
    roundss xmm4, xmm3, 1
    subss xmm3, xmm4

    movss xmm5, [rel exp_const_c4]
    mulss xmm5, xmm3
    addss xmm5, [rel exp_const_c3]
    mulss xmm5, xmm3
    addss xmm5, [rel exp_const_c2]
    mulss xmm5, xmm3
    addss xmm5, [rel exp_const_c1]
    mulss xmm5, xmm3
    addss xmm5, xmm15

    cvtss2si r10d, xmm4
    add r10d, 127
    shl r10d, 23
    movd xmm4, r10d
    mulss xmm5, xmm4

    movss xmm6, xmm15
    addss xmm6, xmm5
    divss xmm5, xmm6
    mulss xmm0, xmm5
    movss [r13 + rax*4], xmm0

    inc rax
    jmp .silu_remainder

.silu_done:
    pop r13
    pop r12
    pop rbx
    vzeroupper
    ret

; ===========================================================================
;                  FORWARD PASS - MATRIX OPERATIONS
; ===========================================================================

; ===========================================================================
; GEMM MICRO-KERNEL 4x4 (kept for small tile fallback)
; void gemm_kernel_4x4_avx2(const f32* A, const f32* B, f32* C,
;                           i32 K, i32 lda, i32 ldb, i32 ldc)
;
; Windows x64: RCX=A, RDX=B, R8=C, R9=K
; Stack: [rsp+40]=lda, [rsp+48]=ldb, [rsp+56]=ldc
; ===========================================================================
global gemm_kernel_4x4_avx2
gemm_kernel_4x4_avx2:
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rsi
    push rdi

    mov rsi, rcx                    ; A
    mov rdi, rdx                    ; B
    mov r10, r8                     ; C
    mov r11, r9                     ; K

    mov r12, [rsp + 96]             ; lda (7 pushes * 8 + 40)
    mov r13, [rsp + 104]            ; ldb
    mov r14, [rsp + 112]            ; ldc

    shl r12, 2
    shl r13, 2
    shl r14, 2

    vxorps xmm8, xmm8, xmm8
    vxorps xmm9, xmm9, xmm9
    vxorps xmm10, xmm10, xmm10
    vxorps xmm11, xmm11, xmm11

    test r11, r11
    jz .store_result

.k_loop:
    vmovups xmm0, [rdi]

    vbroadcastss xmm1, [rsi]
    vfmadd231ps xmm8, xmm1, xmm0

    vbroadcastss xmm1, [rsi + r12]
    vfmadd231ps xmm9, xmm1, xmm0

    lea rax, [rsi + r12*2]
    vbroadcastss xmm1, [rax]
    vfmadd231ps xmm10, xmm1, xmm0

    add rax, r12
    vbroadcastss xmm1, [rax]
    vfmadd231ps xmm11, xmm1, xmm0

    add rsi, 4
    add rdi, r13

    dec r11
    jnz .k_loop

.store_result:
    vmovups xmm0, [r10]
    vaddps xmm8, xmm8, xmm0
    vmovups [r10], xmm8

    add r10, r14
    vmovups xmm0, [r10]
    vaddps xmm9, xmm9, xmm0
    vmovups [r10], xmm9

    add r10, r14
    vmovups xmm0, [r10]
    vaddps xmm10, xmm10, xmm0
    vmovups [r10], xmm10

    add r10, r14
    vmovups xmm0, [r10]
    vaddps xmm11, xmm11, xmm0
    vmovups [r10], xmm11

    pop rdi
    pop rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    vzeroupper
    ret

; ===========================================================================
; GEMM MICRO-KERNEL 6x16 - HIGH PERFORMANCE
; void gemm_kernel_6x16_avx2(const f32* A, const f32* B, f32* C,
;                             i32 K, i32 lda, i32 ldb, i32 ldc)
;
; Computes C[6,16] += A[6,K] * B[K,16]
; Uses ALL 16 YMM registers:
;   YMM0 -YMM5:  C[row 0..5, cols 0:8]   (left half accumulators)
;   YMM6 -YMM11: C[row 0..5, cols 8:16]  (right half accumulators)
;   YMM12:        B[k, 0:8]
;   YMM13:        B[k, 8:16]
;   YMM14:        broadcast A[row, k]
;   YMM15:        (temp)
;
; This yields 12 FMAs per K-iteration (96 FLOPs), maximizing compute density.
;
; Windows x64: RCX=A, RDX=B, R8=C, R9=K
; Stack: [rsp+40]=lda, [rsp+48]=ldb, [rsp+56]=ldc
; ===========================================================================
global gemm_kernel_6x16_avx2
gemm_kernel_6x16_avx2:
    ; Save callee-saved integer registers
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    ; 7 pushes = 56 bytes

    ; Save callee-saved XMM registers (Windows x64: XMM6-XMM15)
    ; We use YMM6-YMM11 as accumulators, must save XMM6-XMM11
    sub rsp, 112                     ; 6 * 16 = 96 + 16 alignment pad
    vmovaps [rsp],      xmm6
    vmovaps [rsp + 16], xmm7
    vmovaps [rsp + 32], xmm8
    vmovaps [rsp + 48], xmm9
    vmovaps [rsp + 64], xmm10
    vmovaps [rsp + 80], xmm11

    ; Load parameters
    mov rsi, rcx                     ; A
    mov rdi, rdx                     ; B
    mov r10, r8                      ; C
    mov r11, r9                      ; K

    ; Stack params at: rsp + 112 (sub) + 56 (pushes) + 8 (ret) + 32 (shadow)
    ; = rsp + 208 for first stack param
    mov r12, [rsp + 208]             ; lda
    mov r13, [rsp + 216]             ; ldb
    mov r14, [rsp + 224]             ; ldc

    ; Convert strides from elements to bytes
    shl r12, 2
    shl r13, 2
    shl r14, 2

    ; Initialize 12 accumulators to zero
    vxorps ymm0,  ymm0,  ymm0
    vxorps ymm1,  ymm1,  ymm1
    vxorps ymm2,  ymm2,  ymm2
    vxorps ymm3,  ymm3,  ymm3
    vxorps ymm4,  ymm4,  ymm4
    vxorps ymm5,  ymm5,  ymm5
    vxorps ymm6,  ymm6,  ymm6
    vxorps ymm7,  ymm7,  ymm7
    vxorps ymm8,  ymm8,  ymm8
    vxorps ymm9,  ymm9,  ymm9
    vxorps ymm10, ymm10, ymm10
    vxorps ymm11, ymm11, ymm11

    test r11, r11
    jz .store_6x16

    ; Precompute row offsets for A
    ; Row 0: rsi
    ; Row 1: rsi + r12
    ; Row 2: rsi + 2*r12
    ; Row 3: rsi + 3*r12
    ; Row 4: rsi + 4*r12
    ; Row 5: rsi + 5*r12

.k_loop_6x16:
    ; Prefetch next B row for next iteration
    prefetcht0 [rdi + r13]
    prefetcht0 [rdi + r13 + 32]

    ; Load B[k, 0:16] (16 floats = 2 YMM registers)
    vmovups ymm12, [rdi]
    vmovups ymm13, [rdi + 32]

    ; Row 0: C[0,:] += A[0,k] * B[k,:]
    vbroadcastss ymm14, [rsi]
    vfmadd231ps ymm0, ymm14, ymm12
    vfmadd231ps ymm6, ymm14, ymm13

    ; Row 1: C[1,:] += A[1,k] * B[k,:]
    vbroadcastss ymm14, [rsi + r12]
    vfmadd231ps ymm1, ymm14, ymm12
    vfmadd231ps ymm7, ymm14, ymm13

    ; Row 2: C[2,:] += A[2,k] * B[k,:]
    lea rbx, [rsi + r12*2]
    vbroadcastss ymm14, [rbx]
    vfmadd231ps ymm2, ymm14, ymm12
    vfmadd231ps ymm8, ymm14, ymm13

    ; Row 3: C[3,:] += A[3,k] * B[k,:]
    add rbx, r12
    vbroadcastss ymm14, [rbx]
    vfmadd231ps ymm3, ymm14, ymm12
    vfmadd231ps ymm9, ymm14, ymm13

    ; Row 4: C[4,:] += A[4,k] * B[k,:]
    add rbx, r12
    vbroadcastss ymm14, [rbx]
    vfmadd231ps ymm4, ymm14, ymm12
    vfmadd231ps ymm10, ymm14, ymm13

    ; Row 5: C[5,:] += A[5,k] * B[k,:]
    add rbx, r12
    vbroadcastss ymm14, [rbx]
    vfmadd231ps ymm5, ymm14, ymm12
    vfmadd231ps ymm11, ymm14, ymm13

    ; Advance: A to next column, B to next row
    add rsi, 4
    add rdi, r13

    dec r11
    jnz .k_loop_6x16

.store_6x16:
    ; Accumulate into existing C values and store
    ; Row 0
    vaddps ymm0, ymm0, [r10]
    vaddps ymm6, ymm6, [r10 + 32]
    vmovups [r10], ymm0
    vmovups [r10 + 32], ymm6

    ; Row 1
    add r10, r14
    vaddps ymm1, ymm1, [r10]
    vaddps ymm7, ymm7, [r10 + 32]
    vmovups [r10], ymm1
    vmovups [r10 + 32], ymm7

    ; Row 2
    add r10, r14
    vaddps ymm2, ymm2, [r10]
    vaddps ymm8, ymm8, [r10 + 32]
    vmovups [r10], ymm2
    vmovups [r10 + 32], ymm8

    ; Row 3
    add r10, r14
    vaddps ymm3, ymm3, [r10]
    vaddps ymm9, ymm9, [r10 + 32]
    vmovups [r10], ymm3
    vmovups [r10 + 32], ymm9

    ; Row 4
    add r10, r14
    vaddps ymm4, ymm4, [r10]
    vaddps ymm10, ymm10, [r10 + 32]
    vmovups [r10], ymm4
    vmovups [r10 + 32], ymm10

    ; Row 5
    add r10, r14
    vaddps ymm5, ymm5, [r10]
    vaddps ymm11, ymm11, [r10 + 32]
    vmovups [r10], ymm5
    vmovups [r10 + 32], ymm11

    ; Restore callee-saved XMM registers
    vmovaps xmm6,  [rsp]
    vmovaps xmm7,  [rsp + 16]
    vmovaps xmm8,  [rsp + 32]
    vmovaps xmm9,  [rsp + 48]
    vmovaps xmm10, [rsp + 64]
    vmovaps xmm11, [rsp + 80]
    add rsp, 112

    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    vzeroupper
    ret

; ===========================================================================
; GEMV - Matrix-Vector Multiply
; void gemv_f32_avx2(const f32* A, const f32* x, f32* y, i32 M, i32 N)
;
; Computes y[i] = sum_j(A[i,j] * x[j]) for i in [0,M)
; A is row-major [M, N], x is [N], y is [M]
;
; Windows x64: RCX=A, RDX=x, R8=y, R9=M, [rsp+40]=N
; ===========================================================================
global gemv_f32_avx2
gemv_f32_avx2:
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov r12, rcx                     ; A
    mov r13, rdx                     ; x
    mov r14, r8                      ; y
    mov rbx, r9                      ; M
    mov r15, [rsp + 80]              ; N (5 pushes * 8 + 40)

    xor r10, r10                     ; row index

.row_loop:
    cmp r10, rbx
    jge .gemv_done

    ; Dot product: A[row, :] . x[:]
    vxorps ymm0, ymm0, ymm0         ; accumulator 0
    vxorps ymm1, ymm1, ymm1         ; accumulator 1
    xor rax, rax
    mov r11, r15
    shr r11, 4                       ; N / 16

    test r11, r11
    jz .dot_vec8

.dot_loop16:
    vmovups ymm2, [r12 + rax*4]
    vmovups ymm3, [r12 + rax*4 + 32]
    vfmadd231ps ymm0, ymm2, [r13 + rax*4]
    vfmadd231ps ymm1, ymm3, [r13 + rax*4 + 32]
    add rax, 16
    dec r11
    jnz .dot_loop16

    vaddps ymm0, ymm0, ymm1

.dot_vec8:
    lea r11, [rax + 8]
    cmp r11, r15
    jg .dot_scalar

    vmovups ymm2, [r12 + rax*4]
    vfmadd231ps ymm0, ymm2, [r13 + rax*4]
    add rax, 8

.dot_scalar:
    cmp rax, r15
    jge .dot_reduce

    movss xmm1, [r12 + rax*4]
    movss xmm2, [r13 + rax*4]
    mulss xmm1, xmm2
    addss xmm0, xmm1
    inc rax
    jmp .dot_scalar

.dot_reduce:
    ; Horizontal sum
    vextractf128 xmm1, ymm0, 1
    vaddps xmm0, xmm0, xmm1
    vhaddps xmm0, xmm0, xmm0
    vhaddps xmm0, xmm0, xmm0

    movss [r14 + r10*4], xmm0       ; y[row] = result

    ; Advance A to next row: A += N * 4 bytes
    lea rax, [r15*4]
    add r12, rax
    inc r10
    jmp .row_loop

.gemv_done:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    vzeroupper
    ret

; ===========================================================================
; DOT PRODUCT
; f32 vec_dot_f32_avx2(const f32* a, const f32* b, i32 n)
;
; Computes sum(a[i] * b[i]) using dual accumulators.
; Windows x64: RCX=a, RDX=b, R8=n
; Return: XMM0
; ===========================================================================
global vec_dot_f32_avx2
vec_dot_f32_avx2:
    vxorps ymm0, ymm0, ymm0         ; Accumulator 0
    vxorps ymm1, ymm1, ymm1         ; Accumulator 1
    xor rax, rax
    mov r10, r8
    shr r10, 4                       ; n / 16

    test r10, r10
    jz .dot_vec8

.dot_loop16:
    vmovups ymm2, [rcx + rax*4]
    vmovups ymm3, [rcx + rax*4 + 32]
    vfmadd231ps ymm0, ymm2, [rdx + rax*4]
    vfmadd231ps ymm1, ymm3, [rdx + rax*4 + 32]
    add rax, 16
    dec r10
    jnz .dot_loop16

    vaddps ymm0, ymm0, ymm1

.dot_vec8:
    lea r10, [rax + 8]
    cmp r10, r8
    jg .dot_scalar

    vmovups ymm2, [rcx + rax*4]
    vfmadd231ps ymm0, ymm2, [rdx + rax*4]
    add rax, 8

.dot_scalar:
    cmp rax, r8
    jge .dot_hsum

    movss xmm1, [rcx + rax*4]
    movss xmm2, [rdx + rax*4]
    mulss xmm1, xmm2
    addss xmm0, xmm1
    inc rax
    jmp .dot_scalar

.dot_hsum:
    vextractf128 xmm1, ymm0, 1
    vaddps xmm0, xmm0, xmm1
    vhaddps xmm0, xmm0, xmm0
    vhaddps xmm0, xmm0, xmm0
    vzeroupper
    ret

; ===========================================================================
;                  FORWARD PASS - NORMALIZATION
; ===========================================================================

; ===========================================================================
; SOFTMAX - Numerically Stable Per-Row
; void softmax_row_avx2(const f32* x, f32* out, i32 dim)
;
; out[i] = exp(x[i] - max(x)) / sum(exp(x[i] - max(x)))
; ===========================================================================
global softmax_row_avx2
softmax_row_avx2:
    push rbx
    push r12
    push r13

    mov r12, rcx                    ; x
    mov r13, rdx                    ; out
    mov rbx, r8                     ; dim

    ; ---- Step 1: Find max (dual accumulator) ----
    vbroadcastss ymm0, [r12]
    vmovaps ymm1, ymm0
    xor rax, rax
    mov r10, rbx
    shr r10, 4

    test r10, r10
    jz .max_vec8

.max_loop16:
    vmaxps ymm0, ymm0, [r12 + rax*4]
    vmaxps ymm1, ymm1, [r12 + rax*4 + 32]
    add rax, 16
    dec r10
    jnz .max_loop16

    vmaxps ymm0, ymm0, ymm1

.max_vec8:
    lea r10, [rax + 8]
    cmp r10, rbx
    jg .max_scalar

    vmaxps ymm0, ymm0, [r12 + rax*4]
    add rax, 8

.max_scalar:
    cmp rax, rbx
    jge .max_reduce
    movss xmm1, [r12 + rax*4]
    maxss xmm0, xmm1
    inc rax
    jmp .max_scalar

.max_reduce:
    vextractf128 xmm1, ymm0, 1
    vmaxps xmm0, xmm0, xmm1
    vshufps xmm1, xmm0, xmm0, 0x4E
    vmaxps xmm0, xmm0, xmm1
    vshufps xmm1, xmm0, xmm0, 0xB1
    vmaxps xmm0, xmm0, xmm1
    vbroadcastss ymm7, xmm0         ; max in all lanes

    ; ---- Step 2: Compute exp(x - max) and sum ----
    vxorps ymm6, ymm6, ymm6         ; sum accumulator
    xor rax, rax
    mov r10, rbx
    shr r10, 3

    vmovaps ymm14, [rel exp_const_log2e]
    vmovaps ymm15, [rel gelu_const_one]

.exp_loop:
    test r10, r10
    jz .exp_remainder

    vmovups ymm0, [r12 + rax*4]
    vsubps ymm0, ymm0, ymm7         ; x - max

    ; Fast exp
    vmulps ymm1, ymm0, ymm14
    vroundps ymm2, ymm1, 1
    vsubps ymm1, ymm1, ymm2

    vmovaps ymm3, [rel exp_const_c4]
    vfmadd213ps ymm3, ymm1, [rel exp_const_c3]
    vfmadd213ps ymm3, ymm1, [rel exp_const_c2]
    vfmadd213ps ymm3, ymm1, [rel exp_const_c1]
    vfmadd213ps ymm3, ymm1, ymm15

    vcvtps2dq ymm2, ymm2
    vpslld ymm2, ymm2, 23
    vpaddd ymm2, ymm2, [rel magic_exp_127]
    vmulps ymm3, ymm3, ymm2

    vmovups [r13 + rax*4], ymm3
    vaddps ymm6, ymm6, ymm3

    add rax, 8
    dec r10
    jmp .exp_loop

.exp_remainder:
    cmp rax, rbx
    jge .sum_reduce

    movss xmm0, [r12 + rax*4]
    subss xmm0, xmm7

    movss xmm1, [rel exp_const_log2e]
    mulss xmm1, xmm0
    roundss xmm2, xmm1, 1
    subss xmm1, xmm2

    movss xmm3, [rel exp_const_c4]
    mulss xmm3, xmm1
    addss xmm3, [rel exp_const_c3]
    mulss xmm3, xmm1
    addss xmm3, [rel exp_const_c2]
    mulss xmm3, xmm1
    addss xmm3, [rel exp_const_c1]
    mulss xmm3, xmm1
    addss xmm3, xmm15

    cvtss2si r10d, xmm2
    add r10d, 127
    shl r10d, 23
    movd xmm2, r10d
    mulss xmm3, xmm2

    movss [r13 + rax*4], xmm3
    addss xmm6, xmm3

    inc rax
    jmp .exp_remainder

.sum_reduce:
    vextractf128 xmm1, ymm6, 1
    vaddps xmm6, xmm6, xmm1
    vhaddps xmm6, xmm6, xmm6
    vhaddps xmm6, xmm6, xmm6

    ; 1/sum
    movss xmm7, [rel gelu_const_one]
    divss xmm7, xmm6
    vbroadcastss ymm7, xmm7

    ; ---- Step 3: Normalize (2x unrolled) ----
    xor rax, rax
    mov r10, rbx
    shr r10, 4

    test r10, r10
    jz .norm_vec8

.norm_loop16:
    vmovups ymm0, [r13 + rax*4]
    vmovups ymm1, [r13 + rax*4 + 32]
    vmulps ymm0, ymm0, ymm7
    vmulps ymm1, ymm1, ymm7
    vmovups [r13 + rax*4], ymm0
    vmovups [r13 + rax*4 + 32], ymm1
    add rax, 16
    dec r10
    jnz .norm_loop16

.norm_vec8:
    lea r10, [rax + 8]
    cmp r10, rbx
    jg .norm_scalar

    vmovups ymm0, [r13 + rax*4]
    vmulps ymm0, ymm0, ymm7
    vmovups [r13 + rax*4], ymm0
    add rax, 8

.norm_scalar:
    cmp rax, rbx
    jge .softmax_done
    movss xmm0, [r13 + rax*4]
    mulss xmm0, xmm7
    movss [r13 + rax*4], xmm0
    inc rax
    jmp .norm_scalar

.softmax_done:
    pop r13
    pop r12
    pop rbx
    vzeroupper
    ret

; ===========================================================================
; LAYER NORMALIZATION
; void layernorm_row_avx2(const f32* x, const f32* gamma, const f32* beta,
;                         f32* out, i32 dim, f32 eps)
;
; Windows x64: RCX=x, RDX=gamma, R8=beta, R9=out
; Stack: [rsp+40]=dim, XMM5=eps
; ===========================================================================
global layernorm_row_avx2
layernorm_row_avx2:
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov r12, rcx                    ; x
    mov r13, rdx                    ; gamma
    mov r14, r8                     ; beta
    mov r15, r9                     ; out
    mov rbx, [rsp + 80]             ; dim (5 pushes * 8 + 40)
    vmovss xmm15, xmm5              ; eps

    ; ---- Step 1: Compute mean (dual accumulator) ----
    vxorps ymm0, ymm0, ymm0
    vxorps ymm1, ymm1, ymm1
    xor rax, rax
    mov r10, rbx
    shr r10, 4

    test r10, r10
    jz .mean_vec8

.mean_loop16:
    vaddps ymm0, ymm0, [r12 + rax*4]
    vaddps ymm1, ymm1, [r12 + rax*4 + 32]
    add rax, 16
    dec r10
    jnz .mean_loop16

    vaddps ymm0, ymm0, ymm1

.mean_vec8:
    lea r10, [rax + 8]
    cmp r10, rbx
    jg .mean_scalar

    vaddps ymm0, ymm0, [r12 + rax*4]
    add rax, 8

.mean_scalar:
    cmp rax, rbx
    jge .mean_reduce
    movss xmm1, [r12 + rax*4]
    addss xmm0, xmm1
    inc rax
    jmp .mean_scalar

.mean_reduce:
    vextractf128 xmm1, ymm0, 1
    vaddps xmm0, xmm0, xmm1
    vhaddps xmm0, xmm0, xmm0
    vhaddps xmm0, xmm0, xmm0

    cvtsi2ss xmm1, ebx
    divss xmm0, xmm1                ; mean
    vbroadcastss ymm6, xmm0         ; mean in all lanes

    ; ---- Step 2: Compute variance (dual accumulator) ----
    vxorps ymm0, ymm0, ymm0
    vxorps ymm1, ymm1, ymm1
    xor rax, rax
    mov r10, rbx
    shr r10, 4

    test r10, r10
    jz .var_vec8

.var_loop16:
    vmovups ymm2, [r12 + rax*4]
    vsubps ymm2, ymm2, ymm6
    vfmadd231ps ymm0, ymm2, ymm2

    vmovups ymm3, [r12 + rax*4 + 32]
    vsubps ymm3, ymm3, ymm6
    vfmadd231ps ymm1, ymm3, ymm3

    add rax, 16
    dec r10
    jnz .var_loop16

    vaddps ymm0, ymm0, ymm1

.var_vec8:
    lea r10, [rax + 8]
    cmp r10, rbx
    jg .var_scalar

    vmovups ymm2, [r12 + rax*4]
    vsubps ymm2, ymm2, ymm6
    vfmadd231ps ymm0, ymm2, ymm2
    add rax, 8

.var_scalar:
    cmp rax, rbx
    jge .var_reduce
    movss xmm1, [r12 + rax*4]
    subss xmm1, xmm6
    mulss xmm1, xmm1
    addss xmm0, xmm1
    inc rax
    jmp .var_scalar

.var_reduce:
    vextractf128 xmm1, ymm0, 1
    vaddps xmm0, xmm0, xmm1
    vhaddps xmm0, xmm0, xmm0
    vhaddps xmm0, xmm0, xmm0

    cvtsi2ss xmm1, ebx
    divss xmm0, xmm1                ; variance
    addss xmm0, xmm15               ; var + eps
    sqrtss xmm0, xmm0

    movss xmm1, [rel gelu_const_one]
    divss xmm1, xmm0                ; 1/std
    vbroadcastss ymm7, xmm1

    ; ---- Step 3: Normalize with gamma/beta (2x unrolled) ----
    xor rax, rax
    mov r10, rbx
    shr r10, 4

    test r10, r10
    jz .norm_ln_vec8

.norm_ln_loop16:
    ; First 8 elements
    vmovups ymm0, [r12 + rax*4]
    vsubps ymm0, ymm0, ymm6
    vmulps ymm0, ymm0, ymm7
    vmulps ymm0, ymm0, [r13 + rax*4]
    vaddps ymm0, ymm0, [r14 + rax*4]
    vmovups [r15 + rax*4], ymm0

    ; Second 8 elements
    vmovups ymm2, [r12 + rax*4 + 32]
    vsubps ymm2, ymm2, ymm6
    vmulps ymm2, ymm2, ymm7
    vmulps ymm2, ymm2, [r13 + rax*4 + 32]
    vaddps ymm2, ymm2, [r14 + rax*4 + 32]
    vmovups [r15 + rax*4 + 32], ymm2

    add rax, 16
    dec r10
    jnz .norm_ln_loop16

.norm_ln_vec8:
    lea r10, [rax + 8]
    cmp r10, rbx
    jg .norm_ln_scalar

    vmovups ymm0, [r12 + rax*4]
    vsubps ymm0, ymm0, ymm6
    vmulps ymm0, ymm0, ymm7
    vmulps ymm0, ymm0, [r13 + rax*4]
    vaddps ymm0, ymm0, [r14 + rax*4]
    vmovups [r15 + rax*4], ymm0
    add rax, 8

.norm_ln_scalar:
    cmp rax, rbx
    jge .ln_done

    movss xmm0, [r12 + rax*4]
    subss xmm0, xmm6
    mulss xmm0, xmm7
    movss xmm1, [r13 + rax*4]
    mulss xmm0, xmm1
    movss xmm2, [r14 + rax*4]
    addss xmm0, xmm2
    movss [r15 + rax*4], xmm0

    inc rax
    jmp .norm_ln_scalar

.ln_done:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    vzeroupper
    ret

; ===========================================================================
; RMS NORMALIZATION
; void rmsnorm_row_avx2(const f32* x, const f32* gamma, f32* out,
;                       i32 dim, f32 eps)
;
; Windows x64: RCX=x, RDX=gamma, R8=out, R9=dim, XMM4=eps
; ===========================================================================
global rmsnorm_row_avx2
rmsnorm_row_avx2:
    push rbx
    push r12
    push r13
    push r14

    mov r12, rcx                    ; x
    mov r13, rdx                    ; gamma
    mov r14, r8                     ; out
    mov rbx, r9                     ; dim
    vmovss xmm15, xmm4              ; eps

    ; ---- Step 1: Compute mean of squares (dual accumulator) ----
    vxorps ymm0, ymm0, ymm0
    vxorps ymm1, ymm1, ymm1
    xor rax, rax
    mov r10, rbx
    shr r10, 4

    test r10, r10
    jz .rms_sq_vec8

.rms_sq_loop16:
    vmovups ymm2, [r12 + rax*4]
    vfmadd231ps ymm0, ymm2, ymm2

    vmovups ymm3, [r12 + rax*4 + 32]
    vfmadd231ps ymm1, ymm3, ymm3

    add rax, 16
    dec r10
    jnz .rms_sq_loop16

    vaddps ymm0, ymm0, ymm1

.rms_sq_vec8:
    lea r10, [rax + 8]
    cmp r10, rbx
    jg .rms_sq_scalar

    vmovups ymm2, [r12 + rax*4]
    vfmadd231ps ymm0, ymm2, ymm2
    add rax, 8

.rms_sq_scalar:
    cmp rax, rbx
    jge .rms_reduce
    movss xmm1, [r12 + rax*4]
    mulss xmm1, xmm1
    addss xmm0, xmm1
    inc rax
    jmp .rms_sq_scalar

.rms_reduce:
    vextractf128 xmm1, ymm0, 1
    vaddps xmm0, xmm0, xmm1
    vhaddps xmm0, xmm0, xmm0
    vhaddps xmm0, xmm0, xmm0

    cvtsi2ss xmm1, ebx
    divss xmm0, xmm1                ; mean(x^2)
    addss xmm0, xmm15               ; + eps
    sqrtss xmm0, xmm0

    movss xmm1, [rel gelu_const_one]
    divss xmm1, xmm0                ; 1/rms
    vbroadcastss ymm7, xmm1

    ; ---- Step 2: Normalize with gamma (2x unrolled) ----
    xor rax, rax
    mov r10, rbx
    shr r10, 4

    test r10, r10
    jz .rms_norm_vec8

.rms_norm_loop16:
    vmovups ymm0, [r12 + rax*4]
    vmulps ymm0, ymm0, ymm7
    vmulps ymm0, ymm0, [r13 + rax*4]
    vmovups [r14 + rax*4], ymm0

    vmovups ymm2, [r12 + rax*4 + 32]
    vmulps ymm2, ymm2, ymm7
    vmulps ymm2, ymm2, [r13 + rax*4 + 32]
    vmovups [r14 + rax*4 + 32], ymm2

    add rax, 16
    dec r10
    jnz .rms_norm_loop16

.rms_norm_vec8:
    lea r10, [rax + 8]
    cmp r10, rbx
    jg .rms_norm_scalar

    vmovups ymm0, [r12 + rax*4]
    vmulps ymm0, ymm0, ymm7
    vmulps ymm0, ymm0, [r13 + rax*4]
    vmovups [r14 + rax*4], ymm0
    add rax, 8

.rms_norm_scalar:
    cmp rax, rbx
    jge .rms_done

    movss xmm0, [r12 + rax*4]
    mulss xmm0, xmm7
    movss xmm1, [r13 + rax*4]
    mulss xmm0, xmm1
    movss [r14 + rax*4], xmm0

    inc rax
    jmp .rms_norm_scalar

.rms_done:
    pop r14
    pop r13
    pop r12
    pop rbx
    vzeroupper
    ret

; ===========================================================================
;                        OPTIMIZER KERNELS
; ===========================================================================

; ===========================================================================
; ADAM OPTIMIZER STEP - Fused Kernel
; void adam_step_avx2(f32* param, const f32* grad, f32* m, f32* v,
;                     f32 lr, f32 beta1, f32 beta2, f32 eps,
;                     f32 beta1_t, f32 beta2_t, i32 n)
;
; Windows x64: RCX=param, RDX=grad, R8=m, R9=v
; XMM4=lr, XMM5=beta1, [rsp+48]=beta2, [rsp+56]=eps
; [rsp+64]=beta1_t, [rsp+72]=beta2_t, [rsp+80]=n
; ===========================================================================
global adam_step_avx2
adam_step_avx2:
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 32                     ; Shadow space + alignment

    mov r12, rcx                    ; param
    mov r13, rdx                    ; grad
    mov r14, r8                     ; m
    mov r15, r9                     ; v

    ; Load and broadcast scalars
    vbroadcastss ymm10, xmm4        ; lr
    vbroadcastss ymm11, xmm5        ; beta1

    vmovss xmm0, [rsp + 128]        ; beta2
    vbroadcastss ymm12, xmm0

    vmovss xmm0, [rsp + 136]        ; eps
    vbroadcastss ymm13, xmm0

    vmovss xmm0, [rsp + 144]        ; beta1_t = 1 - beta1^t
    vbroadcastss ymm14, xmm0

    vmovss xmm0, [rsp + 152]        ; beta2_t = 1 - beta2^t
    vbroadcastss ymm15, xmm0

    mov rbx, [rsp + 160]            ; n

    ; (1 - beta1) and (1 - beta2)
    vmovaps ymm8, [rel gelu_const_one]
    vsubps ymm6, ymm8, ymm11        ; 1 - beta1
    vsubps ymm7, ymm8, ymm12        ; 1 - beta2

    xor rax, rax
    mov r10, rbx
    shr r10, 3

.adam_loop:
    test r10, r10
    jz .adam_remainder

    vmovups ymm0, [r13 + rax*4]     ; grad
    vmovups ymm1, [r14 + rax*4]     ; m
    vmovups ymm2, [r15 + rax*4]     ; v
    vmovups ymm3, [r12 + rax*4]     ; param

    ; m = beta1 * m + (1 - beta1) * grad
    vmulps ymm1, ymm1, ymm11
    vfmadd231ps ymm1, ymm0, ymm6
    vmovups [r14 + rax*4], ymm1

    ; v = beta2 * v + (1 - beta2) * grad^2
    vmulps ymm4, ymm0, ymm0
    vmulps ymm2, ymm2, ymm12
    vfmadd231ps ymm2, ymm4, ymm7
    vmovups [r15 + rax*4], ymm2

    ; Bias correction
    vdivps ymm1, ymm1, ymm14        ; m_hat
    vdivps ymm2, ymm2, ymm15        ; v_hat

    ; Update: param -= lr * m_hat / (sqrt(v_hat) + eps)
    vsqrtps ymm2, ymm2
    vaddps ymm2, ymm2, ymm13
    vdivps ymm1, ymm1, ymm2
    vmulps ymm1, ymm1, ymm10
    vsubps ymm3, ymm3, ymm1
    vmovups [r12 + rax*4], ymm3

    add rax, 8
    dec r10
    jmp .adam_loop

.adam_remainder:
    cmp rax, rbx
    jge .adam_done

    movss xmm0, [r13 + rax*4]
    movss xmm1, [r14 + rax*4]
    movss xmm2, [r15 + rax*4]
    movss xmm3, [r12 + rax*4]

    mulss xmm1, xmm11
    movss xmm4, xmm0
    mulss xmm4, xmm6
    addss xmm1, xmm4
    movss [r14 + rax*4], xmm1

    movss xmm4, xmm0
    mulss xmm4, xmm0
    mulss xmm4, xmm7
    mulss xmm2, xmm12
    addss xmm2, xmm4
    movss [r15 + rax*4], xmm2

    divss xmm1, xmm14
    divss xmm2, xmm15
    sqrtss xmm2, xmm2
    addss xmm2, xmm13
    divss xmm1, xmm2
    mulss xmm1, xmm10
    subss xmm3, xmm1
    movss [r12 + rax*4], xmm3

    inc rax
    jmp .adam_remainder

.adam_done:
    add rsp, 32
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    vzeroupper
    ret

; ===========================================================================
;                       BACKWARD PASS KERNELS
; ===========================================================================

; ===========================================================================
; RELU BACKWARD
; void relu_backward_avx2(const f32* grad_out, const f32* x, f32* grad_in, i32 n)
; grad_in = grad_out * (x > 0)
; 2x unrolled.
; ===========================================================================
global relu_backward_avx2
relu_backward_avx2:
    xor rax, rax
    mov r10, r9
    shr r10, 4
    vxorps ymm5, ymm5, ymm5         ; Zero vector

    test r10, r10
    jz .relu_bwd_vec8

.relu_bwd_loop16:
    vmovups ymm0, [rcx + rax*4]
    vmovups ymm1, [rdx + rax*4]
    vcmpps ymm2, ymm1, ymm5, 14     ; GT
    vandps ymm0, ymm0, ymm2
    vmovups [r8 + rax*4], ymm0

    vmovups ymm3, [rcx + rax*4 + 32]
    vmovups ymm4, [rdx + rax*4 + 32]
    vcmpps ymm2, ymm4, ymm5, 14
    vandps ymm3, ymm3, ymm2
    vmovups [r8 + rax*4 + 32], ymm3

    add rax, 16
    dec r10
    jnz .relu_bwd_loop16

.relu_bwd_vec8:
    lea r10, [rax + 8]
    cmp r10, r9
    jg .relu_bwd_scalar

    vmovups ymm0, [rcx + rax*4]
    vmovups ymm1, [rdx + rax*4]
    vcmpps ymm2, ymm1, ymm5, 14
    vandps ymm0, ymm0, ymm2
    vmovups [r8 + rax*4], ymm0
    add rax, 8

.relu_bwd_scalar:
    cmp rax, r9
    jge .relu_bwd_done

    movss xmm0, [rcx + rax*4]
    movss xmm1, [rdx + rax*4]
    xorps xmm2, xmm2
    cmpss xmm2, xmm1, 1             ; x <= 0
    andnps xmm2, xmm0
    movss [r8 + rax*4], xmm2

    inc rax
    jmp .relu_bwd_scalar

.relu_bwd_done:
    vzeroupper
    ret

; ===========================================================================
; GELU BACKWARD
; void gelu_backward_avx2(const f32* grad_out, const f32* x, f32* grad_in, i32 n)
;
; gelu'(x) = sigmoid(1.702*x) * (1 + 1.702*x * (1 - sigmoid(1.702*x)))
; ===========================================================================
global gelu_backward_avx2
gelu_backward_avx2:
    push rbx
    push r12
    push r13
    push r14

    mov r12, rcx                    ; grad_out
    mov r13, rdx                    ; x
    mov r14, r8                     ; grad_in
    mov rbx, r9                     ; n

    xor rax, rax
    mov r10, rbx
    shr r10, 3

    vmovaps ymm10, [rel gelu_const_1702]
    vmovaps ymm11, [rel gelu_const_one]
    vmovaps ymm12, [rel exp_const_log2e]

.gelu_bwd_loop:
    test r10, r10
    jz .gelu_bwd_remainder

    vmovups ymm0, [r12 + rax*4]     ; grad_out
    vmovups ymm1, [r13 + rax*4]     ; x

    vmulps ymm2, ymm1, ymm10        ; t = 1.702 * x

    ; Clamp
    vmovaps ymm3, [rel exp_clamp_lo]
    vmaxps ymm2, ymm2, ymm3
    vmovaps ymm3, [rel exp_clamp_hi]
    vminps ymm2, ymm2, ymm3

    ; exp(t)
    vmulps ymm3, ymm2, ymm12
    vroundps ymm4, ymm3, 1
    vsubps ymm3, ymm3, ymm4

    vmovaps ymm5, [rel exp_const_c4]
    vfmadd213ps ymm5, ymm3, [rel exp_const_c3]
    vfmadd213ps ymm5, ymm3, [rel exp_const_c2]
    vfmadd213ps ymm5, ymm3, [rel exp_const_c1]
    vfmadd213ps ymm5, ymm3, ymm11

    vcvtps2dq ymm4, ymm4
    vpslld ymm4, ymm4, 23
    vpaddd ymm4, ymm4, [rel magic_exp_127]
    vmulps ymm5, ymm5, ymm4         ; exp(t)

    ; sigmoid(t)
    vaddps ymm6, ymm5, ymm11
    vdivps ymm5, ymm5, ymm6

    ; gelu'(x) = s * (1 + t * (1 - s))
    vsubps ymm6, ymm11, ymm5        ; 1 - s
    vmulps ymm2, ymm1, ymm10        ; t = 1.702 * x (unclamped for derivative)
    vmulps ymm6, ymm6, ymm2
    vaddps ymm6, ymm6, ymm11
    vmulps ymm6, ymm5, ymm6

    vmulps ymm0, ymm0, ymm6
    vmovups [r14 + rax*4], ymm0

    add rax, 8
    dec r10
    jmp .gelu_bwd_loop

.gelu_bwd_remainder:
    cmp rax, rbx
    jge .gelu_bwd_done

    movss xmm0, [r12 + rax*4]
    movss xmm1, [r13 + rax*4]

    movss xmm2, [rel gelu_const_1702]
    mulss xmm2, xmm1

    movss xmm3, [rel exp_clamp_lo]
    maxss xmm2, xmm3
    movss xmm3, [rel exp_clamp_hi]
    minss xmm2, xmm3

    movss xmm3, [rel exp_const_log2e]
    mulss xmm3, xmm2
    roundss xmm4, xmm3, 1
    subss xmm3, xmm4

    movss xmm5, [rel exp_const_c4]
    mulss xmm5, xmm3
    addss xmm5, [rel exp_const_c3]
    mulss xmm5, xmm3
    addss xmm5, [rel exp_const_c2]
    mulss xmm5, xmm3
    addss xmm5, [rel exp_const_c1]
    mulss xmm5, xmm3
    addss xmm5, xmm11

    cvtss2si r10d, xmm4
    add r10d, 127
    shl r10d, 23
    movd xmm4, r10d
    mulss xmm5, xmm4

    movss xmm6, xmm11
    addss xmm6, xmm5
    divss xmm5, xmm6                ; sigmoid

    movss xmm6, xmm11
    subss xmm6, xmm5
    movss xmm2, [rel gelu_const_1702]
    mulss xmm2, xmm1
    mulss xmm6, xmm2
    addss xmm6, xmm11
    mulss xmm6, xmm5
    mulss xmm0, xmm6

    movss [r14 + rax*4], xmm0

    inc rax
    jmp .gelu_bwd_remainder

.gelu_bwd_done:
    pop r14
    pop r13
    pop r12
    pop rbx
    vzeroupper
    ret

; ===========================================================================
; SILU BACKWARD
; void silu_backward_avx2(const f32* grad_out, const f32* x, f32* grad_in, i32 n)
;
; SiLU'(x) = sigmoid(x) * (1 + x * (1 - sigmoid(x)))
; ===========================================================================
global silu_backward_avx2
silu_backward_avx2:
    push rbx
    push r12
    push r13
    push r14

    mov r12, rcx
    mov r13, rdx
    mov r14, r8
    mov rbx, r9

    xor rax, rax
    mov r10, rbx
    shr r10, 3

    vmovaps ymm11, [rel gelu_const_one]
    vmovaps ymm12, [rel exp_const_log2e]

.silu_bwd_loop:
    test r10, r10
    jz .silu_bwd_remainder

    vmovups ymm0, [r12 + rax*4]     ; grad_out
    vmovups ymm1, [r13 + rax*4]     ; x

    ; Clamp
    vmovaps ymm2, [rel exp_clamp_lo]
    vmaxps ymm3, ymm1, ymm2
    vmovaps ymm2, [rel exp_clamp_hi]
    vminps ymm3, ymm3, ymm2

    ; exp(x)
    vmulps ymm4, ymm3, ymm12
    vroundps ymm5, ymm4, 1
    vsubps ymm4, ymm4, ymm5

    vmovaps ymm6, [rel exp_const_c4]
    vfmadd213ps ymm6, ymm4, [rel exp_const_c3]
    vfmadd213ps ymm6, ymm4, [rel exp_const_c2]
    vfmadd213ps ymm6, ymm4, [rel exp_const_c1]
    vfmadd213ps ymm6, ymm4, ymm11

    vcvtps2dq ymm5, ymm5
    vpslld ymm5, ymm5, 23
    vpaddd ymm5, ymm5, [rel magic_exp_127]
    vmulps ymm6, ymm6, ymm5

    ; sigmoid(x)
    vaddps ymm7, ymm6, ymm11
    vdivps ymm6, ymm6, ymm7

    ; silu'(x) = s * (1 + x * (1 - s))
    vsubps ymm7, ymm11, ymm6
    vmulps ymm7, ymm7, ymm1
    vaddps ymm7, ymm7, ymm11
    vmulps ymm7, ymm6, ymm7

    vmulps ymm0, ymm0, ymm7
    vmovups [r14 + rax*4], ymm0

    add rax, 8
    dec r10
    jmp .silu_bwd_loop

.silu_bwd_remainder:
    cmp rax, rbx
    jge .silu_bwd_done

    movss xmm0, [r12 + rax*4]
    movss xmm1, [r13 + rax*4]

    movss xmm2, [rel exp_clamp_lo]
    movaps xmm3, xmm1
    maxss xmm3, xmm2
    movss xmm2, [rel exp_clamp_hi]
    minss xmm3, xmm2

    movss xmm4, [rel exp_const_log2e]
    mulss xmm4, xmm3
    roundss xmm5, xmm4, 1
    subss xmm4, xmm5

    movss xmm6, [rel exp_const_c4]
    mulss xmm6, xmm4
    addss xmm6, [rel exp_const_c3]
    mulss xmm6, xmm4
    addss xmm6, [rel exp_const_c2]
    mulss xmm6, xmm4
    addss xmm6, [rel exp_const_c1]
    mulss xmm6, xmm4
    addss xmm6, xmm11

    cvtss2si r10d, xmm5
    add r10d, 127
    shl r10d, 23
    movd xmm5, r10d
    mulss xmm6, xmm5

    movss xmm7, xmm11
    addss xmm7, xmm6
    divss xmm6, xmm7

    movss xmm7, xmm11
    subss xmm7, xmm6
    mulss xmm7, xmm1
    addss xmm7, xmm11
    mulss xmm7, xmm6
    mulss xmm0, xmm7
    movss [r14 + rax*4], xmm0

    inc rax
    jmp .silu_bwd_remainder

.silu_bwd_done:
    pop r14
    pop r13
    pop r12
    pop rbx
    vzeroupper
    ret

; ===========================================================================
; SOFTMAX BACKWARD (Jacobian-vector product)
; void softmax_backward_avx2(const f32* grad_out, const f32* softmax_out,
;                            f32* grad_in, i32 dim)
;
; grad_in = softmax_out * (grad_out - sum(grad_out * softmax_out))
; ===========================================================================
global softmax_backward_avx2
softmax_backward_avx2:
    push rbx
    push r12
    push r13
    push r14

    mov r12, rcx                    ; grad_out
    mov r13, rdx                    ; softmax_out
    mov r14, r8                     ; grad_in
    mov rbx, r9                     ; dim

    ; ---- Step 1: Compute dot = sum(grad_out * softmax_out) ----
    vxorps ymm0, ymm0, ymm0
    vxorps ymm1, ymm1, ymm1
    xor rax, rax
    mov r10, rbx
    shr r10, 4

    test r10, r10
    jz .sbwd_dot_vec8

.sbwd_dot_loop16:
    vmovups ymm2, [r12 + rax*4]
    vmovups ymm3, [r12 + rax*4 + 32]
    vfmadd231ps ymm0, ymm2, [r13 + rax*4]
    vfmadd231ps ymm1, ymm3, [r13 + rax*4 + 32]
    add rax, 16
    dec r10
    jnz .sbwd_dot_loop16

    vaddps ymm0, ymm0, ymm1

.sbwd_dot_vec8:
    lea r10, [rax + 8]
    cmp r10, rbx
    jg .sbwd_dot_scalar

    vmovups ymm2, [r12 + rax*4]
    vfmadd231ps ymm0, ymm2, [r13 + rax*4]
    add rax, 8

.sbwd_dot_scalar:
    cmp rax, rbx
    jge .sbwd_dot_reduce

    movss xmm1, [r12 + rax*4]
    movss xmm2, [r13 + rax*4]
    mulss xmm1, xmm2
    addss xmm0, xmm1

    inc rax
    jmp .sbwd_dot_scalar

.sbwd_dot_reduce:
    vextractf128 xmm1, ymm0, 1
    vaddps xmm0, xmm0, xmm1
    vhaddps xmm0, xmm0, xmm0
    vhaddps xmm0, xmm0, xmm0
    vbroadcastss ymm7, xmm0         ; dot in all lanes

    ; ---- Step 2: grad_in = softmax_out * (grad_out - dot) (2x unrolled) ----
    xor rax, rax
    mov r10, rbx
    shr r10, 4

    test r10, r10
    jz .sbwd_grad_vec8

.sbwd_grad_loop16:
    vmovups ymm1, [r12 + rax*4]
    vmovups ymm2, [r13 + rax*4]
    vsubps ymm1, ymm1, ymm7
    vmulps ymm1, ymm1, ymm2
    vmovups [r14 + rax*4], ymm1

    vmovups ymm3, [r12 + rax*4 + 32]
    vmovups ymm4, [r13 + rax*4 + 32]
    vsubps ymm3, ymm3, ymm7
    vmulps ymm3, ymm3, ymm4
    vmovups [r14 + rax*4 + 32], ymm3

    add rax, 16
    dec r10
    jnz .sbwd_grad_loop16

.sbwd_grad_vec8:
    lea r10, [rax + 8]
    cmp r10, rbx
    jg .sbwd_grad_scalar

    vmovups ymm1, [r12 + rax*4]
    vmovups ymm2, [r13 + rax*4]
    vsubps ymm1, ymm1, ymm7
    vmulps ymm1, ymm1, ymm2
    vmovups [r14 + rax*4], ymm1
    add rax, 8

.sbwd_grad_scalar:
    cmp rax, rbx
    jge .softmax_bwd_done

    movss xmm1, [r12 + rax*4]
    movss xmm2, [r13 + rax*4]
    subss xmm1, xmm7
    mulss xmm1, xmm2
    movss [r14 + rax*4], xmm1

    inc rax
    jmp .sbwd_grad_scalar

.softmax_bwd_done:
    pop r14
    pop r13
    pop r12
    pop rbx
    vzeroupper
    ret

; ===========================================================================
; CROSS-ENTROPY LOSS GRADIENT (fused softmax + cross-entropy backward)
; void cross_entropy_backward_avx2(const f32* softmax_out, const i32* targets,
;                                   f32* grad, i32 batch, i32 vocab_size)
;
; For each sample: grad = softmax_out / batch, then grad[target] -= 1/batch
; ===========================================================================
global cross_entropy_backward_avx2
cross_entropy_backward_avx2:
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov r12, rcx                    ; softmax_out
    mov r13, rdx                    ; targets
    mov r14, r8                     ; grad
    mov r15, r9                     ; batch
    mov rbx, [rsp + 88]             ; vocab_size (5 pushes * 8 + 48)

    ; scale = 1/batch
    cvtsi2ss xmm7, r15d
    movss xmm6, [rel gelu_const_one]
    divss xmm6, xmm7
    vbroadcastss ymm6, xmm6

    xor r10, r10                    ; batch index

.ce_batch_loop:
    cmp r10, r15
    jge .ce_done

    mov eax, [r13 + r10*4]          ; target
    push rax

    ; Copy and scale (2x unrolled)
    xor rax, rax
    mov r11, rbx
    shr r11, 4

    test r11, r11
    jz .ce_copy_vec8

.ce_copy_loop16:
    vmovups ymm0, [r12 + rax*4]
    vmovups ymm1, [r12 + rax*4 + 32]
    vmulps ymm0, ymm0, ymm6
    vmulps ymm1, ymm1, ymm6
    vmovups [r14 + rax*4], ymm0
    vmovups [r14 + rax*4 + 32], ymm1
    add rax, 16
    dec r11
    jnz .ce_copy_loop16

.ce_copy_vec8:
    lea r11, [rax + 8]
    cmp r11, rbx
    jg .ce_copy_scalar

    vmovups ymm0, [r12 + rax*4]
    vmulps ymm0, ymm0, ymm6
    vmovups [r14 + rax*4], ymm0
    add rax, 8

.ce_copy_scalar:
    cmp rax, rbx
    jge .ce_subtract

    movss xmm0, [r12 + rax*4]
    mulss xmm0, xmm6
    movss [r14 + rax*4], xmm0
    inc rax
    jmp .ce_copy_scalar

.ce_subtract:
    pop rax                         ; target
    movss xmm0, [r14 + rax*4]
    subss xmm0, xmm6
    movss [r14 + rax*4], xmm0

    lea r12, [r12 + rbx*4]
    lea r14, [r14 + rbx*4]
    inc r10
    jmp .ce_batch_loop

.ce_done:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    vzeroupper
    ret

; ===========================================================================
;                     GRADIENT UTILITY KERNELS
; ===========================================================================

; ===========================================================================
; VECTOR ACCUMULATE (for gradient accumulation)
; void vec_acc_f32_avx2(f32* acc, const f32* src, i32 n)
; acc[i] += src[i]
; 2x unrolled.
; ===========================================================================
global vec_acc_f32_avx2
vec_acc_f32_avx2:
    xor rax, rax
    mov r10, r8
    shr r10, 4

    test r10, r10
    jz .acc_vec8

.acc_loop16:
    vmovups ymm0, [rcx + rax*4]
    vmovups ymm2, [rcx + rax*4 + 32]
    vaddps ymm0, ymm0, [rdx + rax*4]
    vaddps ymm2, ymm2, [rdx + rax*4 + 32]
    vmovups [rcx + rax*4], ymm0
    vmovups [rcx + rax*4 + 32], ymm2
    add rax, 16
    dec r10
    jnz .acc_loop16

.acc_vec8:
    lea r10, [rax + 8]
    cmp r10, r8
    jg .acc_scalar

    vmovups ymm0, [rcx + rax*4]
    vaddps ymm0, ymm0, [rdx + rax*4]
    vmovups [rcx + rax*4], ymm0
    add rax, 8

.acc_scalar:
    cmp rax, r8
    jge .acc_done

    movss xmm0, [rcx + rax*4]
    addss xmm0, [rdx + rax*4]
    movss [rcx + rax*4], xmm0

    inc rax
    jmp .acc_scalar

.acc_done:
    vzeroupper
    ret

; ===========================================================================
; GRADIENT CLIPPING BY NORM
; void grad_clip_norm_avx2(f32* grad, i32 n, f32 max_norm)
;
; If ||grad||_2 > max_norm, scale by max_norm / ||grad||_2
; ===========================================================================
global grad_clip_norm_avx2
grad_clip_norm_avx2:
    push rbx
    push r12

    mov r12, rcx                    ; grad
    mov rbx, rdx                    ; n
    vmovss xmm7, xmm2               ; max_norm

    ; ---- Step 1: Compute L2 norm squared (dual accumulator) ----
    vxorps ymm0, ymm0, ymm0
    vxorps ymm1, ymm1, ymm1
    xor rax, rax
    mov r10, rbx
    shr r10, 4

    test r10, r10
    jz .clip_norm_vec8

.clip_norm_loop16:
    vmovups ymm2, [r12 + rax*4]
    vfmadd231ps ymm0, ymm2, ymm2
    vmovups ymm3, [r12 + rax*4 + 32]
    vfmadd231ps ymm1, ymm3, ymm3
    add rax, 16
    dec r10
    jnz .clip_norm_loop16

    vaddps ymm0, ymm0, ymm1

.clip_norm_vec8:
    lea r10, [rax + 8]
    cmp r10, rbx
    jg .clip_norm_scalar

    vmovups ymm2, [r12 + rax*4]
    vfmadd231ps ymm0, ymm2, ymm2
    add rax, 8

.clip_norm_scalar:
    cmp rax, rbx
    jge .clip_norm_reduce

    movss xmm1, [r12 + rax*4]
    mulss xmm1, xmm1
    addss xmm0, xmm1

    inc rax
    jmp .clip_norm_scalar

.clip_norm_reduce:
    vextractf128 xmm1, ymm0, 1
    vaddps xmm0, xmm0, xmm1
    vhaddps xmm0, xmm0, xmm0
    vhaddps xmm0, xmm0, xmm0

    sqrtss xmm0, xmm0               ; norm

    comiss xmm0, xmm7
    jbe .clip_done                   ; No clipping needed

    ; scale = max_norm / norm
    divss xmm7, xmm0
    vbroadcastss ymm7, xmm7

    ; Scale gradients (2x unrolled)
    xor rax, rax
    mov r10, rbx
    shr r10, 4

    test r10, r10
    jz .clip_scale_vec8

.clip_scale_loop16:
    vmovups ymm0, [r12 + rax*4]
    vmovups ymm1, [r12 + rax*4 + 32]
    vmulps ymm0, ymm0, ymm7
    vmulps ymm1, ymm1, ymm7
    vmovups [r12 + rax*4], ymm0
    vmovups [r12 + rax*4 + 32], ymm1
    add rax, 16
    dec r10
    jnz .clip_scale_loop16

.clip_scale_vec8:
    lea r10, [rax + 8]
    cmp r10, rbx
    jg .clip_scale_scalar

    vmovups ymm0, [r12 + rax*4]
    vmulps ymm0, ymm0, ymm7
    vmovups [r12 + rax*4], ymm0
    add rax, 8

.clip_scale_scalar:
    cmp rax, rbx
    jge .clip_done

    movss xmm0, [r12 + rax*4]
    mulss xmm0, xmm7
    movss [r12 + rax*4], xmm0

    inc rax
    jmp .clip_scale_scalar

.clip_done:
    pop r12
    pop rbx
    vzeroupper
    ret
