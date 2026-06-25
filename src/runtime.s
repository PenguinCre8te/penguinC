.text
.global io.print
.global io.println
.global io.print_int
.global io.println_int
.global program_exit

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
