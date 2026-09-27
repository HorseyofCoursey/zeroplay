#include "queue.h"
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

void queue_init(Queue *q)
{
    memset(q, 0, sizeof(*q));
    q->max = QUEUE_SIZE;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

void queue_init_size(Queue *q, int max)
{
    queue_init(q);
    if (max > 0 && max <= QUEUE_SIZE)
        q->max = max;
}

int queue_push(Queue *q, void *item)
{
    pthread_mutex_lock(&q->mutex);
    while (q->count == q->max && !q->closed)
        pthread_cond_wait(&q->not_full, &q->mutex);

    if (q->closed) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }

    q->items[q->tail] = item;
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

int queue_pop(Queue *q, void **item)
{
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->mutex);

    if (q->count == 0) {
        /* closed and empty */
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }

    *item = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_SIZE;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

int queue_trypop(Queue *q, void **item)
{
    pthread_mutex_lock(&q->mutex);
    if (q->count == 0) {
        int closed = q->closed;
        pthread_mutex_unlock(&q->mutex);
        return closed ? -1 : 0;  /* -1 = closed+empty, 0 = just empty */
    }
    *item = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_SIZE;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

void queue_close(Queue *q)
{
    pthread_mutex_lock(&q->mutex);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
}

void queue_flush(Queue *q)
{
    pthread_mutex_lock(&q->mutex);
    q->head  = 0;
    q->tail  = 0;
    q->count = 0; // this apparently makes the program forget it heap-allocated elements, causing/enabling memory leak
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
}


// New function I wrote to cleanly free memory before flushing queue
// pointer to function is needed for handling vdec frames which are different

void queue_flush_with_free(Queue *q, void (*free_fn)(void *))
{
    // fprintf(stderr, "DEBUG: queue.c: STARTED queue_flush_with_free().\n");
	pthread_mutex_lock(&q->mutex);
    // fprintf(stderr, "DEBUG: queue.c: DONE pthread_mutex_lock().\n");
	while (q->count > 0) {
		void *item = q->items[q->head];
        // fprintf(stderr, "DEBUG: queue.c: DONE void *item = q->items[q->head].\n");
		q->head = (q->head + 1) % QUEUE_SIZE;
		q->count--;
        // fprintf(stderr, "DEBUG: queue.c: STARTED free_fn().\n");
		free_fn(item);  // SEGFAULT occurs here when closing audio_queue
        // fprintf(stderr, "DEBUG: queue.c: DONE 1 iteration of free_fn(item). Count now: %d. \n", q->count);
	}
    // fprintf(stderr, "DEBUG: queue.c: DONE all free_fn(item) iterations.\n");
	q->head = 0;
	q->tail = 0;
	pthread_cond_broadcast(&q->not_full);
	pthread_mutex_unlock(&q->mutex);
    // fprintf(stderr, "DEBUG: queue.c: FINISHED queue_flush_with_free().\n");
}

void queue_destroy(Queue *q)
{
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}
