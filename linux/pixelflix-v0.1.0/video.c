#include "video.h"
#include "player.h"
#include "logger.h"
#include <pthread.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>

//video playing thread

void* videoPlaying(void* arg)
{

    PlayerStatus* ps = (PlayerStatus*)arg;

    AVCodecContext* p_avcodec_ctx = ps->v_codecCtx;
    Queue* vpq = &ps->vpq;

    AVFrame* p_avframe_raw = NULL;
    AVFrame* p_avframe_yuv = NULL;
    AVPacket* p_avpacket = NULL;
    SDL_Event* event = NULL;

    struct SwsContext* p_sws_ctx;
    SDL_Window* win;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    SDL_Rect rect;

    int buf_size;
    int ret = 0;
    uint8_t* buffer;

    p_avframe_raw = av_frame_alloc();
    p_avframe_yuv = av_frame_alloc();
    if (!p_avframe_raw || !p_avframe_yuv)
    {
        printf("Failed to allocate an avframe.\n");
        exit(EXIT_FAILURE);
    }
    buf_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P ,
        p_avcodec_ctx->width ,
        p_avcodec_ctx->height ,
        1);//last para means align,1 means no align ,4 means 4 bytes align etc.
    // printf("buf_size: %d\n", buf_size);
    buffer = (uint8_t*)av_malloc(buf_size);//buffer is a pointer, buffer++ means moving to next bytes.

    av_image_fill_arrays(p_avframe_yuv->data ,
        p_avframe_yuv->linesize ,
        buffer ,
        AV_PIX_FMT_YUV420P ,
        p_avcodec_ctx->width ,
        p_avcodec_ctx->height ,
        1
    );

    //initilize sws context
    p_sws_ctx = sws_getContext(p_avcodec_ctx->width ,
        p_avcodec_ctx->height ,
        p_avcodec_ctx->pix_fmt ,//src fmt
        p_avcodec_ctx->width ,
        p_avcodec_ctx->height ,
        AV_PIX_FMT_YUV420P ,
        SWS_BICUBIC ,
        NULL ,
        NULL ,
        NULL
    );
    if (!p_sws_ctx)
    {
        printf("Falied to initilize sws context.\n");
        exit(EXIT_FAILURE);
    }

    win = SDL_CreateWindow(
        "PixelFlix 简易视频播放器" ,
        SDL_WINDOWPOS_UNDEFINED ,
        SDL_WINDOWPOS_UNDEFINED ,
        p_avcodec_ctx->width ,
        p_avcodec_ctx->height ,
        SDL_WINDOW_OPENGL
    );
    if (!win)
    {
        printf("Failed to create a window.\n");
        exit(EXIT_FAILURE);
    }
    renderer = SDL_CreateRenderer(win , -1 , 0);
    if (!renderer)
    {
        printf("Failed to create a renderer.\n");
        exit(EXIT_FAILURE);
    }
    texture = SDL_CreateTexture(
        renderer ,
        SDL_PIXELFORMAT_IYUV ,
        SDL_TEXTUREACCESS_STREAMING ,
        p_avcodec_ctx->width ,
        p_avcodec_ctx->height
    );
    if (!texture)
    {
        printf("Failed to create a renderer.\n");
        exit(EXIT_FAILURE);
    }
    rect.x = 0;
    rect.y = 0;
    rect.w = p_avcodec_ctx->width;
    rect.h = p_avcodec_ctx->height;


    // read packet from stream
    // one packet contains one video frame or audio frame
    p_avpacket = av_packet_alloc();
    if (!p_avpacket) logger(EXIT_FAILURE , "Failed to alloc packet.");
    while (1)
    {
        // dequeue a video packet
        ret = vpq->dequeue(vpq , (void**)&p_avpacket);
        if (ret != 1)
        {
            continue;
        }

    }
    sws_scale(p_sws_ctx ,
        (const uint8_t* const*)p_avframe_raw->data ,
        p_avframe_raw->linesize ,
        0 ,
        p_avcodec_ctx->height ,
        p_avframe_yuv->data ,
        p_avframe_yuv->linesize
    );
    SDL_UpdateYUVTexture(texture ,
        &rect ,
        p_avframe_yuv->data[0] ,
        p_avframe_yuv->linesize[0] ,
        p_avframe_yuv->data[1] ,
        p_avframe_yuv->linesize[1] ,
        p_avframe_yuv->data[2] ,
        p_avframe_yuv->linesize[2]
    );
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer ,
        texture ,
        NULL ,
        &rect
    );

    SDL_RenderPresent(renderer);

    SDL_WaitEvent(event);
    // close file
    SDL_Quit();
    sws_freeContext(p_sws_ctx);
    av_free(buffer);
    av_frame_free(&p_avframe_yuv);
    av_frame_free(&p_avframe_raw);
    avcodec_free_context(&p_avcodec_ctx);
    return NULL;

}
//video decode thread
//1. decode packet to frame
//2. enqueue frame to vfq
void* videoDecode(void* arg)
{
    PlayerStatus* ps = (PlayerStatus*)arg;
    AVCodecContext* v_codecCtx = ps->v_codecCtx;
    AVPacket* pkt = NULL;
    AVFrame* raw_frame;
    AVFrame* yuv_frame;

    int ret;
    pkt = av_packet_alloc();
    raw_frame = av_frame_alloc();
    yuv_frame = av_frame_alloc();
    if (!pkt) logger(EXIT_FAILURE , "Failed to alloc packet.");
    if (!raw_frame) logger(EXIT_FAILURE , "Failed to alloc raw_frame.");
    if (!yuv_frame) logger(EXIT_FAILURE , "Failed to alloc yuv_frame.");
    while (1)
    {
        //1 send video packet to codec context
        ret = avcodec_send_packet(v_codecCtx , pkt);
        if (ret != 0)//failed to send packet to codecCtx
        {
            if (ret == AVERROR_EOF)
            {
                logger(LOG , "All video packets have been dequeued.");
                av_packet_unref(pkt);
                break;

            }
            else
            {
                logger(LOG , "Failed to send packet to v_codecCtx.");
                av_packet_unref(pkt);
                break;
            }
        }
        //2 receive a video frame from codec context
        ret = avcodec_receive_frame(v_codecCtx , raw_frame);
        if (ret) logger(EXIT_FAILURE , "Failed to receive frame from v_codecCtx.");

    }


}

int videoDisplay(PlayerStatus* ps)
{

}

int openVideo(PlayerStatus* ps)
{

    pthread_t videoDecodeThread;
    pthread_t videoPlayingThread;
    pthread_create(&videoDecodeThread , NULL , videoDecode , ps);
    pthread_create(&videoPlayingThread , NULL , videoPlaying , ps);

    return 1;
}