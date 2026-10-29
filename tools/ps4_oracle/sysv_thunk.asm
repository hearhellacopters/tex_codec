; Windows x64 -> System V x64 call thunk plus SysV libc replacements for the
; PS4 eboot's memcpy / memset import stubs.

.code

; uint64_t sysv_call(void* fn, const uint64_t* args, uint64_t nargs)
;   rcx = fn, rdx = args, r8 = nargs (args beyond 6 go on the stack)
sysv_call PROC FRAME
    push rbp
    .pushreg rbp
    push rbx
    .pushreg rbx
    push rdi
    .pushreg rdi
    push rsi
    .pushreg rsi
    push r12
    .pushreg r12
    push r13
    .pushreg r13
    push r14
    .pushreg r14
    push r15
    .pushreg r15
    sub rsp, 168
    .allocstack 168
    movdqu [rsp+0],   xmm6
    movdqu [rsp+16],  xmm7
    movdqu [rsp+32],  xmm8
    movdqu [rsp+48],  xmm9
    movdqu [rsp+64],  xmm10
    movdqu [rsp+80],  xmm11
    movdqu [rsp+96],  xmm12
    movdqu [rsp+112], xmm13
    movdqu [rsp+128], xmm14
    movdqu [rsp+144], xmm15
    mov rbp, rsp
    .setframe rbp, 0
    .endprolog

    mov r12, rcx            ; fn
    mov r13, rdx            ; args
    mov r14, r8             ; nargs

    ; stack args: count = max(nargs - 6, 0), keep rsp 16-aligned at the call
    xor rax, rax
    cmp r14, 6
    jbe no_stack
    mov rax, r14
    sub rax, 6
no_stack:
    mov r15, rax
    lea rcx, [rax*8 + 15]
    and rcx, -16
    sub rsp, rcx
    and rsp, -16
    xor rcx, rcx
copy_loop:
    cmp rcx, r15
    jae copy_done
    mov rax, [r13 + rcx*8 + 48]
    mov [rsp + rcx*8], rax
    inc rcx
    jmp copy_loop
copy_done:
    mov rdi, [r13 + 0]
    mov rsi, [r13 + 8]
    mov rdx, [r13 + 16]
    mov rcx, [r13 + 24]
    mov r8,  [r13 + 32]
    mov r9,  [r13 + 40]
    xor eax, eax
    call r12

    mov rsp, rbp
    movdqu xmm6,  [rsp+0]
    movdqu xmm7,  [rsp+16]
    movdqu xmm8,  [rsp+32]
    movdqu xmm9,  [rsp+48]
    movdqu xmm10, [rsp+64]
    movdqu xmm11, [rsp+80]
    movdqu xmm12, [rsp+96]
    movdqu xmm13, [rsp+112]
    movdqu xmm14, [rsp+128]
    movdqu xmm15, [rsp+144]
    add rsp, 168
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbx
    pop rbp
    ret
sysv_call ENDP

; SysV: void* memcpy(rdi dst, rsi src, rdx n)
sysv_memcpy PROC
    mov rax, rdi
    mov rcx, rdx
    rep movsb
    ret
sysv_memcpy ENDP

; SysV: void* memset(rdi dst, esi c, rdx n)
sysv_memset PROC
    mov r8, rdi
    mov eax, esi
    mov rcx, rdx
    rep stosb
    mov rax, r8
    ret
sysv_memset ENDP

; __stack_chk_fail: trap
sysv_trap PROC
    int 3
    ret
sysv_trap ENDP

END
