#include "audio.h"
#include "player.h"
#include "logger.h"
#include "queue.h"
#include "SDL2/SDL.h"
#include <assert.h>


#define Packet_QUEUE_SIZE UINT32_MAX
#define AUDIO_BUFFER_SIZE 1024 // how many samples the audio_buf has
#define MAX_AUDIO_FRAME_SIZE 192000 //how many bytes
#define SDL_USERVENT_REFRESH (SDL_USEREVENT+1)

// exit
int exitCase(const char* c)
{
    printf("%s\n" , c);
    exit(EXIT_FAILURE);
}



static FFAudioParas srcParas;
static FFAudioParas tgtParas;
static bool isStreamFinished = false;
static bool isAudioDecodeFinished = false;
static bool isVideoDecodeFinished = false;
static struct SwrContext* swrCtx;
static bool signal = false;

//resampling
static uint8_t* resample_buf = NULL;
static unsigned int resample_buf_len = 0;


//audio decode packet
//positive number on success, means the frame size that decoded
//-1 on failure
//usually av_send_packet to codec then receive frame from
int audioDecodePacket(AVCodecContext* ctx , AVPacket* pkt , uint8_t* audio_buf , int buf_size)
{
    AVFrame* pf = av_frame_alloc();
    if (!pf) exitCase("Failed to allocate AVFrame.");
    int res = 0;
    int nb_samples;
    uint8_t* p_cp_buf = NULL;
    int cp_len;
    int frm_size = 0;
    bool resend = true;

    while (resend)
    {
        //send packet to codec
        res = avcodec_send_packet(ctx , pkt);
        if (res != 0)
        {
            fprintf(stderr , "Failed to send packet to decoder.\n");
            av_packet_unref(pkt);
            res = -1;
            return res;
        }
        //receive frame from codec
        while (1)
        {
            res = avcodec_receive_frame(ctx , pf);
            printf("res:%d\n" , res);
            if (res != 0)
            {
                if (res == AVERROR(EAGAIN))//pkt decoded
                {
                    resend = false;
                    break;
                }
                else if (res == AVERROR_EOF)
                {
                    printf("the decoder has been fully flushed, and there will be no more output frames\n");
                    return res;
                }
                else if (res == AVERROR(EINVAL))
                {
                    printf("codec not opened, or it is an encoder\n");
                    return res;
                }
                else if (res == AVERROR_INPUT_CHANGED)
                {
                    printf("current decoded frame has changed parameters with respect to first decoded frame. Applicable when flag AV_CODEC_FLAG_DROPCHANGED is set\n");
                    return res;
                }
                else
                {
                    printf("legitimate decoding errors\n");
                    return res;
                }
            }
            else//res==0
            {
                // s_audio_param_tgt是SDL可接受的音频帧数，是main()中取得的参数
                // 在main()函数中又有“s_audio_param_src = s_audio_param_tgt”
                // 此处表示：如果frame中的音频参数 == s_audio_param_src == s_audio_param_tgt，那音频重采样的过程就免了(因此时s_audio_swr_ctx是NULL)
                // 否则使用frame(源)和s_audio_param_src(目标)中的音频参数来设置s_audio_swr_ctx，并使用frame中的音频参数来赋值s_audio_param_src
                if (pf->format != srcParas.fmt ||
                    (int64_t)pf->channel_layout != srcParas.channel_layout ||
                    pf->sample_rate != srcParas.freq)
                {
                    swr_free(&swrCtx);
                    swrCtx = swr_alloc_set_opts(NULL ,
                        tgtParas.channel_layout ,
                        tgtParas.fmt ,
                        tgtParas.freq ,
                        pf->channel_layout ,
                        (enum AVSampleFormat)pf->format ,
                        pf->sample_rate ,
                        0 ,
                        NULL);

                    if (swrCtx == NULL || swr_init(swrCtx) < 0)
                    {
                        printf("Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n" ,
                            pf->sample_rate , av_get_sample_fmt_name((enum AVSampleFormat)pf->format) , pf->channels ,
                            tgtParas.freq , av_get_sample_fmt_name(tgtParas.fmt) , tgtParas.channels);
                        swr_free(&swrCtx);
                        return -1;
                    }

                    // 使用frame中的参数更新s_audio_param_src，第一次更新后后面基本不用执行此if分支了，因为一个音频流中各frame通用参数一样
                    srcParas.channel_layout = pf->channel_layout;
                    srcParas.channels = pf->channels;
                    srcParas.freq = pf->sample_rate;
                    srcParas.fmt = (enum AVSampleFormat)pf->format;
                }

                if (swrCtx != NULL)        // 重采样
                {
                    // 重采样输入参数1：输入音频样本数是p_frame->nb_samples
                    // 重采样输入参数2：输入音频缓冲区
                    const uint8_t** in = (const uint8_t**)pf->extended_data;
                    // 重采样输出参数1：输出音频缓冲区尺寸
                    // 重采样输出参数2：输出音频缓冲区
                    uint8_t** out = &resample_buf;
                    // 重采样输出参数：输出音频样本数(多加了256个样本)
                    int out_count = (int64_t)pf->nb_samples * tgtParas.freq / pf->sample_rate + 256;
                    // 重采样输出参数：输出音频缓冲区尺寸(以字节为单位)
                    int out_size = av_samples_get_buffer_size(NULL , tgtParas.channels , out_count , tgtParas.fmt , 0);
                    if (out_size < 0)
                    {
                        printf("av_samples_get_buffer_size() failed\n");
                        return -1;
                    }

                    if (resample_buf == NULL)
                    {
                        av_fast_malloc(&resample_buf , &resample_buf_len , out_size);
                    }
                    if (resample_buf == NULL)
                    {
                        return AVERROR(ENOMEM);
                    }
                    // 音频重采样：返回值是重采样后得到的音频数据中单个声道的样本数
                    nb_samples = swr_convert(swrCtx , out , out_count , in , pf->nb_samples);
                    if (nb_samples < 0) {
                        printf("swr_convert() failed\n");
                        return -1;
                    }
                    if (nb_samples == out_count)
                    {
                        printf("audio buffer is probably too small\n");
                        if (swr_init(swrCtx) < 0)
                            swr_free(&swrCtx);
                    }

                    // 重采样返回的一帧音频数据大小(以字节为单位)
                    p_cp_buf = resample_buf;
                    cp_len = nb_samples * tgtParas.channels * av_get_bytes_per_sample(tgtParas.fmt);
                }
                else    // 不重采样
                {
                    // 根据相应音频参数，获得所需缓冲区大小
                    frm_size = av_samples_get_buffer_size(
                        NULL ,
                        ctx->channels ,
                        pf->nb_samples ,
                        ctx->sample_fmt ,
                        1);

                    printf("frame size %d, buffer size %d\n" , frm_size , buf_size);
                    assert(frm_size <= buf_size);

                    p_cp_buf = pf->data[0];
                    cp_len = frm_size;
                }

                // 将音频帧拷贝到函数输出参数audio_buf
                memcpy(audio_buf , p_cp_buf , cp_len);
                res = cp_len;
                printf("cp_len:%d\n" , res);
                return res;
            }

        }
    }
    return res;
}


