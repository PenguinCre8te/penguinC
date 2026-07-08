#include <pthread.h>
#include <stdlib.h>
#include <stddef.h>

typedef struct {
    pthread_mutex_t mutex;
    long lock_count;
} mutex_handle;

long mutex_create(void) {
    mutex_handle *m = (mutex_handle *)malloc(sizeof(mutex_handle));
    pthread_mutex_init(&m->mutex, NULL);
    m->lock_count = 0;
    return (long)m;
}

void mutex_destroy(long handle) {
    mutex_handle *m = (mutex_handle *)handle;
    if (m) {
        pthread_mutex_destroy(&m->mutex);
        free(m);
    }
}

void mutex_enter(long handle) {
    mutex_handle *m = (mutex_handle *)handle;
    if (m) {
        pthread_mutex_lock(&m->mutex);
        m->lock_count++;
    }
}

void mutex_exit(long handle) {
    mutex_handle *m = (mutex_handle *)handle;
    if (m) {
        m->lock_count--;
        pthread_mutex_unlock(&m->mutex);
    }
}

long mutex_try_enter(long handle) {
    mutex_handle *m = (mutex_handle *)handle;
    if (m) {
        int result = pthread_mutex_trylock(&m->mutex);
        if (result == 0) {
            m->lock_count++;
            return 1;
        }
        return 0;
    }
    return 0;
}

long mutex_get_lock_count(long handle) {
    mutex_handle *m = (mutex_handle *)handle;
    if (m) return m->lock_count;
    return 0;
}
