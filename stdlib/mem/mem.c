#include <stdlib.h>

void *mem_alloc(long size) {
    return malloc(size);
}

long mem_sizeof_int(void) {
    return sizeof(long);
}

long mem_sizeof_float(void) {
    return sizeof(double);
}

long mem_sizeof_bool(void) {
    return 1;
}

long mem_sizeof_pointer(void) {
    return sizeof(void *);
}

void mem_drop(void *ptr) {
    free(ptr);
}
