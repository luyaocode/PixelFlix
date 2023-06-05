/************************************************************************
 *ffplayer.c
 *detail:
 *  A simple ffmpeg player.
 *version: 0.0.2
 *  Add timer thread to make video more fluent
 *version: 0.0.3
 *  Play video and audio
 *  use audio packet queue and video packet queue
 *
 ************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#include <pthread.h>
#include <assert.h>

#define Packet_QUEUE_SIZE UINT32_MAX
#define AUDIO_BUFFER_SIZE 1024 // how many samples the audio_buf has
#define MAX_AUDIO_FRAME_SIZE 192000 //how many bytes
#define SDL_USERVENT_REFRESH (SDL_USEREVENT+1)

typedef struct globalParas
{
    AVFormatContext* p_avfmt_ctx;
    AVCodecContext* p_avcodec_ctx_video;
    AVCodecContext* p_avcodec_ctx_audio;
    AVCodec* p_avcodec_video;
    AVCodec* p_avcodec_audio;
    int v_idx;
    int a_idx;
}globalParas;


// exit
int exitCase(const char* c)
{
    printf("%s\n" , c);
    exit(EXIT_FAILURE);
}
typedef struct Node
{
    AVPacket packet;
    struct Node* next;
}Node;

typedef struct PacketQueue
{
    Node* head;
    Node* rear;
    uint32_t n;
    uint32_t max;
    uint32_t bytes;
    int (*destroy)(struct PacketQueue* q);
    int (*isEmpty)(struct PacketQueue* q);
    int (*isFull)(struct PacketQueue* q);
    int (*enqueue)(struct PacketQueue* q , AVPacket* p);
    int (*dequeue)(struct PacketQueue* q , AVPacket* p);
    SDL_mutex* mutex;
    SDL_cond* cond;
    bool blocked;
}PacketQueue;

typedef struct FF_AudioParas
{
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_second;

}FFAudioParas;


static PacketQueue q;
static PacketQueue vq;

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

int isEmpty(PacketQueue* q)
{
    if (q->head == q->rear) return 1;
    else return 0;
}
int isFull(PacketQueue* q)
{
    if (q->n == q->max) return 1;
    else return 0;
}
int destroy(PacketQueue* q)
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
int enqueue(PacketQueue* q , AVPacket* p)
{
    if (av_packet_make_refcounted(p))
    {
        fprintf(stderr , "Failed to set packet reference counted.\n");
    }
    if (isFull(q)) return 0;

    Node* pnode = (Node*)av_malloc(sizeof(Node));
    if (!pnode)
    {
        fprintf(stderr , "Failed to malloc Node.\n");
    }
    SDL_LockMutex(q->mutex);
    pnode->packet = *p;
    pnode->next = NULL;
    q->rear->next = pnode;
    q->rear = q->rear->next;
    q->rear->next = NULL;
    q->n++;
    q->bytes += sizeof(*pnode);
    signal = true;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    printf("en: n=%d, size=%d\n" , q->n , q->bytes);
    return 1;
}

int dequeue(PacketQueue* q , AVPacket* p)
{
    Node* temp = NULL;
    int res;
    SDL_LockMutex(q->mutex);
    while (1)
    {
        temp = q->head->next;
        if (temp != NULL)// n>0
        {
            *p = temp->packet;
            q->head->next = temp->next;
            temp->next = NULL;
            if (temp == q->rear) q->rear = q->head;//if n=1, q->rear should be q->head after dequeue.
            q->bytes -= sizeof(*temp);
            q->n--;
            printf("de: n=%d, size=%d\n" , q->n , q->bytes);
            res = 1;
            break;
        }
        else if (isStreamFinished || q->blocked) //n=0 and (stream is over or queue is blocked)
        {
            res = 0;
            break;
        }
        else //n=0 and (stream is not over and queue is not blocked)
        {
            while (!signal)
            {
                SDL_CondWait(q->cond , q->mutex);
            }
        }
    }
    SDL_UnlockMutex(q->mutex);
    return res;
}

int init(PacketQueue* q)
{
    if (!q->head)
    {
        Node* head = (Node*)malloc(sizeof(Node));
        if (!head)
        {
            fprintf(stderr , "Failed to init PacketQueue.\n");
            return 0;
        }
        head->next = NULL;
        q->head = head;
    }
    q->rear = q->head;
    q->n = 0;
    q->max = Packet_QUEUE_SIZE;
    q->bytes = 0;
    q->destroy = destroy;
    q->isEmpty = isEmpty;
    q->isFull = isFull;
    q->dequeue = dequeue;
    q->enqueue = enqueue;
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
    q->blocked = false;
    return 1;
}

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
            if (!q.dequeue(&q , pkt))
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


pthread_t timerThread;
pthread_t videoThread;

//timer thread
void* timer(void* arg)
{
    uint32_t* time = (uint32_t*)arg;
    while (1) {
        SDL_Event event;
        event.type = SDL_USERVENT_REFRESH;
        SDL_PushEvent(&event);
        SDL_Delay(*time);
    }
}
//Audio
// It is generally not recommended to create a separate audio thread solely for handling the decoding of audio packets in SDL.
// The reason is that SDL already provides an audio callback mechanism that allows you to handle audio decoding and playback in a more efficient and synchronized manner.
// SDL's audio callback allows you to supply a callback function that gets called periodically to fill the audio buffer with decoded audio data.
// This callback is called from the main thread by SDL's audio subsystem, ensuring proper synchronization with the audio playback.
// With this approach, SDL's audio subsystem takes care of scheduling and playing back the audio, while your callback function is responsible for decoding and filling the audio buffer.
// This eliminates the need for creating a separate audio thread, as the audio decoding and playback are handled within the main thread.
// Remember to properly manage the synchronization between audio decoding and video rendering if you're dealing with synchronized audio and video playback.
int openAudioStream(void* arg)
{
    globalParas* gbl = (globalParas*)arg;

    AVCodecContext* codecCtx = gbl->p_avcodec_ctx_audio;
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

    if (SDL_OpenAudio(&desiredSpec , &obtainedSpec)) exitCase("Failed to open audio device.\n");
    //Build audio resampling parameters based on SDL audio parameters.
    tgtParas.fmt = AV_SAMPLE_FMT_S16;
    tgtParas.freq = obtainedSpec.freq;
    tgtParas.channel_layout = av_get_default_channel_layout(obtainedSpec.channels);;
    tgtParas.channels = obtainedSpec.channels;
    tgtParas.frame_size = av_samples_get_buffer_size(NULL , obtainedSpec.channels , 1 , tgtParas.fmt , 1);
    tgtParas.bytes_per_second = av_samples_get_buffer_size(NULL , obtainedSpec.channels , obtainedSpec.freq , tgtParas.fmt , 1);
    if (tgtParas.bytes_per_second <= 0 || tgtParas.frame_size <= 0) exitCase("Failed to get buffer size.\n");
    srcParas = tgtParas;

    SDL_PauseAudio(0);
    return 1;
}

//video thread
void* playVideo(void* arg)
{
    globalParas* gbl = (globalParas*)arg;

    // AVCodec* p_avcodec = gbl->p_avcodec_video;
    AVCodecContext* p_avcodec_ctx = gbl->p_avcodec_ctx_video;

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
        "FFmpeg player demo" ,
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
    if (!p_avpacket) exitCase("Failed to alloc packet.\n");
    while (1)
    {
        // dequeue a video packet
        ret = vq.dequeue(&vq , p_avpacket);
        if (ret != 1)
        {
            continue;
        }
        //send video packet to codec context
        ret = avcodec_send_packet(p_avcodec_ctx , p_avpacket);
        if (ret != 0)
        {
            if (ret == AVERROR_EOF)
            {
                printf("All video packets have been dequeued.\n");
                av_packet_unref(p_avpacket);
                break;

            }
            else
            {
                printf("Failed to send packet to codec context: %d\n" , ret);
                av_packet_unref(p_avpacket);

                exit(EXIT_FAILURE);
            }
        }
        //receive a video frame from codec context
        ret = avcodec_receive_frame(p_avcodec_ctx , p_avframe_raw);
        if (ret)
        {
            printf("Failed to decode packet: %d\n" , ret);
            //exit(EXIT_FAILURE);
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

    }

    // close file
    SDL_Quit();
    sws_freeContext(p_sws_ctx);
    av_free(buffer);
    av_frame_free(&p_avframe_yuv);
    av_frame_free(&p_avframe_raw);

    avcodec_free_context(&p_avcodec_ctx);
    return NULL;
}

int main(int argc , char* argv[])
{

    if (argc < 2) exitCase("Need more para.");
    const char* path = argv[1];
    printf("File Name: %s\n" , path);

    //Init
    AVFormatContext* p_avfmt_ctx = NULL;
    AVCodecParameters* p_avcodec_para = NULL;
    AVCodecContext* p_avcodec_ctx = NULL;
    AVCodecContext* codecCtx = NULL;//audio
    AVCodec* p_avcodec = NULL;
    AVCodec* codec = NULL;//audio
    AVPacket* p_packet = NULL;


    int ret = 0;

    // open file
    ret = avformat_open_input(&p_avfmt_ctx , path , NULL , NULL);
    if (ret)
    {
        printf("Failed to open file.\n");
        exit(EXIT_FAILURE);
    }
    ret = avformat_find_stream_info(p_avfmt_ctx , NULL);
    if (ret < 0)
    {
        printf("Failed to find stream info.\n");
        exit(EXIT_FAILURE);
    }

    // print file info to stadard error stream
    av_dump_format(p_avfmt_ctx , 0 , path , 0);

    // find video stream and audio stream
    int v_idx = -1;
    int a_idx = -1;

    for (uint32_t i = 0; i < p_avfmt_ctx->nb_streams; i++)
    {
        if (p_avfmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            v_idx = i;
        }
        else if (p_avfmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            a_idx = i;
        }
    }
    if (v_idx == -1)
    {
        printf("No video stream.\n");
    }
    if (v_idx == -1)
    {
        printf("No video stream.\n");
    }

    printf("video stream index:%d\n" , v_idx);
    printf("audio stream index:%d\n" , a_idx);
    // get video codec context
    p_avcodec_para = p_avfmt_ctx->streams[v_idx]->codecpar;
    p_avcodec = avcodec_find_decoder(p_avcodec_para->codec_id);
    if (!p_avcodec)
    {
        printf("Failed to get a codec.\n");
        exit(EXIT_FAILURE);
    }
    p_avcodec_ctx = avcodec_alloc_context3(p_avcodec);
    ret = avcodec_parameters_to_context(p_avcodec_ctx , p_avcodec_para);
    if (ret < 0)
    {
        printf("Falied to fill codec context.\n");
        exit(EXIT_FAILURE);
    }

    ret = avcodec_open2(p_avcodec_ctx , p_avcodec , NULL);
    if (ret < 0)
    {
        printf("Failed to initialize codec context.\n");
        exit(EXIT_FAILURE);
    }

    //get audio codec context
    codec = avcodec_find_decoder(p_avfmt_ctx->streams[a_idx]->codecpar->codec_id);
    if (!codec) exitCase("Failed to find a codec.");
    codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx , p_avfmt_ctx->streams[a_idx]->codecpar);
    if (avcodec_open2(codecCtx , codec , NULL) != 0) exitCase("Failed to initialize audio codec context.");

    AVRational frameRate = av_guess_frame_rate(p_avfmt_ctx , p_avfmt_ctx->streams[v_idx] , NULL);
    printf("Video Frame Rate: %d/%d\n" , frameRate.num , frameRate.den);
    uint32_t delay = frameRate.den * 1000 / frameRate.num;
    printf("delay: %d\n" , delay);


    globalParas gbl;
    gbl.p_avfmt_ctx = p_avfmt_ctx;
    gbl.p_avcodec_video = p_avcodec;
    gbl.p_avcodec_audio = codec;
    gbl.p_avcodec_ctx_video = p_avcodec_ctx;
    gbl.p_avcodec_ctx_audio = codecCtx;
    gbl.a_idx = a_idx;
    gbl.v_idx = v_idx;

    //init queue
    if (!init(&q)) exitCase("Failed to initilize audio queue.");
    if (!init(&vq)) exitCase("Failed to initilize video queue.");


    //initilize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
    {
        printf("Failed to initilize SDL.\n");
        exit(EXIT_FAILURE);
    }


    //create timer thread
    pthread_create(&timerThread , NULL , timer , &delay);

    //open audio stream
    openAudioStream(&gbl);
    //create video stread
    pthread_create(&videoThread , NULL , playVideo , &gbl);

    //put packet into video queue or audio queue
    p_packet = av_packet_alloc();
    if (!p_packet) exitCase("Failed to alloc packet.");
    while (1)
    {
        ret = av_read_frame(p_avfmt_ctx , p_packet);
        if (ret == 0) //Ok
        {
            if (p_packet->stream_index == v_idx)//video packet
            {
                vq.enqueue(&vq , p_packet);
            }
            else if (p_packet->stream_index == a_idx)//audio packet
            {
                q.enqueue(&q , p_packet);
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
            isStreamFinished = true;
            printf("All packets have been enqueued.\n");
            av_packet_unref(p_packet);
            break;
        }

    }
    pthread_join(videoThread , NULL);

    while (!isAudioDecodeFinished || !isVideoDecodeFinished)
    {
        SDL_Delay(100);
    }

    //release resources
    q.destroy(&q);
    q.destroy(&vq);

    avformat_close_input(&p_avfmt_ctx);
    return 0;

}