#include <pthread.h>
#include <stdlib.h>
#include <stddef.h>

typedef long (*thread_fn)(long);

typedef struct {
    pthread_t thread;
    thread_fn fn;
    long arg;
    long result;
    int done;
} thread_handle;

static void *trampoline(void *data) {
    thread_handle *h = (thread_handle *)data;
    h->result = h->fn(h->arg);
    h->done = 1;
    return NULL;
}

long thread_run(long fn_ptr, long arg) {
    thread_handle *h = (thread_handle *)malloc(sizeof(thread_handle));
    h->fn = (thread_fn)fn_ptr;
    h->arg = arg;
    h->result = 0;
    h->done = 0;

    pthread_create(&h->thread, NULL, trampoline, h);

    return (long)h;
}

long thread_run1(long fn_ptr) {
    return thread_run(fn_ptr, 0);
}

void thread_join(long handle) {
    thread_handle *h = (thread_handle *)handle;
    if (h) {
        pthread_join(h->thread, NULL);
    }
}

long thread_get(long handle) {
    thread_handle *h = (thread_handle *)handle;
    if (h) {
        pthread_join(h->thread, NULL);
        long result = h->result;
        free(h);
        return result;
    }
    return 0;
}
