/************************************************************************
 *ffplayer.c
 *detail:
 *  A simple ffmpeg player.
 *version: 0.0.2.5
 *  Play audio only
 *
 ************************************************************************/
#include "pixelflix-v3.h"
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

#define Packet_QUEUE_SIZE 996
#define AUDIO_BUFFER_SIZE 1024  // means hao many samples the audio buffer has, 
//the para is set by user usually between handruads and thousands samples
// it must be power of 2
#define MAX_AUDIO_FRAME_SIZE 192000

// exit
int exitCase(const char* c)
{
	printf("%s\n", c);
	exit(EXIT_FAILURE);
}
enum Err
{
	FAILURE = 0,
	NULL_POINTER_ERROR = -1,
};
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
	int (*destory)(struct PacketQueue* q);
	int (*isEmpty)(struct PacketQueue* q);
	int (*isFull)(struct PacketQueue* q);
	int (*enqueue)(struct PacketQueue* q, AVPacket* p);
	int (*dequeue)(struct PacketQueue* q, AVPacket* p, int block);
	SDL_mutex* mutex;
	SDL_cond* cond;
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
static FFAudioParas srcParas;
static FFAudioParas tgtParas;
static bool isStreamFinished = false;
static bool isDecodeFinished = false;
static struct SwrContext* swrCtx;
static uint8_t* s_resample_buf = NULL;  // 重采样输出缓冲区
static unsigned int s_resample_buf_len = 0;      // 重采样输出缓冲区长度

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
int enqueue(PacketQueue* q, AVPacket* p)
{
	if (av_packet_make_refcounted(p))
	{
		fprintf(stderr,"Failed to set packet reference counted.");
		return FAILURE;
	}
	if (isFull(q)) return FAILURE;

	Node* pnode = (Node*)av_malloc(sizeof(Node));
	if (!pnode)
	{
		fprintf(stderr,"Failed to malloc Node.\n");
		return FAILURE;
	}
	SDL_LockMutex(q->mutex);

	pnode->packet = *p;
	pnode->next = NULL;
	q->rear->next = pnode;
	q->rear = q->rear->next;
	q->n++;
	q->bytes += sizeof(p);

	SDL_CondSignal(q->cond);
	SDL_UnlockMutex(q->mutex);
	printf("en:q->n:%d\n", q->n);
	return 1;
}

int dequeue(PacketQueue* q, AVPacket* p, int block)
{
	int ret=0;
	Node* temp;
	SDL_LockMutex(q->mutex);
	while (1)
	{
		temp = q->head->next;
		if (temp!=NULL)
		{
			*p = temp->packet;
			q->bytes -= sizeof(*temp);
			q->head->next = q->head->next->next;
			if (temp->next== NULL) q->rear = q->head;
			q->n--;
			av_free(temp);
			ret = 1;
			printf("de:q->n:%d\n", q->n);
			break;
		}
		else if (isStreamFinished)
		{
			ret = 0;
			break;
		}
		else if (!block)
		{
			ret = 0;
			break;
		}
		else
		{
			SDL_CondWait(q->cond, q->mutex);
		}
	}
	SDL_UnlockMutex(q->mutex);

	return ret;
}

int init(PacketQueue* q)
{
	if (!q->head)
	{
		Node* head = (Node*)malloc(sizeof(Node));
		if (!head) return 0;
		head->next = NULL;
		q->head = head;
	}
	q->rear = q->head;
	q->n = 0;
	q->max = Packet_QUEUE_SIZE;
	q->bytes = 0;
	q->destory = destroy;
	q->isEmpty = isEmpty;
	q->isFull = isFull;
	q->dequeue = dequeue;
	q->enqueue = enqueue;
	q->mutex= SDL_CreateMutex();
	q->cond= SDL_CreateCond();

	return 1;
}

