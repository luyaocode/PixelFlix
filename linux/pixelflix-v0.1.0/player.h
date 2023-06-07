#ifndef PLAYER_H__
#define PLAYER_H__
#include "queue.h"
#include <stdbool.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#define DEFAULT_VALUE -1

typedef struct FF_AudioParas
{
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_second;

}FFAudioParas;

typedef struct PlayerStatus
{
    bool isStreamFinished;
    bool isAudioDecodeFinished;
    bool isVideoDecodeFinished;
    bool signal;
    int a_idx;
    int v_idx;
    AVFormatContext* fmtCtx;
    AVCodecContext* v_codecCtx;
    AVCodecContext* a_codecCtx;
    AVCodec* v_codec;
    AVCodec* a_codec;
    struct SwrContext* swrCtx;
    //resampling
    Queue vpq;//video packet queue
    Queue apq;//audio packet queue
    Queue vfq;//video frame queue
    Queue afq;//audio frame queue
    FFAudioParas srcParas;
    FFAudioParas tgtParas;

}PlayerStatus;

extern PlayerStatus player_status;

int playerInit(const char* c);
int playerRun(const char* c);


#endif