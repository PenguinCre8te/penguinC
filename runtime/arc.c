#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

typedef struct {
    _Atomic uint64_t refcount;
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

char *arc_wrap_string(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = arc_alloc(len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

void *arc_retain_shared(void *ptr) {
    if (ptr) {
        ArcHeader *h = (ArcHeader *)ptr - 1;
        atomic_fetch_add(&h->refcount, 1);
    }
    return ptr;
}

void arc_release_shared(void *ptr) {
    if (ptr) {
        ArcHeader *h = (ArcHeader *)ptr - 1;
        if (atomic_fetch_sub(&h->refcount, 1) == 1) {
            free(h);
        }
    }
}
