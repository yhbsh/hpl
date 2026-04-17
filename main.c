#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>

typedef struct {
    SDL_atomic_t *quit;
    int64_t deadline_us;
} InterruptCtx;

static int interrupt_cb(void *opaque) {
    InterruptCtx *c = opaque;
    if (c->quit && SDL_AtomicGet(c->quit)) return 1;
    if (c->deadline_us && av_gettime_relative() > c->deadline_us) return 1;
    return 0;
}

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

static int demuxer_init(Demuxer *dmx, const char *url, InterruptCtx *ictx) {
    int ret;

    dmx->fmt_ctx = avformat_alloc_context();
    if (!dmx->fmt_ctx) return AVERROR(ENOMEM);
    if (ictx) {
        dmx->fmt_ctx->interrupt_callback.callback = interrupt_cb;
        dmx->fmt_ctx->interrupt_callback.opaque = ictx;
    }

    dmx->pkt = av_packet_alloc();
    if (!dmx->pkt) return AVERROR(ENOMEM);

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "timeout", "5000000", 0);
    av_dict_set(&opts, "rw_timeout", "5000000", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0);

    if ((ret = avformat_open_input(&dmx->fmt_ctx, url, NULL, &opts)) < 0) {
        av_dict_free(&opts);
        return ret;
    }
    av_dict_free(&opts);

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
        int needed = out_samples * 2 * sizeof(int16_t);
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

typedef struct {
    const char *url;

    Demuxer dmx;
    VideoDecoder vdec;
    AudioDecoder adec;
    int has_video;
    int has_audio;
    int width, height;

    InterruptCtx ictx;
    SDL_atomic_t quit;
    SDL_atomic_t init_done;
    SDL_atomic_t init_failed;
    SDL_atomic_t eof;

    SDL_mutex *frame_mu;
    uint8_t *frame_buf;
    int frame_buf_size;
    int frame_ready;
} Player;

static int io_thread(void *data) {
    Player *p = data;

    p->ictx.quit = &p->quit;
    p->ictx.deadline_us = av_gettime_relative() + 10 * 1000000;

    if (demuxer_init(&p->dmx, p->url, &p->ictx) != 0) {
        fprintf(stderr, "error: failed to open input\n");
        SDL_AtomicSet(&p->init_failed, 1);
        SDL_AtomicSet(&p->init_done, 1);
        return 1;
    }

    p->ictx.deadline_us = 0;

    if (p->dmx.video_stream_idx < 0 && p->dmx.audio_stream_idx < 0) {
        fprintf(stderr, "error: input has neither video nor audio stream\n");
        SDL_AtomicSet(&p->init_failed, 1);
        SDL_AtomicSet(&p->init_done, 1);
        return 1;
    }

    if (p->dmx.video_stream_idx >= 0) {
        AVCodecParameters *cp = p->dmx.fmt_ctx->streams[p->dmx.video_stream_idx]->codecpar;
        p->width = cp->width;
        p->height = cp->height;
        if (video_decoder_init(&p->vdec, &p->dmx, p->width, p->height) == 0) {
            p->has_video = 1;
            fprintf(stdout, "video: %s (%dx%d)\n", p->vdec.codec->name, p->width, p->height);
        } else {
            fprintf(stderr, "warning: video stream found but decoder init failed\n");
        }
    }

    if (p->dmx.audio_stream_idx >= 0) {
        if (audio_decoder_init(&p->adec, &p->dmx) == 0) {
            p->has_audio = 1;
            fprintf(stdout, "audio: %s (%d Hz, %d ch)\n",
                    p->adec.codec->name, p->adec.codec_ctx->sample_rate,
                    p->adec.codec_ctx->ch_layout.nb_channels);
        } else {
            fprintf(stderr, "warning: audio stream found but decoder init failed\n");
        }
    }

    if (!p->has_video && !p->has_audio) {
        fprintf(stderr, "error: no usable streams\n");
        SDL_AtomicSet(&p->init_failed, 1);
        SDL_AtomicSet(&p->init_done, 1);
        return 1;
    }

    SDL_AtomicSet(&p->init_done, 1);

    while (!SDL_AtomicGet(&p->quit)) {
        int ret = demuxer_read(&p->dmx);
        if (ret < 0) {
            SDL_AtomicSet(&p->eof, 1);
            break;
        }

        if (p->has_video && p->dmx.pkt->stream_index == p->dmx.video_stream_idx) {
            video_decoder_send(&p->vdec, p->dmx.pkt);
            uint8_t *rgba = NULL;
            if (video_decoder_receive(&p->vdec, &rgba) == 0 && rgba) {
                int size = p->width * p->height * 4;
                SDL_LockMutex(p->frame_mu);
                if (p->frame_buf_size < size) {
                    p->frame_buf = realloc(p->frame_buf, size);
                    p->frame_buf_size = size;
                }
                memcpy(p->frame_buf, rgba, size);
                p->frame_ready = 1;
                SDL_UnlockMutex(p->frame_mu);
            }
        } else if (p->has_audio && p->dmx.pkt->stream_index == p->dmx.audio_stream_idx) {
            audio_decoder_send(&p->adec, p->dmx.pkt);
            audio_decoder_receive(&p->adec);
        }
        av_packet_unref(p->dmx.pkt);
    }

    return 0;
}