//audio decode packet
int audioDecodePacket(AVCodecContext* ctx, AVPacket* pkt, uint8_t* audio_buf, int buf_size)
{
	AVFrame* pf = av_frame_alloc();
	if (!pf) exitCase("Failed to allocate AVFrame.");
	int res = 0;
	int frame_size = 0;
	int nb_samples;
	uint8_t* p_cp_buf = NULL;
	int cp_len;
	bool needNew = false;
	int frm_size = 0;

	//get a frame from decoder
	while (1)
	{
		res = avcodec_receive_frame(ctx, pf);
		printf("res:%d\n",res);
		if (res != 0)
		{
			if (res == AVERROR(EAGAIN)) needNew = true;
			else exitCase("Failed to get a frame.");
		}
		else
		{
			// s_audio_param_tgt是SDL可接受的音频帧数，是main()中取得的参数
			// 在main()函数中又有“s_audio_param_src = s_audio_param_tgt”
			// 此处表示：如果frame中的音频参数 == s_audio_param_src == s_audio_param_tgt，那音频重采样的过程就免了(因此时s_audio_swr_ctx是NULL)
			// 否则使用frame(源)和s_audio_param_src(目标)中的音频参数来设置s_audio_swr_ctx，并使用frame中的音频参数来赋值s_audio_param_src
			if (pf->format != srcParas.fmt ||
				pf->channel_layout != srcParas.channel_layout ||
				pf->sample_rate != srcParas.freq)
			{
				swr_free(&swrCtx);
				swrCtx = swr_alloc_set_opts(NULL,
					tgtParas.channel_layout,
					tgtParas.fmt,
					tgtParas.freq,
					pf->channel_layout,
					(enum AVSampleFormat)pf->format,
					pf->sample_rate,
					0,
					NULL);

				if (swrCtx == NULL || swr_init(swrCtx) < 0)
				{
					printf("Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
						pf->sample_rate, av_get_sample_fmt_name((enum AVSampleFormat)pf->format), pf->channels,
						tgtParas.freq, av_get_sample_fmt_name(tgtParas.fmt), tgtParas.channels);
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
				uint8_t** out = &s_resample_buf;
				// 重采样输出参数：输出音频样本数(多加了256个样本)
				int out_count = (int64_t)pf->nb_samples * tgtParas.freq / pf->sample_rate + 256;
				// 重采样输出参数：输出音频缓冲区尺寸(以字节为单位)
				int out_size = av_samples_get_buffer_size(NULL, tgtParas.channels, out_count, tgtParas.fmt, 0);
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
				// 音频重采样：返回值是重采样后得到的音频数据中单个声道的样本数
				nb_samples = swr_convert(swrCtx, out, out_count, in, pf->nb_samples);
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
				p_cp_buf = s_resample_buf;
				cp_len = nb_samples * tgtParas.channels * av_get_bytes_per_sample(tgtParas.fmt);
			}
			else    // 不重采样
			{
				// 根据相应音频参数，获得所需缓冲区大小
				frm_size = av_samples_get_buffer_size(
					NULL,
					ctx->channels,
					pf->nb_samples,
					ctx->sample_fmt,
					1);

				printf("frame size %d, buffer size %d\n", frm_size, buf_size);
				assert(frm_size <= buf_size);

				p_cp_buf = pf->data[0];
				cp_len = frm_size;
			}

			// 将音频帧拷贝到函数输出参数audio_buf
			memcpy(audio_buf, p_cp_buf, cp_len);
			res = cp_len;
			printf("cp_len:%d\n", cp_len);
			return res;
		}
		// 2 向解码器喂数据，每次喂一个packet
		if (needNew)
		{
			if(avcodec_send_packet(ctx, pkt)!=0)
			{
				printf("avcodec_send_packet() failed %d\n", res);
				av_packet_unref(pkt);
				res = -1;
				return res;
			}
		}
	}
}


// sdl audio callback
void audioCallback(void* userdata, uint8_t* stream, int len)
{
	AVCodecContext* codecCtx = (AVCodecContext*)userdata;
	if (!userdata) exitCase("Failed to get codec context.\n");

	int copyLen;
	int getSize;
	static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3 / 2)];//audio frame buffer:how many samples
	static uint32_t newReceive = 0;//have received from packet
	static uint32_t totalSend = 0;//have sent to device
	AVPacket* pkt = NULL;

	while (len > 0)
	{
		if (isDecodeFinished) return;

		if (newReceive<=totalSend)
		{
			pkt = (AVPacket*)av_malloc(sizeof(AVPacket));
			if (!pkt) exitCase("Failed to malloc packet.");

			//get a packet
			if (!q.dequeue(&q, pkt, 1))
			{
				//if stream has all been read, pkt=NULL
				if (isStreamFinished)
				{
					av_packet_unref(pkt);
					pkt = NULL;    // flush decoder
					printf("Flushing decoder.\n");
				}
				else
				{
					av_packet_unref(pkt);
				}
			}
			//decode packet
			getSize = audioDecodePacket(codecCtx, pkt, audio_buf, sizeof(audio_buf));
			if (getSize < 0)
			{
				memset(audio_buf, 0, 1024);
				av_packet_unref(pkt);
			}
			else if (getSize == 0)
			{
				isDecodeFinished = true;
			}
			else
			{
				newReceive = getSize;
				av_packet_unref(pkt);
			}
			totalSend = 0;
			if (pkt->data != NULL)
			{
				av_packet_unref(pkt);
			}

		}
		copyLen = newReceive - totalSend;
		if (copyLen > len)
		{
			copyLen = len;
		}

		// 将解码后的音频帧(s_audio_buf+)写入音频设备缓冲区(stream)，播放
		memcpy(stream, (uint8_t*)audio_buf + totalSend, copyLen);
		len -= copyLen;
		stream += copyLen;
		totalSend += copyLen;
	}

}


