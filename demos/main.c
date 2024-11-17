#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

void *t_task(void *arg) {
    while (1) {
        pthread_mutex_lock(&mutex);
        printf("Waiting...\n");
        pthread_cond_wait(&cond, &mutex);
        printf("Released\n");
        pthread_mutex_unlock(&mutex);
    }
    return NULL;
}

void *t_main(void *arg) {
    while (1) {
        usleep(1e5);
        pthread_mutex_lock(&mutex);
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&mutex);
    }
    return NULL;
}

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, t_task, NULL);
    pthread_create(&t2, NULL, t_main, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    return 0;
}