int main(int argc, char **argv) {
    int log_level = AV_LOG_TRACE;
    int opt;

    while ((opt = getopt(argc, argv, "l:h")) != -1) {
        switch (opt) {
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
                fprintf(stderr, "  -l LEVEL  log level: QUIET, ERROR, WARNING, INFO, DEBUG, TRACE (default TRACE)\n");
                fprintf(stderr, "  -h        show this help\n");
                return opt == 'h' ? 0 : 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "error: missing url argument\n");
        return 1;
    }

    const char *url = argv[optind];
    av_log_set_level(log_level);
    av_log_set_callback(av_log_default_callback);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "error: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    fprintf(stdout, "loading: %s\n", url);

    Player player = {0};
    player.url = url;
    SDL_AtomicSet(&player.quit, 0);
    SDL_AtomicSet(&player.init_done, 0);
    SDL_AtomicSet(&player.init_failed, 0);
    SDL_AtomicSet(&player.eof, 0);
    player.frame_mu = SDL_CreateMutex();

    SDL_Thread *io = SDL_CreateThread(io_thread, "io", &player);

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    int running = 1;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_q)) {
                running = 0;
            }
        }

        if (!SDL_AtomicGet(&player.init_done)) {
            SDL_Delay(10);
            continue;
        }

        if (SDL_AtomicGet(&player.init_failed)) {
            running = 0;
            break;
        }

        if (player.has_video && !window) {
            window = SDL_CreateWindow("hpl", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                      player.width, player.height, SDL_WINDOW_SHOWN);
            if (!window) {
                fprintf(stderr, "error: SDL_CreateWindow: %s\n", SDL_GetError());
                running = 0;
                break;
            }
            SDL_RaiseWindow(window);

            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!renderer) {
                fprintf(stderr, "error: SDL_CreateRenderer: %s\n", SDL_GetError());
                running = 0;
                break;
            }

            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                        player.width, player.height);
            if (!texture) {
                fprintf(stderr, "error: SDL_CreateTexture: %s\n", SDL_GetError());
                running = 0;
                break;
            }
        }

        if (player.has_video) {
            SDL_LockMutex(player.frame_mu);
            if (player.frame_ready && texture) {
                SDL_UpdateTexture(texture, NULL, player.frame_buf, player.width * 4);
                player.frame_ready = 0;
            }
            SDL_UnlockMutex(player.frame_mu);

            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);
        } else {
            if (SDL_AtomicGet(&player.eof) &&
                (!player.has_audio || SDL_GetQueuedAudioSize(player.adec.dev) == 0)) {
                running = 0;
            }
            SDL_Delay(10);
        }
    }

    SDL_AtomicSet(&player.quit, 1);
    SDL_WaitThread(io, NULL);

    if (player.has_video) video_decoder_deinit(&player.vdec);
    if (player.has_audio) audio_decoder_deinit(&player.adec);
    demuxer_deinit(&player.dmx);
    if (player.frame_mu) SDL_DestroyMutex(player.frame_mu);
    free(player.frame_buf);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
