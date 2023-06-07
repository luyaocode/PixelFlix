#include "queue.h"
#include "logger.h"
#include "player.h"
#include <stdlib.h>
#include <stdbool.h>
#include <libavformat/avformat.h>
#include <SDL2/SDL.h>

int isEmpty(Queue* q)
{
    if (q->head == q->rear) return 1;
    else return 0;
}
int isFull(Queue* q)
{
    if (q->n == q->max) return 1;
    else return 0;
}
int destroy(Queue* q)
{
    if (!q->head) return 0;
    Node* temp = q->head;
    while (!temp)
    {
        Node* next = temp->next;
        free(temp);
        temp = next;
    }
    q->head = NULL;
    q->n = 0;
    q->bytes = 0;
    return 1;
}

//1 on success, 0 on failure
int enqueue(Queue* q , void* elem)
{
    SDL_LockMutex(q->mutex);
    if (q == NULL || elem == NULL) logger(EXIT_FAILURE , "NULL pointer error");
    if (av_packet_make_refcounted(elem)) logger(LOG , "Failed to set elem reference counted.");
    if (isFull(q)) return 0;
    Node* pnode = (Node*)av_malloc(sizeof(Node));
    if (!pnode) logger(EXIT_FAILURE , "Failed to malloc Node.");
    pnode->e = elem;
    pnode->next = NULL;
    q->rear->next = pnode;
    q->rear = pnode;
    q->n++;
    if (q->type == AVPACKET) q->bytes += sizeof(AVPacket);
    else if (q->type == AVFRAME) q->bytes += sizeof(AVFrame);
    player_status.signal = true;
    SDL_CondSignal(q->cond);
    if (q->type == AVPACKET)
        logger(LOG , "[%d]en: n=%d, size=%d, last_data=%x" , q->type , q->n , q->bytes , ((AVPacket*)(q->rear->e))->data);
    else if (q->type == AVFRAME)
        logger(LOG , "[%d]en: n=%d, size=%d, last_data=%x" , q->type , q->n , q->bytes , ((AVFrame*)(q->rear->e))->data);
    SDL_UnlockMutex(q->mutex);
    return 1;
}

int dequeue(Queue* q , void** elem)
{
    if (q == NULL) logger(EXIT_FAILURE , "NULL pointer error");
    Node* temp = NULL;
    int res;
    SDL_LockMutex(q->mutex);
    while (1)
    {

        temp = q->head->next;
        if (temp != NULL)// n>0
        {
            if (q->type == AVPACKET) *elem = temp->e;
            q->head->next = q->head->next->next;
            temp->next = NULL;
            if (temp == q->rear) q->rear = q->head;//if n=1, q->rear should be q->head after dequeue.
            if (q->type == AVPACKET) q->bytes -= sizeof(AVPacket);
            else if (q->type == AVFRAME) q->bytes -= sizeof(AVFrame);
            q->n--;
            free(temp);
            if (q->type == AVPACKET)
                logger(LOG , "[%d]de: n=%d, size=%d, first_data=%x" , q->type , q->n , q->bytes , ((AVPacket*)(q->head->next->e))->data);
            else if (q->type == AVFRAME)
                logger(LOG , "[%d]de: n=%d, size=%d, first_data=%lx" , q->type , q->n , q->bytes , ((AVFrame*)(q->head->next->e))->data);

            res = 1;
            break;
        }
        else if (player_status.isStreamFinished || q->blocked) //n=0 and (stream is over or queue is blocked)
        {
            res = 0;
            break;
        }
        else //n=0 and (stream is not over and queue is not blocked)
        {
            while (!player_status.signal)
            {
                SDL_CondWait(q->cond , q->mutex);
            }
        }
    }
    SDL_UnlockMutex(q->mutex);
    return res;
}

int init(ElementType type , Queue* q)
{
    if (!q->head)
    {
        Node* head = (Node*)malloc(sizeof(Node));
        if (!head) logger(EXIT_FAILURE , "Failed to malloc Node.");
        head->e = NULL;
        head->next = NULL;
        q->head = head;
    }
    q->rear = q->head;
    q->n = 0;
    if (type == AVPACKET) q->max = PACKET_QUEUE_SIZE;
    else if (type == AVFRAME) q->max = FRAME_QUEUE_SIZE;
    else logger(EXIT_FAILURE , "Unknown queue type.");
    q->bytes = 0;
    q->destroy = destroy;
    q->isEmpty = isEmpty;
    q->isFull = isFull;
    q->dequeue = dequeue;
    q->enqueue = enqueue;
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
    q->blocked = false;
    q->type = type;
    return 1;
}