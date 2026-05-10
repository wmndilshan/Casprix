; Casprix Runtime - Coroutine Context Switch (x86-64)
; Optimized for Linux/POSIX ABI

bits 64
default rel

section .text
global coro_context_switch

; coro_context_switch(void** from_sp, void* to_sp)
; rdi = from_sp (pointer to the RSP storage of the current coroutine)
; rsi = to_sp (the RSP value of the target coroutine)
coro_context_switch:
    ; Save callee-saved registers of the current coroutine
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save current RSP to *from_sp
    mov [rdi], rsp

    ; Switch to the new RSP
    mov rsp, rsi

    ; Restore callee-saved registers of the target coroutine
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ret

; coro_context_init(void* stack_top, void* entry, void* context)
; rdi = stack_top
; rsi = entry
; rdx = context
; This helper prepares the initial stack for a new coroutine
global coro_context_init
coro_context_init:
    mov rax, rsp       ; Save real RSP
    mov rsp, rdi       ; Switch to coroutine stack top

    ; Align stack and prepare for call
    sub rsp, 8         ; Dummy alignment for ret

    ; Push the wrapper that will call entry(context)
    push rdx           ; context (argument for entry)
    push rsi           ; entry (function pointer)
    
    ; We'll use a small wrapper in C to handle the entry/exit
    ; For now, just push a dummy return address to simulate a switch-in
    sub rsp, 48        ; space for callee-saved (6 * 8)
    
    mov rdi, rsp       ; Return the new RSP
    mov rsp, rax       ; Restore real RSP
    ret
