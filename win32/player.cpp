/************************************************************************
 *ffplayer.c
 *detail:
 *  A simple ffmpeg player.
 *
 ************************************************************************/
#include "player.h"
extern "C"
{
	#include <libavformat/avformat.h>
	#include <libavcodec/avcodec.h>
	#include <libavutil/frame.h>
	#include <libavutil/imgutils.h>
	#include <libavutil/mem.h>
	#include <libswscale/swscale.h>
	#include <SDL.h>

}

int play(int argc, char* argv[])
{

	// initialize
	AVFormatContext* p_avfmt_ctx = NULL;
	AVCodecContext* p_avcodec_ctx = NULL;
	AVCodecParameters* p_avcodec_para = NULL;
	AVCodec* p_avcodec = NULL;
	AVFrame* p_avframe_raw = NULL;
	AVFrame* p_avframe_yuv = NULL;
	AVPacket* p_avpacket = NULL;

	struct SwsContext* p_sws_ctx;
	SDL_Window* win;
	SDL_Renderer* renderer;
	SDL_Texture* texture;
	SDL_Rect rect;

	int buf_size;
	int ret = 0;
	uint8_t* buffer = NULL;

	//if (argc < 2)
	//{
	//	printf("Need more paras.\n");
	//	exit(EXIT_FAILURE);
	//}
	//char* path = argv[1];
	const char* path = "testvideo.flv";
	//open file
	ret = avformat_open_input(&p_avfmt_ctx, path, NULL, NULL);
	if (ret)
	{
		printf("Failed to open file.\n");
		exit(EXIT_FAILURE);
	}
	ret = avformat_find_stream_info(p_avfmt_ctx, NULL);
	if (ret < 0)
	{
		printf("Failed to find stream info.\n");
		exit(EXIT_FAILURE);
	}

	// print file info to stadard error stream
	av_dump_format(p_avfmt_ctx, 0, path, 0);

	// find video stream
	int v_idx = -1;
	for (uint32_t i = 0; i < p_avfmt_ctx->nb_streams; i++)
	{
		if (p_avfmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			v_idx = i;
			break;
		}
	}
	if (v_idx == -1)
	{
		printf("No video stream.\n");
		exit(EXIT_FAILURE);
	}

	// get a codec
	p_avcodec_para = p_avfmt_ctx->streams[v_idx]->codecpar;
	p_avcodec = avcodec_find_decoder(p_avcodec_para->codec_id);
	if (!p_avcodec)
	{
		printf("Failed to get a codec.\n");
		exit(EXIT_FAILURE);
	}
	p_avcodec_ctx = avcodec_alloc_context3(p_avcodec);
	ret = avcodec_parameters_to_context(p_avcodec_ctx, p_avcodec_para);
	if (ret < 0)
	{
		printf("Falied to fill codec context.\n");
		exit(EXIT_FAILURE);
	}

	ret = avcodec_open2(p_avcodec_ctx, p_avcodec, NULL);
	if (ret < 0)
	{
		printf("Failed to initialize codec context.\n");
		exit(EXIT_FAILURE);
	}

	//allocate an avframe
	p_avframe_raw = av_frame_alloc();
	p_avframe_yuv = av_frame_alloc();
	if (!p_avframe_raw || !p_avframe_yuv)
	{
		printf("Failed to allocate an avframe.\n");
		exit(EXIT_FAILURE);
	}
	buf_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P,
		p_avcodec_ctx->width,
		p_avcodec_ctx->height,
		1
	);//last para means align,1 means no align ,4 means 4 bytes align etc.
	// printf("buf_size: %d\n", buf_size);
	buffer = (uint8_t*)av_malloc(buf_size);//buffer is a pointer, buffer++ means moving to next bytes.

	av_image_fill_arrays(p_avframe_yuv->data,
		p_avframe_yuv->linesize,
		buffer,
		AV_PIX_FMT_YUV420P,
		p_avcodec_ctx->width,
		p_avcodec_ctx->height,
		1
	);

	//initilize sws context
	//trans AV_PIX_FMT_YUV420P to SDL_PIXELFORMAT_IYUV 
	p_sws_ctx = sws_getContext(p_avcodec_ctx->width,
		p_avcodec_ctx->height,
		p_avcodec_ctx->pix_fmt,//src fmt
		p_avcodec_ctx->width,
		p_avcodec_ctx->height,
		AV_PIX_FMT_YUV420P,
		SWS_BICUBIC,
		NULL,
		NULL,
		NULL
	);
	if (!p_sws_ctx)
	{
		printf("Falied to initilize sws context.\n");
		exit(EXIT_FAILURE);
	}

	//initilize SDL
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		printf("Failed to initilize SDL.\n");
		exit(EXIT_FAILURE);
	}

	win = SDL_CreateWindow(
		"FFmpeg player demo",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		p_avcodec_ctx->width,
		p_avcodec_ctx->height,
		SDL_WINDOW_OPENGL
	);
	if (!win)
	{
		printf("Failed to create a window.\n");
		exit(EXIT_FAILURE);
	}
	renderer = SDL_CreateRenderer(win, -1, 0);
	if (!renderer)
	{
		printf("Failed to create a renderer.\n");
		exit(EXIT_FAILURE);
	}
	texture = SDL_CreateTexture(
		renderer,
		SDL_PIXELFORMAT_IYUV,
		SDL_TEXTUREACCESS_STREAMING,
		p_avcodec_ctx->width,
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
	p_avpacket = (AVPacket*)av_malloc(sizeof(AVPacket));

	// read packet from stream
	// one packet contains one video frame or audio frame
	while (av_read_frame(p_avfmt_ctx, p_avpacket) == 0)
	{
		//if is video frame
		if (p_avpacket->stream_index == v_idx)
		{
			ret = avcodec_send_packet(p_avcodec_ctx, p_avpacket);
			if (ret)
			{
				printf("Failed to send packet to codec.\n");
				exit(EXIT_FAILURE);
			}
			ret = avcodec_receive_frame(p_avcodec_ctx, p_avframe_raw);
			if (ret)
			{
				printf("Failed to decode packet.\n");
				exit(EXIT_FAILURE);
			}
			
			//trans raw data to yuv data
			sws_scale(p_sws_ctx,
				(const uint8_t* const*)p_avframe_raw->data,
				p_avframe_raw->linesize,
				0,
				p_avcodec_ctx->height,
				p_avframe_yuv->data,
				p_avframe_yuv->linesize
			);
			SDL_UpdateYUVTexture(texture,
				&rect,
				p_avframe_yuv->data[0],
				p_avframe_yuv->linesize[0],
				p_avframe_yuv->data[1],
				p_avframe_yuv->linesize[1],
				p_avframe_yuv->data[2],
				p_avframe_yuv->linesize[2]
			);
			SDL_RenderClear(renderer);
			SDL_RenderCopy(renderer,
				texture,
				NULL,
				&rect
			);
			SDL_RenderPresent(renderer);
			SDL_Delay(1);
			av_packet_unref(p_avpacket);

		}

	}

	// close file
	SDL_Quit();
	sws_freeContext(p_sws_ctx);
	av_free(buffer);
	av_frame_free(&p_avframe_yuv);
	av_frame_free(&p_avframe_raw);

	avcodec_free_context(&p_avcodec_ctx);
	avformat_close_input(&p_avfmt_ctx);
	return 0;
}
