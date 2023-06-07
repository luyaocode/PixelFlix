#include "demux.h"
#include "logger.h"
#include "player.h"
#include <libavformat/avformat.h>
#include <pthread.h>

//thread dePacket
void* demux(void* arg)
{
    printf("thread start\n");
    PlayerStatus* ps = (PlayerStatus*)arg;
    AVFormatContext* p_avfmt_ctx = ps->fmtCtx;
    int v_idx = ps->v_idx;
    int a_idx = ps->a_idx;
    Queue* vpq = &ps->vpq;
    Queue* apq = &ps->apq;

    int ret;
    AVPacket* p_packet;
    while (1)
    {
        p_packet = av_packet_alloc();
        if (!p_packet) logger(EXIT_FAILURE , "Failed to alloc packet.");

        ret = av_read_frame(p_avfmt_ctx , p_packet);
        if (ret == 0) //Ok
        {
            if (p_packet->stream_index == v_idx)//video packet
            {
                logger(LOG , "vpq enqueue.");
                vpq->enqueue(vpq , p_packet);
            }
            else if (p_packet->stream_index == a_idx)//audio packet
            {
                logger(LOG , "apq enqueue.");

                apq->enqueue(apq , p_packet);
            }
            else
            {
                printf("Not video or audio packet.\n");
            }
            //You can't release the packet because once you release the packet,
            // the space which `data` pointer in packet point to will be freed,
            // when you dequeue the packet from the queue, it will try to access unknown space.
            // Struct `AVPacket` has a `data` pointer point to the real space that stores the packet,
            // `av_packet_unref()` will clear the packet's paras and relevant memory space that the `data` points to.
            // Pointer packet will not be set NULL after `av_packet_unref()` being called in any case.
            // but the `data` will set NULL
            // av_packet_unref(p_packet);
        }
        else
        {
            ps->isStreamFinished = true;
            printf("All packets have been enqueued.\n");
            // av_packet_unref(p_packet);
            break;
        }


    }
    return NULL;
}
//start demux thread
int openDemux(PlayerStatus* ps)
{

    pthread_t demuxThread;
    pthread_create(&demuxThread , NULL , demux , ps);
    return 1;

}