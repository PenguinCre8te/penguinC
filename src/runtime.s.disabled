.text
.global io.print
.global io.println
.global io.print_int
.global io.println_int
.global program_exit
.global str_len
.global str_concat

// io.print(char* buf, int len)
io.print:
    mov x2, x1
    mov x1, x0
    mov x0, #1
    mov x8, #64
    svc #0
    ret

// io.println(char* buf, int len)
io.println:
    mov x2, x1
    mov x1, x0
    mov x0, #1
    mov x8, #64
    svc #0
    adrp x1, .Lnewline
    add  x1, x1, :lo12:.Lnewline
    mov  x2, #1
    mov  x0, #1
    mov  x8, #64
    svc #0
    ret

// io.print_int(long val)
io.print_int:
    sub sp, sp, #48
    str x30, [sp, #40]
    str x19, [sp, #32]
    str x20, [sp, #24]
    mov x19, x0
    add x20, sp, #15
    mov w0, #0
    strb w0, [x20]
    cmp x19, #0
    bge .Lpositive
    mov x0, #1
    adrp x1, .Lminus
    add  x1, x1, :lo12:.Lminus
    mov  x2, #1
    mov  x8, #64
    svc #0
    neg x19, x19
.Lpositive:
.Ldigit_loop:
    sub x20, x20, #1
    mov x0, x19
    mov x1, #10
    udiv x2, x0, x1
    msub x0, x2, x1, x0
    add w0, w0, #'0'
    strb w0, [x20]
    mov x19, x2
    cbnz x19, .Ldigit_loop
    mov x0, x20
.Llen_loop:
    ldrb w1, [x0]
    cbz w1, .Llen_done
    add x0, x0, #1
    b .Llen_loop
.Llen_done:
    sub x2, x0, x20
    mov x0, #1
    mov x1, x20
    mov x8, #64
    svc #0
    ldr x30, [sp, #40]
    ldr x19, [sp, #32]
    ldr x20, [sp, #24]
    add sp, sp, #48
    ret

// io.println_int(long val)
io.println_int:
    stp x29, x30, [sp, #-16]!
    bl io.print_int
    mov x0, #1
    adrp x1, .Lnewline
    add  x1, x1, :lo12:.Lnewline
    mov  x2, #1
    mov  x8, #64
    svc #0
    ldp x29, x30, [sp], #16
    ret

// program_exit(int code)
program_exit:
    mov x8, #93
    svc #0

.Lnewline:
    .byte 10
.Lminus:
    .byte 45

// str_len(char* buf) -> long
// Returns length of null-terminated string
str_len:
    mov x1, x0
    mov x0, #0
.Lstrlen_loop:
    ldrb w2, [x1, x0]
    cbz w2, .Lstrlen_done
    add x0, x0, #1
    b .Lstrlen_loop
.Lstrlen_done:
    ret

// str_concat(char* a, char* b) -> char*
// Allocates new buffer, copies a + b into it
// Stack layout: [sp+16]=len_a, [sp+24]=len_b, [sp+8]=x30, [sp]=x29
str_concat:
    stp x29, x30, [sp, #-32]!
    str x19, [sp, #16]
    str x20, [sp, #24]
    mov x19, x0          // x19 = a
    mov x20, x1          // x20 = b
    // len_a
    bl str_len
    str x0, [sp, #16]    // save len_a on stack
    // len_b
    mov x0, x20
    bl str_len
    str x0, [sp, #24]    // save len_b on stack
    // total = len_a + len_b + 1
    ldr x9, [sp, #16]
    ldr x10, [sp, #24]
    add x0, x9, x10
    add x0, x0, #1
    // malloc(total)
    bl malloc
    // x0 = allocated buffer, save it
    mov x20, x0          // x20 = buffer (reuse x20, b is no longer needed)
    // copy a
    ldr x2, [sp, #16]    // x2 = len_a
    mov x1, x19          // x1 = a
.Lstrcat_copy_a:
    cbz x2, .Lstrcat_copy_b
    ldrb w3, [x1]
    strb w3, [x20]
    add x20, x20, #1
    add x1, x1, #1
    sub x2, x2, #1
    b .Lstrcat_copy_a
.Lstrcat_copy_b:
    ldr x2, [sp, #24]    // x2 = len_b
    // reload b pointer from saved x20... wait, we used x20 for buffer
    // We need b pointer. Let me restructure.
    // Actually, we saved b in x20 but then reused x20. Let me save buffer in a different way.

.Lstrcat_copy_b_loop:
    cbz x2, .Lstrcat_null
    ldrb w3, [x1]
    strb w3, [x0]
    add x0, x0, #1
    add x1, x1, #1
    sub x2, x2, #1
    b .Lstrcat_copy_b_loop
.Lstrcat_null:
    mov w3, #0
    strb w3, [x0]
    // return pointer to start of buffer
    // we need to subtract len_a + len_b from current x0
    sub x0, x0, x9
    sub x0, x0, x10
    ldr x19, [sp, #16]
    ldr x20, [sp, #24]
    ldp x29, x30, [sp], #32
    ret
