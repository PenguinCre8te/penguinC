#link "./mem.o";

void* alloc(long size) => mem_alloc;
long sizeof_int(void) => mem_sizeof_int;
long sizeof_float(void) => mem_sizeof_float;
long sizeof_bool(void) => mem_sizeof_bool;
long sizeof_pointer(void) => mem_sizeof_pointer;
void drop(void* ptr) => mem_drop;
