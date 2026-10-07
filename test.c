#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

long counter = 0;

void* increment(void* arg) {
    for (int i = 0; i < 1000000; i++) {
        // Intentional data race (no mutex lock)
        counter = counter + 1;
    }
    return NULL;
}

int main() {
    pthread_t t1, t2, t3;

    pthread_create(&t1, NULL, increment, NULL);
    pthread_create(&t2, NULL, increment, NULL);
    pthread_create(&t3, NULL, increment, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);

    printf("%ld\n", counter);
    return 0;
}