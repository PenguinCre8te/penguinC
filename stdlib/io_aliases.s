.text

.macro trampoline name, target
.globl \name
\name:
    b \target
.endm

trampoline io.print, io_print
trampoline io.println, io_println
trampoline io.print_int, io_print_int
trampoline io.println_int, io_println_int
trampoline program_exit, program_exit_c
trampoline str_concat, penguin_str_concat