// playAudio
void* playAudio(void* arg)
{
	//const char* path = (const char*)arg;
	const char* path = "D:/IDM/testvideo.flv";
	if (path[0] == '\0') exitCase("Invalid file name.");
	printf("Audio name: %s\n", path);

	AVFormatContext* fmtCtx = NULL;
	AVCodec* codec = NULL;
	AVCodecContext* codecCtx = NULL;
	AVPacket* p_packet;
	SDL_AudioSpec desiredSpec;
	SDL_AudioSpec obtainedSpec;

	int a_idx = -1;

	if (avformat_open_input(&fmtCtx, path, NULL, NULL) != 0) exitCase("Failed to open file.");
	if (avformat_find_stream_info(fmtCtx, NULL) < 0) exitCase("Failed to find stream info.");
	// index：negative number means all info of input file
	av_dump_format(fmtCtx, -1, path, 0);

	for (uint8_t i = 0; i < fmtCtx->nb_streams; i++)
	{
		if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			a_idx = i;
			break;
		}
	}
	if (a_idx == -1) exitCase("Failed to find audio stream.");
	printf("index of audio stream is %d\n",a_idx);
	codec = avcodec_find_decoder(fmtCtx->streams[a_idx]->codecpar->codec_id);
	if (!codec) exitCase("Failed to find a codec.");
	codecCtx = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(codecCtx, fmtCtx->streams[a_idx]->codecpar);
	if (avcodec_open2(codecCtx, codec, NULL) != 0) exitCase("Failed to initialize codec context.");
	// Init SDL subsystem
	if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER)) exitCase("Failed to initilize SDL subsystem.");
	// Init packet queue
	if (!init(&q)) exitCase("Failed to initilize PacketQueue.");

	desiredSpec.freq = codecCtx->sample_rate;
	desiredSpec.format = AUDIO_S16SYS;//unsigned 16-bit samples in native byte order
	desiredSpec.channels = codecCtx->channels;
	desiredSpec.silence = 0;
	desiredSpec.samples = AUDIO_BUFFER_SIZE;//must be power of 2, means audio buffer in samples
	//`desiredSpec.size` means audio buffer in bytes
	//`desiredSpec.size` is autoly calculated and set: size=samples*channels*(bytes per sample)
	//usually obtainedSpec.size=desiredSpec.size=len
	//`len` is the para passed into callback function `audioCallback`
	//For example, AUDIO_BUFFFER_SIZE=1024, channels=2, format=AUDIO_S16SYS(means using 16 bits unsigned integer to desccribe a sample)
	//then len=1024*2*2=4096 bytes.(it's autoly calculated and you can test it.)
	desiredSpec.callback = audioCallback;
	desiredSpec.userdata = codecCtx;
	if (SDL_OpenAudio(&desiredSpec, &obtainedSpec)) exitCase("Failed to open audio device.\n");
	//Build audio resampling parameters based on SDL audio parameters.
	tgtParas.fmt = AV_SAMPLE_FMT_S16;
	tgtParas.freq = obtainedSpec.freq;
	tgtParas.channel_layout = av_get_default_channel_layout(obtainedSpec.channels);;
	tgtParas.channels = obtainedSpec.channels;
	tgtParas.frame_size = av_samples_get_buffer_size(NULL, obtainedSpec.channels, 1, tgtParas.fmt, 1);
	tgtParas.bytes_per_second = av_samples_get_buffer_size(NULL, obtainedSpec.channels, obtainedSpec.freq, tgtParas.fmt, 1);
	if (tgtParas.bytes_per_second <= 0 || tgtParas.frame_size <= 0) exitCase("Failed to get buffer size.\n");
	srcParas = tgtParas;

	SDL_PauseAudio(0);

	p_packet = (AVPacket*)av_malloc(sizeof(AVPacket));
	if (!p_packet) exitCase("Failed to allocte a packet.");
	//av_read_frame() will read next packet and set the pointer in para p_packet to make the pointer point to actual data,
	//it will not produce a copy.
	//it will reset the para p_packet's info every time when it is called.
	while (!av_read_frame(fmtCtx, p_packet))
	{
		if (p_packet->stream_index == a_idx) enqueue(&q, p_packet);
		else av_packet_unref(p_packet);//unref means clear all data
	}
	printf("All packet have been enqueued.\n");
	SDL_Delay(40);
	isStreamFinished = true;

	while (!isDecodeFinished)
	{
		SDL_Delay(1000);
	}


	//close device
	SDL_PauseAudio(1);
	SDL_CloseAudio();
	av_packet_free(&p_packet);
	p_packet = NULL;
	avcodec_free_context(&codecCtx);
	codecCtx = NULL;
	avformat_close_input(&fmtCtx);
	fmtCtx = NULL;
	return NULL;

}


int play3(int argc, char* argv[])
{
	//printf("Play audio:\n");
	//if (argc < 2) exitCase("Need more para.");

	playAudio(argv);

	return 0;
}
