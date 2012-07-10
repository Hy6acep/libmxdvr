#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "queue.h"


struct queue
{
	unsigned char** buffers;
	int buffer_size;
	int queue_size;
	int first;
	int available;
	pthread_mutex_t mutex;
};

queue queue_new(int buffer_size, int queue_size)
{
	int i;
	queue ptr = NULL;

	if(buffer_size<1||queue_size<1) return NULL;

	ptr = calloc(1, sizeof(struct queue));
	ptr->buffer_size = buffer_size;
	ptr->queue_size = queue_size;
	ptr->first = 0;
	ptr->available = queue_size;


	ptr->buffers = calloc(queue_size, sizeof(queue));

	for(i = 0; i<queue_size; ++i)
		ptr->buffers[i] = calloc(1, buffer_size);

	pthread_mutex_init(&ptr->mutex, NULL);
	return ptr;
}

void queue_delete(queue* q)
{
	queue ptr = *q;
	int i;

	for(i = 0; i < ptr->queue_size; ++i)
	{
		free(ptr->buffers[i]);
	}

	free(ptr->buffers);
	free(ptr);

	*q = NULL;
}


void queue_push(queue q, unsigned char* data)
{
	int index;

	if(!q||!q->available) return;

	pthread_mutex_lock(&q->mutex);

	index = q->first + (q->queue_size - q->available);
	q->available--;

	index %= q->queue_size;

	memcpy(q->buffers[index], data, q->buffer_size);
	pthread_mutex_unlock(&q->mutex);

}

unsigned char* queue_pop(queue q)
{
	int index;

	if(!q||q->available == q->queue_size) return NULL;

	pthread_mutex_lock(&q->mutex);

	index = q->first;

	q->first++;
	q->available++;

	q->first %= q->queue_size;
	pthread_mutex_unlock(&q->mutex);

	return q->buffers[index];
}






