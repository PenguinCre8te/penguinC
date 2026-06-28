#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint64_t refcount;
} ArcHeader;

void *arc_alloc(size_t size) {
    ArcHeader *p = malloc(sizeof(ArcHeader) + size);
    p->refcount = 1;
    return p + 1;
}

void *arc_retain(void *ptr) {
    if (ptr) {
        ArcHeader *h = (ArcHeader *)ptr - 1;
        h->refcount++;
    }
    return ptr;
}

void arc_release(void *ptr) {
    if (ptr) {
        ArcHeader *h = (ArcHeader *)ptr - 1;
        if (--h->refcount == 0) {
            free(h);
        }
    }
}

uint64_t arc_refcount(void *ptr) {
    if (ptr) {
        ArcHeader *h = (ArcHeader *)ptr - 1;
        return h->refcount;
    }
    return 0;
}
