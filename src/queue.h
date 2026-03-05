#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>

#define QUEUE_SIZE 64
#define FRAME_QUEUE_SIZE 8   /* decoded frames — keep small to throttle decoder */

typedef struct {
    void           *items[QUEUE_SIZE];
    int             head;
    int             tail;
    int             count;
    int             max;        /* actual capacity, <= QUEUE_SIZE */
    int             closed;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} Queue;

void queue_init(Queue *q);
void queue_init_size(Queue *q, int max);  /* init with custom capacity */
int  queue_push(Queue *q, void *item);   /* returns 0 if closed */
int  queue_pop(Queue *q, void **item);   /* returns 0 if closed and empty */
int  queue_trypop(Queue *q, void **item); /* non-blocking: 1=got item, 0=empty, -1=closed+empty */
void queue_close(Queue *q);              /* unblocks all waiting threads */
void queue_flush(Queue *q);             /* discard all items */
void queue_destroy(Queue *q);

#endif
