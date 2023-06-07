#include "logger.h"
#include "player.h"
#include "demux.h"
#include "audio.h"
#include "video.h"
#include "queue.h"
#include <stdlib.h>
#include <stdbool.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#include <pthread.h>
#include <assert.h>

PlayerStatus player_status;

//create demux thread and audio thread

//return 1 on success, -1 on failure
int playerInit(const char* c)
{
    if (c == NULL || c[0] == '\0') logger(EXIT_FAILURE , "Failed to get file path.");
    const char* path = c;
    AVFormatContext* fmtCtx = NULL;//Must be set NULL or it will raise `Segmentation fault`
    AVCodec* v_codec = NULL;
    AVCodec* a_codec = NULL;
    AVCodecContext* v_codecCtx = NULL;
    AVCodecContext* a_codecCtx = NULL;
    AVCodecParameters* v_codecParas;
    AVCodecParameters* a_codecParas;

    int res = DEFAULT_VALUE;
    int v_idx = DEFAULT_VALUE;
    int a_idx = DEFAULT_VALUE;

    if (avformat_open_input(&fmtCtx , path , NULL , NULL)) logger(EXIT_FAILURE , "Failed to open file.");
    if (avformat_find_stream_info(fmtCtx , NULL) < 0) logger(EXIT_FAILURE , "Failed to find stream info.");
    av_dump_format(fmtCtx , 0 , NULL , 0);

    for (uint32_t i = 0; i < fmtCtx->nb_streams; i++)
    {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            v_idx = i;
        }
        else if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            a_idx = i;
        }
    }
    if (v_idx == DEFAULT_VALUE) logger(LOG , "No video stream.");
    if (a_idx == DEFAULT_VALUE) logger(LOG , "No audio stream.");
    logger(LOG , "Video idx: %d, Audio idx: %d" , v_idx , a_idx);

    // get video codecCtx
    v_codecParas = fmtCtx->streams[v_idx]->codecpar;
    v_codec = avcodec_find_decoder(v_codecParas->codec_id);
    if (!v_codec) logger(EXIT_FAILURE , "Failed to find decoder.");
    v_codecCtx = avcodec_alloc_context3(v_codec);
    if (avcodec_parameters_to_context(v_codecCtx , v_codecParas) < 0) logger(EXIT_FAILURE , "Failed.");
    if (avcodec_open2(v_codecCtx , v_codec , NULL) < 0) logger(EXIT_FAILURE , "Failed");

    // get audio codecCtx
    a_codecParas = fmtCtx->streams[a_idx]->codecpar;
    a_codec = avcodec_find_decoder(a_codecParas->codec_id);
    if (!a_codec) logger(EXIT_FAILURE , "Failed to find decoder.");
    a_codecCtx = avcodec_alloc_context3(a_codec);
    if (avcodec_parameters_to_context(a_codecCtx , a_codecParas) < 0) logger(EXIT_FAILURE , "Failed.");
    if (avcodec_open2(a_codecCtx , a_codec , NULL) < 0) logger(EXIT_FAILURE , "Failed");

    AVRational frameRate = av_guess_frame_rate(fmtCtx , fmtCtx->streams[v_idx] , NULL);
    logger(LOG , "Video Frame Rate: %d/%d\n" , frameRate.num , frameRate.den);
    uint32_t delay = frameRate.den * 1000 / frameRate.num;
    logger(LOG , "delay: %d\n" , delay);

    //init player_status
    player_status.isStreamFinished = false;
    player_status.signal = false;
    player_status.isAudioDecodeFinished = false;
    player_status.isVideoDecodeFinished = false;
    player_status.fmtCtx = fmtCtx;
    player_status.a_codecCtx = a_codecCtx;
    player_status.v_codecCtx = v_codecCtx;
    player_status.a_codec = a_codec;
    player_status.v_codec = v_codec;
    player_status.a_idx = a_idx;
    player_status.v_idx = v_idx;
    player_status.swrCtx = NULL;


    //init queue
    Queue* vpq = &player_status.vpq;
    Queue* apq = &player_status.apq;
    Queue* vfq = &player_status.vfq;
    Queue* afq = &player_status.afq;

    if (!init(AVPACKET , vpq)) logger(EXIT_FAILURE , "Failed to initilize video packet queue.");
    if (!init(AVPACKET , apq)) logger(EXIT_FAILURE , "Failed to initilize audio packet queue.");
    if (!init(AVFRAME , vfq)) logger(EXIT_FAILURE , "Failed to initilize video frame queue.");
    if (!init(AVFRAME , afq)) logger(EXIT_FAILURE , "Failed to initilize audio frame queue.");

    //init SDL subsystem
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) logger(EXIT_FAILURE , "Failed to init SDL subsystem.");
    //open demux thread
    openDemux(&player_status);
    //open audio thread
    openAudio(&player_status);
    //open vidoe thread
    openVideo(&player_status);

    res = 1;
    return res;
}
int playerDeinit()
{

}

int playerPause()
{
    return 1;
}
int playerExit()
{
    return 1;
}

int playerRun(const char* c)
{
    if (c == NULL || c[0] == '\0') logger(EXIT_FAILURE , "Failed to get file path.");
    const char* path = c;

    playerInit(path);

    //handle events
    SDL_Event event;
    while (1)
    {
        SDL_PumpEvents();
        while (!SDL_PollEvent(&event))//have event
        {
            av_usleep(1000000);
            SDL_PumpEvents();
        }
        switch (event.type)
        {
        case SDL_KEYDOWN:
        {
            if (event.key.keysym.sym == SDLK_ESCAPE)
            {
                playerExit();
                break;
            }
            else if (event.key.keysym.sym == SDLK_SPACE)
            {
                playerPause();
            }
        }
        case SDL_WINDOWEVENT:
        {

            break;
        }
        default:break;
        }

    }

    return 0;


}