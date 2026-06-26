// std.mem - Memory management (implemented in stdlib/mem.c)
void* mem.alloc(long size);
long mem.sizeof_int(void);
long mem.sizeof_float(void);
long mem.sizeof_bool(void);
long mem.sizeof_pointer(void);
void mem.drop(void* ptr);
