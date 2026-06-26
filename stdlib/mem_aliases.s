.text

.macro trampoline name, target
.globl \name
\name:
    b \target
.endm

trampoline mem.alloc, mem_alloc
trampoline mem.sizeof_int, mem_sizeof_int
trampoline mem.sizeof_float, mem_sizeof_float
trampoline mem.sizeof_bool, mem_sizeof_bool
trampoline mem.sizeof_pointer, mem_sizeof_pointer
trampoline mem.drop, mem_drop
