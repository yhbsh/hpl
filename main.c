#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>

#define DEFAULT_W 1280
#define DEFAULT_H 720
#define DEFAULT_FPS 180

typedef struct {
    AVFormatContext *fmt_ctx;
    AVPacket *pkt;
    int video_stream_idx;
    int audio_stream_idx;
} Demuxer;

typedef struct {
    const AVCodec *codec;
    AVCodecContext *codec_ctx;
    AVFrame *frame;
    AVFrame *rgba_frame;
    SwsContext *sws_ctx;
} VideoDecoder;

typedef struct {
    const AVCodec *codec;
    AVCodecContext *codec_ctx;
    AVFrame *frame;
    SwrContext *swr_ctx;
    uint8_t *buf;
    int buf_size;
    SDL_AudioDeviceID dev;
} AudioDecoder;

static int demuxer_init(Demuxer *dmx, const char *url) {
    int ret;

    dmx->fmt_ctx = NULL;
    dmx->pkt = av_packet_alloc();
    if (!dmx->pkt) return AVERROR(ENOMEM);

    if ((ret = avformat_open_input(&dmx->fmt_ctx, url, NULL, NULL)) < 0) return ret;
    if ((ret = avformat_find_stream_info(dmx->fmt_ctx, NULL)) < 0) return ret;

    dmx->video_stream_idx = av_find_best_stream(dmx->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    dmx->audio_stream_idx = av_find_best_stream(dmx->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    return 0;
}

static int demuxer_read(Demuxer *dmx) {
    return av_read_frame(dmx->fmt_ctx, dmx->pkt);
}

static void demuxer_deinit(Demuxer *dmx) {
    if (dmx->pkt) av_packet_free(&dmx->pkt);
    if (dmx->fmt_ctx) avformat_close_input(&dmx->fmt_ctx);
}

static int video_decoder_init(VideoDecoder *vdec, Demuxer *dmx, int out_w, int out_h) {
    int ret;

    if (dmx->video_stream_idx < 0) return AVERROR_STREAM_NOT_FOUND;

    AVStream *stream = dmx->fmt_ctx->streams[dmx->video_stream_idx];

    vdec->codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!vdec->codec) return AVERROR_DECODER_NOT_FOUND;

    vdec->codec_ctx = avcodec_alloc_context3(vdec->codec);
    if (!vdec->codec_ctx) return AVERROR(ENOMEM);

    if ((ret = avcodec_parameters_to_context(vdec->codec_ctx, stream->codecpar)) < 0) return ret;
    if ((ret = avcodec_open2(vdec->codec_ctx, vdec->codec, NULL)) < 0) return ret;

    vdec->frame = av_frame_alloc();
    vdec->rgba_frame = av_frame_alloc();
    if (!vdec->frame || !vdec->rgba_frame) return AVERROR(ENOMEM);

    vdec->sws_ctx = sws_getContext(
        vdec->codec_ctx->width, vdec->codec_ctx->height, vdec->codec_ctx->pix_fmt,
        out_w, out_h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL
    );
    if (!vdec->sws_ctx) return AVERROR(EINVAL);

    vdec->rgba_frame->format = AV_PIX_FMT_RGBA;
    vdec->rgba_frame->width = out_w;
    vdec->rgba_frame->height = out_h;

    return 0;
}

static int video_decoder_send(VideoDecoder *vdec, AVPacket *pkt) {
    return avcodec_send_packet(vdec->codec_ctx, pkt);
}

static int video_decoder_receive(VideoDecoder *vdec, uint8_t **out_rgba) {
    int ret = avcodec_receive_frame(vdec->codec_ctx, vdec->frame);
    if (ret < 0) return ret;

    sws_scale_frame(vdec->sws_ctx, vdec->rgba_frame, vdec->frame);
    *out_rgba = vdec->rgba_frame->data[0];
    return 0;
}

static void video_decoder_deinit(VideoDecoder *vdec) {
    if (vdec->sws_ctx) sws_freeContext(vdec->sws_ctx);
    if (vdec->rgba_frame) av_frame_free(&vdec->rgba_frame);
    if (vdec->frame) av_frame_free(&vdec->frame);
    if (vdec->codec_ctx) avcodec_free_context(&vdec->codec_ctx);
}

static int audio_decoder_init(AudioDecoder *adec, Demuxer *dmx) {
    int ret;

    adec->swr_ctx = NULL;
    adec->buf = NULL;
    adec->buf_size = 0;
    adec->dev = 0;

    if (dmx->audio_stream_idx < 0) return AVERROR_STREAM_NOT_FOUND;

    AVStream *stream = dmx->fmt_ctx->streams[dmx->audio_stream_idx];

    adec->codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!adec->codec) return AVERROR_DECODER_NOT_FOUND;

    adec->codec_ctx = avcodec_alloc_context3(adec->codec);
    if (!adec->codec_ctx) return AVERROR(ENOMEM);

    if ((ret = avcodec_parameters_to_context(adec->codec_ctx, stream->codecpar)) < 0) return ret;
    if ((ret = avcodec_open2(adec->codec_ctx, adec->codec, NULL)) < 0) return ret;

    adec->frame = av_frame_alloc();
    if (!adec->frame) return AVERROR(ENOMEM);

    int out_sample_rate = adec->codec_ctx->sample_rate;
    int out_channels = 2;

    adec->swr_ctx = swr_alloc();
    if (!adec->swr_ctx) return AVERROR(ENOMEM);

    AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    av_opt_set_chlayout(adec->swr_ctx, "in_chlayout",  &adec->codec_ctx->ch_layout, 0);
    av_opt_set_chlayout(adec->swr_ctx, "out_chlayout", &out_ch_layout, 0);
    av_opt_set_int(adec->swr_ctx, "in_sample_rate",  adec->codec_ctx->sample_rate, 0);
    av_opt_set_int(adec->swr_ctx, "out_sample_rate", out_sample_rate, 0);
    av_opt_set_sample_fmt(adec->swr_ctx, "in_sample_fmt",  adec->codec_ctx->sample_fmt, 0);
    av_opt_set_sample_fmt(adec->swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    if ((ret = swr_init(adec->swr_ctx)) < 0) return ret;

    SDL_AudioSpec want = {0};
    want.freq = out_sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = out_channels;
    want.samples = 1024;

    adec->dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (adec->dev == 0) return -1;

    SDL_PauseAudioDevice(adec->dev, 0);

    return 0;
}

static int audio_decoder_send(AudioDecoder *adec, AVPacket *pkt) {
    return avcodec_send_packet(adec->codec_ctx, pkt);
}

static int audio_decoder_receive(AudioDecoder *adec) {
    int ret;
    while ((ret = avcodec_receive_frame(adec->codec_ctx, adec->frame)) >= 0) {
        int out_samples = swr_get_out_samples(adec->swr_ctx, adec->frame->nb_samples);
        int needed = out_samples * 2 * sizeof(int16_t); // stereo S16
        if (needed > adec->buf_size) {
            adec->buf = realloc(adec->buf, needed);
            adec->buf_size = needed;
        }

        uint8_t *out_buf = adec->buf;
        int converted = swr_convert(adec->swr_ctx, &out_buf, out_samples,
                                    (const uint8_t **)adec->frame->extended_data,
                                    adec->frame->nb_samples);
        if (converted > 0) {
            SDL_QueueAudio(adec->dev, adec->buf, converted * 2 * sizeof(int16_t));
        }
    }
    return (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ? 0 : ret;
}

static void audio_decoder_deinit(AudioDecoder *adec) {
    if (adec->dev) SDL_CloseAudioDevice(adec->dev);
    if (adec->swr_ctx) swr_free(&adec->swr_ctx);
    if (adec->frame) av_frame_free(&adec->frame);
    if (adec->codec_ctx) avcodec_free_context(&adec->codec_ctx);
    free(adec->buf);
}

int main(int argc, char **argv) {
    int width = DEFAULT_W, height = DEFAULT_H, fps = DEFAULT_FPS;
    int log_level = AV_LOG_INFO;
    int opt;

    while ((opt = getopt(argc, argv, "s:f:l:h")) != -1) {
        switch (opt) {
            case 's': {
                if (sscanf(optarg, "%dx%d", &width, &height) != 2) {
                    fprintf(stderr, "error: invalid size format. use WIDTHxHEIGHT\n");
                    return 1;
                }
                break;
            }
            case 'f': {
                fps = atoi(optarg);
                if (fps <= 0) {
                    fprintf(stderr, "error: fps must be positive\n");
                    return 1;
                }
                break;
            }
            case 'l': {
                if (strcmp(optarg, "QUIET") == 0) log_level = AV_LOG_QUIET;
                else if (strcmp(optarg, "ERROR") == 0) log_level = AV_LOG_ERROR;
                else if (strcmp(optarg, "WARNING") == 0) log_level = AV_LOG_WARNING;
                else if (strcmp(optarg, "INFO") == 0) log_level = AV_LOG_INFO;
                else if (strcmp(optarg, "DEBUG") == 0) log_level = AV_LOG_DEBUG;
                else if (strcmp(optarg, "TRACE") == 0) log_level = AV_LOG_TRACE;
                else {
                    fprintf(stderr, "error: unknown log level '%s'\n", optarg);
                    return 1;
                }
                break;
            }
            case 'h':
            default:
                fprintf(stderr, "usage: %s [options] <url>\n", argv[0]);
                fprintf(stderr, "  -s WIDTHxHEIGHT  output size (default %dx%d)\n", DEFAULT_W, DEFAULT_H);
                fprintf(stderr, "  -f FPS           target fps (default %d)\n", DEFAULT_FPS);
                fprintf(stderr, "  -l LEVEL         log level: QUIET, ERROR, WARNING, INFO, DEBUG, TRACE (default INFO)\n");
                fprintf(stderr, "  -h               show this help\n");
                return opt == 'h' ? 0 : 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "error: missing url argument\n");
        return 1;
    }

    const char *url = argv[optind];
    av_log_set_level(log_level);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "error: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    fprintf(stdout, "loading: %s (output: %dx%d @ %d fps)\n", url, width, height, fps);

    Demuxer dmx;
    if (demuxer_init(&dmx, url) != 0) {
        fprintf(stderr, "error: failed to open input\n");
        SDL_Quit();
        return 1;
    }

    if (dmx.video_stream_idx < 0 && dmx.audio_stream_idx < 0) {
        fprintf(stderr, "error: input has neither video nor audio stream\n");
        demuxer_deinit(&dmx);
        SDL_Quit();
        return 1;
    }

    int has_video = 0;
    VideoDecoder vdec = {0};
    if (dmx.video_stream_idx >= 0) {
        if (video_decoder_init(&vdec, &dmx, width, height) == 0) {
            has_video = 1;
            fprintf(stdout, "video: %s (%dx%d)\n", vdec.codec->name, vdec.codec_ctx->width, vdec.codec_ctx->height);
        } else {
            fprintf(stderr, "warning: video stream found but decoder init failed\n");
        }
    }

    int has_audio = 0;
    AudioDecoder adec = {0};
    if (dmx.audio_stream_idx >= 0) {
        if (audio_decoder_init(&adec, &dmx) == 0) {
            has_audio = 1;
            fprintf(stdout, "audio: %s (%d Hz, %d ch)\n",
                    adec.codec->name, adec.codec_ctx->sample_rate,
                    adec.codec_ctx->ch_layout.nb_channels);
        } else {
            fprintf(stderr, "warning: audio stream found but decoder init failed\n");
        }
    }

    if (!has_video && !has_audio) {
        fprintf(stderr, "error: no usable streams\n");
        demuxer_deinit(&dmx);
        SDL_Quit();
        return 1;
    }

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    SDL_Rect viewport = {0};

    if (has_video) {
        window = SDL_CreateWindow("hpl", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, 0);
        if (!window) {
            fprintf(stderr, "error: SDL_CreateWindow: %s\n", SDL_GetError());
            goto cleanup;
        }

        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            fprintf(stderr, "error: SDL_CreateRenderer: %s\n", SDL_GetError());
            goto cleanup;
        }

        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!texture) {
            fprintf(stderr, "error: SDL_CreateTexture: %s\n", SDL_GetError());
            goto cleanup;
        }

        float src_aspect = (float)vdec.codec_ctx->width / vdec.codec_ctx->height;
        float dst_aspect = (float)width / height;
        if (src_aspect > dst_aspect) {
            viewport.w = width;
            viewport.h = (int)(width / src_aspect);
        } else {
            viewport.h = height;
            viewport.w = (int)(height * src_aspect);
        }
        viewport.x = (width - viewport.w) / 2;
        viewport.y = (height - viewport.h) / 2;
    }

    uint8_t *data = NULL;
    int running = 1;
    int eof = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_q)) {
                running = 0;
            }
        }

        int got_video_frame = 0;
        while (!eof && (has_video ? !got_video_frame : 1)) {
            int ret = demuxer_read(&dmx);
            if (ret < 0) {
                eof = 1;
                break;
            }

            int audio_pkt = 0;
            if (has_video && dmx.pkt->stream_index == dmx.video_stream_idx) {
                video_decoder_send(&vdec, dmx.pkt);
                if (video_decoder_receive(&vdec, &data) == 0 && data) {
                    SDL_UpdateTexture(texture, NULL, data, width * 4);
                    got_video_frame = 1;
                }
            } else if (has_audio && dmx.pkt->stream_index == dmx.audio_stream_idx) {
                audio_decoder_send(&adec, dmx.pkt);
                audio_decoder_receive(&adec);
                audio_pkt = 1;
            }
            av_packet_unref(dmx.pkt);
            if (!has_video && audio_pkt) break;
        }

        if (has_video) {
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, NULL, &viewport);
            SDL_RenderPresent(renderer);
        } else {
            if (eof && SDL_GetQueuedAudioSize(adec.dev) == 0) running = 0;
            SDL_Delay(10);
        }
    }

cleanup:
    if (has_video) video_decoder_deinit(&vdec);
    if (has_audio) audio_decoder_deinit(&adec);
    demuxer_deinit(&dmx);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