// SDL audio callback function
// It will be called when SDL device need more audio frame.
// You should send len bytes data to SDL stream every time when callback function is called.
// Once the callback returns, the buffer will no longer be valid, so you need to fill the SDL stream
// with len bytes before callback function reachs the end and don't forget to release relevent resource handly.
// The callback must completely initialize the buffer; as of SDL 2.0, this buffer is not initialized before the
// callback is called.If there is nothing to play , the callback should fill the buffer with silence. you can use
// `memset(audio_buf , 0 , 1024)` to obtain the goal.
// Callback function's frequency of calling is usually between tens and hundreds of times per second, it depends on
// SDL_AudioSpec's fields such as `freq`, `samples` and other factors such as hardware performance.
//
void audioCallback(void* userdata , uint8_t* stream , int len)
{
    printf("len:%d\n" , len);
    AVCodecContext* codecCtx = (AVCodecContext*)userdata;
    if (!userdata) exitCase("Failed to get codec context.\n");

    //add
    Queue* apq = &player_status.apq;

    int copyLen;
    int getSize;
    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3 / 2)];//audio frame buffer, should be enough to stored every audio frame
    static uint32_t received = 0;//how many bytes haved received from decoder, usually be the size of an audio frame
    static uint32_t send = 0;//how many bytes have sent to SDL `stream`, SDL stream need `len` bytes every time when callback function is called
    AVPacket* pkt = NULL;

    while (len > 0)
    {
        if (isAudioDecodeFinished) return;
        if (send >= received)
        {
            //allocate a heap space to store packet
            pkt = (AVPacket*)av_malloc(sizeof(AVPacket));
            if (!pkt) exitCase("Failed to malloc packet.");


            //get a packet
            if (!apq->dequeue(apq , (void**)&pkt))
            {
                //if stream has all been read, pkt=NULL
                if (isStreamFinished)
                {
                    av_packet_unref(pkt);
                    pkt = NULL;    // flush decoder
                }
                else
                {
                    av_packet_unref(pkt);
                }
            }

            //decode packet
            getSize = audioDecodePacket(codecCtx , pkt , audio_buf , sizeof(audio_buf));
            if (getSize > 0)
            {
                received = getSize;
                send = 0;
                av_packet_unref(pkt);
            }
            else if (getSize == 0 || getSize == AVERROR_EOF) //if getSize err, silence should be put into audio_buf
            {
                isAudioDecodeFinished = true;
                printf("All packets have been decoded.\n");

            }
            else
            {
                memset(audio_buf , 0 , 1024);
                av_packet_unref(pkt);
            }
        }
        copyLen = received - send;
        if (copyLen > len)
        {
            copyLen = len;
        }

        // 将解码后的音频帧(s_audio_buf+)写入音频设备缓冲区(stream)，播放
        memcpy(stream , (uint8_t*)audio_buf + send , copyLen);
        len -= copyLen;
        stream += copyLen;
        send += copyLen;
    }
    av_free(pkt);
}
int openAudio(PlayerStatus* ps)
{
    AVCodecContext* codecCtx = ps->a_codecCtx;

    SDL_AudioSpec desiredSpec;
    SDL_AudioSpec obtainedSpec;

    //audio
    // desiredSpec.size is autoly caculated by size=samples * channels * (bytes per sample)
    desiredSpec.freq = codecCtx->sample_rate;
    desiredSpec.format = AUDIO_S16SYS;//unsigned 16-bit samples in native byte order
    desiredSpec.channels = codecCtx->channels;
    desiredSpec.silence = 0;
    desiredSpec.samples = AUDIO_BUFFER_SIZE;//must be power of 2
    desiredSpec.callback = audioCallback;
    desiredSpec.userdata = codecCtx;

    if (SDL_OpenAudio(&desiredSpec , &obtainedSpec)) logger(EXIT_FAILURE , "Failed to open audio device.\n");
    //Build audio resampling parameters based on SDL audio parameters.
    tgtParas.fmt = AV_SAMPLE_FMT_S16;
    tgtParas.freq = obtainedSpec.freq;
    tgtParas.channel_layout = av_get_default_channel_layout(obtainedSpec.channels);;
    tgtParas.channels = obtainedSpec.channels;
    tgtParas.frame_size = av_samples_get_buffer_size(NULL , obtainedSpec.channels , 1 , tgtParas.fmt , 1);
    tgtParas.bytes_per_second = av_samples_get_buffer_size(NULL , obtainedSpec.channels , obtainedSpec.freq , tgtParas.fmt , 1);
    if (tgtParas.bytes_per_second <= 0 || tgtParas.frame_size <= 0) logger(EXIT_FAILURE , "Failed to get buffer size.\n");
    srcParas = tgtParas;

    SDL_PauseAudio(0);
    return 1;
}