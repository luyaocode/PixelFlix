/*******************************************************************************
 * ffplayer.c
 *
 * history:
 *   2018-11-27 - [lei]     Create file: a simplest ffmpeg player
 *   2018-12-01 - [lei]     Playing audio
 *   2019-01-06 - [lei]     Add audio resampling, fix bug of unsupported audio
 *                          format(such as planar)
 *
 * details:
 *   A simple ffmpeg player.
 *
 * refrence:
 *   1. https://blog.csdn.net/leixiaohua1020/article/details/38868499
 *   2. http://dranger.com/ffmpeg/ffmpegtutorial_all.html#tutorial01.html
 *   3. http://dranger.com/ffmpeg/ffmpegtutorial_all.html#tutorial02.html
 *   4. http://dranger.com/ffmpeg/ffmpegtutorial_all.html#tutorial03.html
 *******************************************************************************/
#define SDL_MAIN_HANDLED
extern "C"
{
    #include <stdlib.h>
    #include <stdbool.h>
    #include <SDL.h>
    #include <assert.h>
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/frame.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/mem.h>
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
}
#include <thread>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

typedef struct packet_queue_t
{
    AVPacketList* first_pkt;
    AVPacketList* last_pkt;
    int nb_packets;   // ������AVPacket�ĸ���
    int size;         // ������AVPacket�ܵĴ�С(�ֽ���)
    SDL_mutex* mutex;
    SDL_cond* cond;
} packet_queue_t;

typedef struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} FF_AudioParams;

static packet_queue_t s_audio_pkt_queue;
static FF_AudioParams s_audio_param_src;
static FF_AudioParams s_audio_param_tgt;
static struct SwrContext* s_audio_swr_ctx;
static uint8_t* s_resample_buf = NULL;  // �ز������������
static unsigned int s_resample_buf_len = 0;      // �ز����������������

static bool s_input_finished = false;   // �ļ���ȡ���
static bool s_decode_finished = false;  // �������

