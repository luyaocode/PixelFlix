#pragma once
extern "C"
{
#include <stdlib.h>
#include <stdbool.h>
#include <libavformat/avformat.h>
}
int play3(int argc, char* argv[]);
int exitCase(const char* c);
int returnCase(const char* c, int n);
struct Node;
struct PacketQueue;
struct FF_AudioParas;
int isEmpty(PacketQueue* q);
int isFull(PacketQueue* q);
int destroy(PacketQueue* q);
int enqueue(PacketQueue* q, AVPacket* p);
int dequeue(PacketQueue* q, AVPacket* p,int block);
int init(PacketQueue* q);
int audioDecodePacket(AVCodecContext* ctx, AVPacket* pkt, uint8_t* audio_buf, int buf_size);
void audioCallback(void* userdata, uint8_t* stream, int len);
void* playAudio(void* arg);