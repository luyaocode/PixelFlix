#ifndef QUEUE_H__
#define QUEUE_H__
#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#define PACKET_QUEUE_SIZE UINT32_MAX
#define FRAME_QUEUE_SIZE UINT32_MAX

typedef enum {
    AVPACKET ,
    AVFRAME ,
} ElementType;


typedef struct Node
{
    void* e;
    struct Node* next;
}Node;

typedef struct Queue
{
    Node* head;
    Node* rear;
    uint32_t n;
    uint32_t max;
    uint32_t bytes;
    int (*destroy)(struct Queue* q);
    int (*isEmpty)(struct Queue* q);
    int (*isFull)(struct Queue* q);
    int (*enqueue)(struct Queue* q , void* p);
    int (*dequeue)(struct Queue* q , void** p);
    SDL_mutex* mutex;
    SDL_cond* cond;
    bool blocked;
    ElementType type;
}Queue;

int init(ElementType type , Queue* q);
int destroy(Queue* q);
int isEmpty(Queue* q);
int isFull(Queue* q);
int enqueue(Queue* q , void* p);
int dequeue(Queue* q , void** p);



#endif