void packet_queue_init(packet_queue_t* q)
{
    memset(q, 0, sizeof(packet_queue_t));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

// д����β����pkt��һ����δ�������Ƶ����
int packet_queue_push(packet_queue_t* q, AVPacket* pkt)
{
    AVPacketList* pkt_list;

    if (av_packet_make_refcounted(pkt) < 0)
    {
        printf("[pkt] is not refrence counted\n");
        return -1;
    }
    pkt_list = (AVPacketList*)av_malloc(sizeof(AVPacketList));
    if (!pkt_list)
    {
        return -1;
    }

    pkt_list->pkt = *pkt;
    pkt_list->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)   // ����Ϊ��
    {
        q->first_pkt = pkt_list;
    }
    else
    {
        q->last_pkt->next = pkt_list;
    }
    q->last_pkt = pkt_list;
    q->nb_packets++;
    q->size += pkt_list->pkt.size;
    // ���������������źţ������ȴ�q->cond����������һ���߳�
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

// ������ͷ����
int packet_queue_pop(packet_queue_t* q, AVPacket* pkt, int block)
{
    AVPacketList* p_pkt_node;
    int ret;

    SDL_LockMutex(q->mutex);

    while (1)
    {
        p_pkt_node = q->first_pkt;
        if (p_pkt_node)             // ���зǿգ�ȡһ������
        {
            q->first_pkt = p_pkt_node->next;
            if (!q->first_pkt)
            {
                q->last_pkt = NULL;
            }
            q->nb_packets--;
            q->size -= p_pkt_node->pkt.size;
            *pkt = p_pkt_node->pkt;
            av_free(p_pkt_node);
            ret = 1;
            break;
        }
        else if (s_input_finished)  // �����ѿգ��ļ��Ѵ�����
        {
            ret = 0;
            break;
        }
        else if (!block)            // ���п���������־��Ч���������˳�
        {
            ret = 0;
            break;
        }
        else                        // ���п���������־��Ч����ȴ�
        {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

int audio_decode_frame(AVCodecContext* p_codec_ctx, AVPacket* p_packet, uint8_t* audio_buf, int buf_size)
{
    AVFrame* p_frame = av_frame_alloc();

    int frm_size = 0;
    int res = 0;
    int ret = 0;
    int nb_samples = 0;             // �ز������������
    uint8_t* p_cp_buf = NULL;
    int cp_len = 0;
    bool need_new = false;

    res = 0;
    while (1)
    {
        need_new = false;

        // 1 ���ս�������������ݣ�ÿ�ν���һ��frame
        ret = avcodec_receive_frame(p_codec_ctx, p_frame);
        if (ret != 0)
        {
            if (ret == AVERROR_EOF)
            {
                printf("audio avcodec_receive_frame(): the decoder has been fully flushed\n");
                res = 0;
                goto exit;
            }
            else if (ret == AVERROR(EAGAIN))
            {
                //printf("audio avcodec_receive_frame(): output is not available in this state - "
                //       "user must try to send new input\n");
                need_new = true;
            }
            else if (ret == AVERROR(EINVAL))
            {
                printf("audio avcodec_receive_frame(): codec not opened, or it is an encoder\n");
                res = -1;
                goto exit;
            }
            else
            {
                printf("audio avcodec_receive_frame(): legitimate decoding errors\n");
                res = -1;
                goto exit;
            }
        }
        else
        {
            // s_audio_param_tgt��SDL�ɽ��ܵ���Ƶ֡������main()��ȡ�õĲ���
            // ��main()���������С�s_audio_param_src = s_audio_param_tgt��
            // �˴���ʾ�����frame�е���Ƶ���� == s_audio_param_src == s_audio_param_tgt������Ƶ�ز����Ĺ��̾�����(���ʱs_audio_swr_ctx��NULL)
            // ��������������ʹ��frame(Դ)��s_audio_param_src(Ŀ��)�е���Ƶ����������s_audio_swr_ctx����ʹ��frame�е���Ƶ��������ֵs_audio_param_src
            if (p_frame->format != s_audio_param_src.fmt ||
                p_frame->channel_layout != s_audio_param_src.channel_layout ||
                p_frame->sample_rate != s_audio_param_src.freq)
            {
                swr_free(&s_audio_swr_ctx);
                // ʹ��frame(Դ)��is->audio_tgt(Ŀ��)�е���Ƶ����������is->swr_ctx
                s_audio_swr_ctx = swr_alloc_set_opts(NULL,
                    s_audio_param_tgt.channel_layout,
                    s_audio_param_tgt.fmt,
                    s_audio_param_tgt.freq,
                    p_frame->channel_layout,
                    (enum AVSampleFormat)p_frame->format,
                    p_frame->sample_rate,
                    0,
                    NULL);
                if (s_audio_swr_ctx == NULL || swr_init(s_audio_swr_ctx) < 0)
                {
                    printf("Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                        p_frame->sample_rate, av_get_sample_fmt_name((enum AVSampleFormat)p_frame->format), p_frame->channels,
                        s_audio_param_tgt.freq, av_get_sample_fmt_name(s_audio_param_tgt.fmt), s_audio_param_tgt.channels);
                    swr_free(&s_audio_swr_ctx);
                    return -1;
                }

                // ʹ��frame�еĲ�������s_audio_param_src����һ�θ��º�����������ִ�д�if��֧�ˣ���Ϊһ����Ƶ���и�frameͨ�ò���һ��
                s_audio_param_src.channel_layout = p_frame->channel_layout;
                s_audio_param_src.channels = p_frame->channels;
                s_audio_param_src.freq = p_frame->sample_rate;
                s_audio_param_src.fmt = (enum AVSampleFormat)p_frame->format;
            }

            if (s_audio_swr_ctx != NULL)        // �ز���
            {
                // �ز����������1��������Ƶ��������p_frame->nb_samples
                // �ز����������2��������Ƶ������
                const uint8_t** in = (const uint8_t**)p_frame->extended_data;
                // �ز����������1�������Ƶ�������ߴ�
                // �ز����������2�������Ƶ������
                uint8_t** out = &s_resample_buf;
                // �ز�����������������Ƶ������(�����256������)
                int out_count = (int64_t)p_frame->nb_samples * s_audio_param_tgt.freq / p_frame->sample_rate + 256;
                // �ز�����������������Ƶ�������ߴ�(���ֽ�Ϊ��λ)
                int out_size = av_samples_get_buffer_size(NULL, s_audio_param_tgt.channels, out_count, s_audio_param_tgt.fmt, 0);
                if (out_size < 0)
                {
                    printf("av_samples_get_buffer_size() failed\n");
                    return -1;
                }

                if (s_resample_buf == NULL)
                {
                    av_fast_malloc(&s_resample_buf, &s_resample_buf_len, out_size);
                }
                if (s_resample_buf == NULL)
                {
                    return AVERROR(ENOMEM);
                }
                // ��Ƶ�ز���������ֵ���ز�����õ�����Ƶ�����е���������������
                nb_samples = swr_convert(s_audio_swr_ctx, out, out_count, in, p_frame->nb_samples);
                if (nb_samples < 0) {
                    printf("swr_convert() failed\n");
                    return -1;
                }
                if (nb_samples == out_count)
                {
                    printf("audio buffer is probably too small\n");
                    if (swr_init(s_audio_swr_ctx) < 0)
                        swr_free(&s_audio_swr_ctx);
                }

                // �ز������ص�һ֡��Ƶ���ݴ�С(���ֽ�Ϊ��λ)
                p_cp_buf = s_resample_buf;
                cp_len = nb_samples * s_audio_param_tgt.channels * av_get_bytes_per_sample(s_audio_param_tgt.fmt);
            }
            else    // ���ز���
            {
                // ������Ӧ��Ƶ������������軺������С
                frm_size = av_samples_get_buffer_size(
                    NULL,
                    p_codec_ctx->channels,
                    p_frame->nb_samples,
                    p_codec_ctx->sample_fmt,
                    1);

                printf("frame size %d, buffer size %d\n", frm_size, buf_size);
                assert(frm_size <= buf_size);

                p_cp_buf = p_frame->data[0];
                cp_len = frm_size;
            }

            // ����Ƶ֡�����������������audio_buf
            memcpy(audio_buf, p_cp_buf, cp_len);

            res = cp_len;
            goto exit;
        }

        // 2 �������ι���ݣ�ÿ��ιһ��packet
        if (need_new)
        {
            ret = avcodec_send_packet(p_codec_ctx, p_packet);
            if (ret != 0)
            {
                printf("avcodec_send_packet() failed %d\n", ret);
                av_packet_unref(p_packet);
                res = -1;
                goto exit;
            }
        }
    }

exit:
    av_frame_unref(p_frame);
    return res;
}

// ��Ƶ����ص������������л�ȡ��Ƶ�������룬����
// �˺�����SDL������ã��˺��������û����߳��У����������Ҫ����
// \param[in]  userdata�û���ע��ص�����ʱָ���Ĳ���
// \param[out] stream ��Ƶ���ݻ�������ַ������������Ƶ��������˻�����
// \param[out] len    ��Ƶ���ݻ�������С����λ�ֽ�
// �ص��������غ�streamָ�����Ƶ����������Ϊ��Ч
// ˫�����������˳��ΪLRLRLR
void sdl_audio_callback(void* userdata, uint8_t* stream, int len)
{
    AVCodecContext* p_codec_ctx = (AVCodecContext*)userdata;
    int copy_len;           // 
    int get_size;           // ��ȡ����������Ƶ���ݴ�С

    static uint8_t s_audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2]; // 1.5������֡�Ĵ�С
    static uint32_t s_audio_len = 0;    // ��ȡ�õ���Ƶ���ݴ�С
    static uint32_t s_tx_idx = 0;       // �ѷ��͸��豸��������

    AVPacket* p_packet;

    int frm_size = 0;
    int ret_size = 0;
    int ret;

    while (len > 0)         // ȷ��stream������������������˺�������
    {
        if (s_decode_finished)
        {
            return;
        }

        if (s_tx_idx >= s_audio_len)
        {   // audio_buf��������������ȫ��ȡ������Ӷ����л�ȡ��������
            p_packet = (AVPacket*)av_malloc(sizeof(AVPacket));

            // 1. �Ӷ����ж���һ����Ƶ����
            if (packet_queue_pop(&s_audio_pkt_queue, p_packet, 1) <= 0)
            {
                if (s_input_finished)
                {
                    av_packet_unref(p_packet);
                    p_packet = NULL;    // flush decoder
                    printf("Flushing decoder...\n");
                }
                else
                {
                    av_packet_unref(p_packet);
                    return;
                }
            }

            // 2. ������Ƶ��
            get_size = audio_decode_frame(p_codec_ctx, p_packet, s_audio_buf, sizeof(s_audio_buf));
            if (get_size < 0)
            {
                // �������һ�ξ���
                s_audio_len = 1024; // arbitrary?
                memset(s_audio_buf, 0, s_audio_len);
                av_packet_unref(p_packet);
            }
            else if (get_size == 0) // ���뻺��������ϴ����������������
            {
                s_decode_finished = true;
            }
            else
            {
                s_audio_len = get_size;
                av_packet_unref(p_packet);
            }
            s_tx_idx = 0;

            if (p_packet->data != NULL)
            {
                //av_packet_unref(p_packet);
            }
        }

        copy_len = s_audio_len - s_tx_idx;
        if (copy_len > len)
        {
            copy_len = len;
        }

        // ����������Ƶ֡(s_audio_buf+)д����Ƶ�豸������(stream)������
        memcpy(stream, (uint8_t*)s_audio_buf + s_tx_idx, copy_len);
        len -= copy_len;
        stream += copy_len;
        s_tx_idx += copy_len;
    }
}

int raw(int argc, char* argv[])
{
    // Initalizing these to NULL prevents segfaults!
    AVFormatContext* p_fmt_ctx = NULL;
    AVCodecContext* p_codec_ctx = NULL;
    AVCodecParameters* p_codec_par = NULL;
    AVCodec* p_codec = NULL;
    AVPacket* p_packet = NULL;

    SDL_AudioSpec       wanted_spec;
    SDL_AudioSpec       actual_spec;

    int                 i = 0;
    int                 a_idx = -1;
    int                 ret = 0;
    int                 res = 0;

    //if (argc < 2)
    //{
    //    printf("Please provide a movie file\n");
    //    return -1;
    //}

    // ��ʼ��libavformat(���и�ʽ)��ע�����и�����/�⸴����
    // av_register_all();   // �ѱ�����Ϊ��ʱ�ģ�ֱ�Ӳ���ʹ�ü���

    // A1. ����AVFormatContext
    // A1.1 ����Ƶ�ļ�����ȡ�ļ�ͷ�����ļ���ʽ��Ϣ�洢��"fmt context"��
    const char* path = "D:/IDM/testvideo.flv";
    ret = avformat_open_input(&p_fmt_ctx, path, NULL, NULL);
    if (ret != 0)
    {
        printf("avformat_open_input() failed %d\n", ret);
        res = -1;
        goto exit0;
    }

    // A1.2 ��������Ϣ����ȡһ����Ƶ�ļ����ݣ����Խ��룬��ȡ��������Ϣ����p_fmt_ctx->streams
    //      p_fmt_ctx->streams��һ��ָ�����飬�����С��pFormatCtx->nb_streams
    ret = avformat_find_stream_info(p_fmt_ctx, NULL);
    if (ret < 0)
    {
        printf("avformat_find_stream_info() failed %d\n", ret);
        res = -1;
        goto exit1;
    }

    // ���ļ������Ϣ��ӡ�ڱ�׼�����豸��
    av_dump_format(p_fmt_ctx, 0, argv[1], 0);

    // A2. ���ҵ�һ����Ƶ��
    a_idx = -1;
    for (i = 0; i < p_fmt_ctx->nb_streams; i++)
    {
        if (p_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            a_idx = i;
            printf("Find a audio stream, index %d\n", a_idx);
            break;
        }
    }
    if (a_idx == -1)
    {
        printf("Cann't find audio stream\n");
        res = -1;
        goto exit1;
    }

    // A3. Ϊ��Ƶ������������AVCodecContext

    // A3.1 ��ȡ����������AVCodecParameters
    p_codec_par = p_fmt_ctx->streams[a_idx]->codecpar;

    // A3.2 ��ȡ������
    p_codec = avcodec_find_decoder(p_codec_par->codec_id);
    if (p_codec == NULL)
    {
        printf("Cann't find codec!\n");
        res = -1;
        goto exit1;
    }

    // A3.3 ����������AVCodecContext
    // A3.3.1 p_codec_ctx��ʼ��������ṹ�壬ʹ��p_codec��ʼ����Ӧ��ԱΪĬ��ֵ
    p_codec_ctx = avcodec_alloc_context3(p_codec);
    if (p_codec_ctx == NULL)
    {
        printf("avcodec_alloc_context3() failed %d\n", ret);
        res = -1;
        goto exit1;
    }
    // A3.3.2 p_codec_ctx��ʼ����p_codec_par ==> p_codec_ctx����ʼ����Ӧ��Ա
    ret = avcodec_parameters_to_context(p_codec_ctx, p_codec_par);
    if (ret < 0)
    {
        printf("avcodec_parameters_to_context() failed %d\n", ret);
        res = -1;
        goto exit2;
    }
    // A3.3.3 p_codec_ctx��ʼ����ʹ��p_codec��ʼ��p_codec_ctx����ʼ�����
    ret = avcodec_open2(p_codec_ctx, p_codec, NULL);
    if (ret < 0)
    {
        printf("avcodec_open2() failed %d\n", ret);
        res = -1;
        goto exit2;
    }

    p_packet = (AVPacket*)av_malloc(sizeof(AVPacket));
    if (p_packet == NULL)
    {
        printf("av_malloc() failed\n");
        res = -1;
        goto exit2;
    }

    // B1. ��ʼ��SDL��ϵͳ��ȱʡ(�¼������ļ�IO���߳�)����Ƶ����Ƶ����ʱ��
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER))
    {
        printf("SDL_Init() failed: %s\n", SDL_GetError());
        res = -1;
        goto exit3;
    }

    packet_queue_init(&s_audio_pkt_queue);

    // B2. ����Ƶ�豸��������Ƶ�����߳�
    // B2.1 ����Ƶ�豸����ȡSDL�豸֧�ֵ���Ƶ����actual_spec(�����Ĳ�����wanted_spec��ʵ�ʵõ�actual_spec)
    // 1) SDL�ṩ����ʹ��Ƶ�豸ȡ����Ƶ���ݷ�����
    //    a. push��SDL���ض���Ƶ�ʵ��ûص��������ڻص�������ȡ����Ƶ����
    //    b. pull���û��������ض���Ƶ�ʵ���SDL_QueueAudio()������Ƶ�豸�ṩ���ݡ��������wanted_spec.callback=NULL
    // 2) ��Ƶ�豸�򿪺󲥷ž������������ص�������SDL_PauseAudio(0)�������ص�����ʼ����������Ƶ
    wanted_spec.freq = p_codec_ctx->sample_rate;    // ������
    wanted_spec.format = AUDIO_S16SYS;              // S������ţ�16�ǲ�����ȣ�SYS�����ϵͳ�ֽ���
    wanted_spec.channels = p_codec_ctx->channels;   // ������
    wanted_spec.silence = 0;                        // ����ֵ
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;    // SDL�����������ߴ磬��λ�ǵ�����������ߴ�xͨ����
    wanted_spec.callback = sdl_audio_callback;      // �ص���������ΪNULL����Ӧʹ��SDL_QueueAudio()����
    wanted_spec.userdata = p_codec_ctx;             // �ṩ���ص������Ĳ���
    if (SDL_OpenAudio(&wanted_spec, &actual_spec) < 0)
    {
        printf("SDL_OpenAudio() failed: %s\n", SDL_GetError());
        goto exit4;
    }

    // B2.2 ����SDL��Ƶ����������Ƶ�ز�������
    // wanted_spec�������Ĳ�����actual_spec��ʵ�ʵĲ�����wanted_spec��auctual_spec����SDL�еĲ�����
    // �˴�audio_param��FFmpeg�еĲ������˲���Ӧ��֤��SDL����֧�ֵĲ����������ز���Ҫ�õ��˲���
    // ��Ƶ֡�����õ���frame�е���Ƶ��ʽδ�ر�SDL֧�֣�����frame������planar��ʽ����SDL2.0����֧��planar��ʽ��
    // ����������frameֱ������SDL��Ƶ���������������޷��������š�������Ҫ�Ƚ�frame�ز���(ת����ʽ)ΪSDL֧�ֵ�ģʽ��
    // Ȼ������д��SDL��Ƶ������
    s_audio_param_tgt.fmt = AV_SAMPLE_FMT_S16;
    s_audio_param_tgt.freq = actual_spec.freq;
    s_audio_param_tgt.channel_layout = av_get_default_channel_layout(actual_spec.channels);;
    s_audio_param_tgt.channels = actual_spec.channels;
    s_audio_param_tgt.frame_size = av_samples_get_buffer_size(NULL, actual_spec.channels, 1, s_audio_param_tgt.fmt, 1);
    s_audio_param_tgt.bytes_per_sec = av_samples_get_buffer_size(NULL, actual_spec.channels, actual_spec.freq, s_audio_param_tgt.fmt, 1);
    if (s_audio_param_tgt.bytes_per_sec <= 0 || s_audio_param_tgt.frame_size <= 0)
    {
        printf("av_samples_get_buffer_size failed\n");
        goto exit4;
    }
    s_audio_param_src = s_audio_param_tgt;

    // B3. ��ͣ/������Ƶ�ص���������1����ͣ��0�������
    //     ����Ƶ�豸��Ĭ��δ�����ص�����ͨ������SDL_PauseAudio(0)�������ص�����
    //     �����Ϳ����ڴ���Ƶ�豸����Ϊ�ص�������ȫ��ʼ�����ݣ�һ�о�������������Ƶ�ص���
    //     ����ͣ�ڼ䣬�Ὣ����ֵ����Ƶ�豸д��
    SDL_PauseAudio(0);

    // A4. ����Ƶ�ļ��ж�ȡһ��packet���˴���������Ƶpacket
    //     ������Ƶ��˵������֡���̶��ĸ�ʽ��һ��packet�ɰ���������frame��
    //                   ����֡���ɱ�ĸ�ʽ��һ��packetֻ����һ��frame
    while (av_read_frame(p_fmt_ctx, p_packet) == 0)
    {
        if (p_packet->stream_index == a_idx)
        {
            packet_queue_push(&s_audio_pkt_queue, p_packet);
        }
        else
        {
            av_packet_unref(p_packet);
        }
    }
    SDL_Delay(40);
    s_input_finished = true;

    // A5. �ȴ��������
    while (!s_decode_finished)
    {
        SDL_Delay(1000);
    }
    SDL_Delay(1000);

exit4:
    SDL_Quit();
exit3:
    av_packet_unref(p_packet);
exit2:
    avcodec_free_context(&p_codec_ctx);
exit1:
    avformat_close_input(&p_fmt_ctx);
exit0:
    if (s_resample_buf != NULL)
    {
        av_free(s_resample_buf);
    }
    return res;
}

