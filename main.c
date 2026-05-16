#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VQ_SIZE 8
#define SYNC_THRESHOLD_SEC 0.01
#define DROP_THRESHOLD_SEC 0.10
#define SEEK_STEP_SEC 5.0
#define SEEK_STEP_BIG_SEC 10.0
#define VOLUME_STEP ((int)(SDL_MIX_MAXVOLUME / 20))
#define OSD_HOLD_US 1500000

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
    AVRational time_base;
    AVRational frame_rate;
    int64_t frame_counter;
} VideoDecoder;

typedef struct {
    const AVCodec *codec;
    AVCodecContext *codec_ctx;
    AVFrame *frame;
    SwrContext *swr_ctx;
    uint8_t *buf;
    int buf_size;
    uint8_t *scratch;
    int scratch_size;
    SDL_AudioDeviceID dev;
    AVRational time_base;
    double clock_written;
    int byte_rate;
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

    vdec->sws_ctx = sws_getContext(vdec->codec_ctx->width, vdec->codec_ctx->height, vdec->codec_ctx->pix_fmt, out_w, out_h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    if (!vdec->sws_ctx) return AVERROR(EINVAL);

    vdec->rgba_frame->format = AV_PIX_FMT_RGBA;
    vdec->rgba_frame->width = out_w;
    vdec->rgba_frame->height = out_h;

    vdec->time_base = stream->time_base;

    vdec->frame_rate = av_guess_frame_rate(dmx->fmt_ctx, stream, NULL);
    if (vdec->frame_rate.num <= 0 || vdec->frame_rate.den <= 0) {
        vdec->frame_rate = (AVRational){30, 1};
        av_log(NULL, AV_LOG_WARNING, "could not determine frame rate, defaulting to %d/%d\n", vdec->frame_rate.num, vdec->frame_rate.den);
    }
    vdec->frame_counter = 0;

    return 0;
}

static int video_decoder_send(VideoDecoder *vdec, AVPacket *pkt) {
    return avcodec_send_packet(vdec->codec_ctx, pkt);
}

static int video_decoder_receive(VideoDecoder *vdec, uint8_t **out_rgba, double *out_pts) {
    int ret = avcodec_receive_frame(vdec->codec_ctx, vdec->frame);
    if (ret < 0) return ret;

    sws_scale_frame(vdec->sws_ctx, vdec->rgba_frame, vdec->frame);
    *out_rgba = vdec->rgba_frame->data[0];

    int64_t pts = vdec->frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) pts = vdec->frame->pts;
    if (pts == AV_NOPTS_VALUE) {
        *out_pts = vdec->frame_counter * av_q2d(av_inv_q(vdec->frame_rate));
    } else {
        *out_pts = pts * av_q2d(vdec->time_base);
    }
    vdec->frame_counter++;
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
    adec->scratch = NULL;
    adec->scratch_size = 0;
    adec->dev = 0;
    adec->clock_written = 0.0;

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
    av_opt_set_chlayout(adec->swr_ctx, "in_chlayout", &adec->codec_ctx->ch_layout, 0);
    av_opt_set_chlayout(adec->swr_ctx, "out_chlayout", &out_ch_layout, 0);
    av_opt_set_int(adec->swr_ctx, "in_sample_rate", adec->codec_ctx->sample_rate, 0);
    av_opt_set_int(adec->swr_ctx, "out_sample_rate", out_sample_rate, 0);
    av_opt_set_sample_fmt(adec->swr_ctx, "in_sample_fmt", adec->codec_ctx->sample_fmt, 0);
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

    adec->time_base = stream->time_base;
    adec->byte_rate = out_sample_rate * out_channels * sizeof(int16_t);

    return 0;
}

static int audio_decoder_send(AudioDecoder *adec, AVPacket *pkt) {
    return avcodec_send_packet(adec->codec_ctx, pkt);
}

static int audio_decoder_receive(AudioDecoder *adec, int volume) {
    int ret;
    while ((ret = avcodec_receive_frame(adec->codec_ctx, adec->frame)) >= 0) {
        int64_t pts_raw = adec->frame->best_effort_timestamp;
        if (pts_raw == AV_NOPTS_VALUE) pts_raw = adec->frame->pts;
        double frame_pts = (pts_raw == AV_NOPTS_VALUE) ? -1.0 : pts_raw * av_q2d(adec->time_base);

        int out_samples = swr_get_out_samples(adec->swr_ctx, adec->frame->nb_samples);
        int needed = out_samples * 2 * sizeof(int16_t);
        if (needed > adec->buf_size) {
            adec->buf = realloc(adec->buf, needed);
            adec->buf_size = needed;
        }

        uint8_t *out_buf = adec->buf;
        int converted = swr_convert(adec->swr_ctx, &out_buf, out_samples, (const uint8_t **)adec->frame->extended_data, adec->frame->nb_samples);
        if (converted > 0) {
            int bytes = converted * 2 * sizeof(int16_t);
            if (volume >= SDL_MIX_MAXVOLUME) {
                SDL_QueueAudio(adec->dev, adec->buf, bytes);
            } else if (volume <= 0) {
                if (bytes > adec->scratch_size) {
                    adec->scratch = realloc(adec->scratch, bytes);
                    adec->scratch_size = bytes;
                }
                memset(adec->scratch, 0, bytes);
                SDL_QueueAudio(adec->dev, adec->scratch, bytes);
            } else {
                if (bytes > adec->scratch_size) {
                    adec->scratch = realloc(adec->scratch, bytes);
                    adec->scratch_size = bytes;
                }
                memset(adec->scratch, 0, bytes);
                SDL_MixAudioFormat(adec->scratch, adec->buf, AUDIO_S16SYS, bytes, volume);
                SDL_QueueAudio(adec->dev, adec->scratch, bytes);
            }
            double duration = (double)converted / adec->codec_ctx->sample_rate;
            if (frame_pts >= 0) {
                adec->clock_written = frame_pts + duration;
            } else {
                adec->clock_written += duration;
            }
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
    free(adec->scratch);
}

typedef struct {
    uint8_t *buf;
    int size;
    double pts;
} VFrame;

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

    SDL_atomic_t paused;
    SDL_atomic_t seek_serial;
    SDL_atomic_t volume;
    SDL_atomic_t muted;
    SDL_atomic_t fullscreen;
    double seek_target_sec;
    double duration_sec;

    SDL_mutex *vq_mu;
    SDL_cond *vq_not_full;
    VFrame vq[VQ_SIZE];
    int vq_head, vq_tail, vq_count;

    SDL_mutex *clock_mu;
    double audio_clock_written;
    double pause_clock;
} Player;

static double get_master_clock(Player *p, int64_t start_wall_us, double start_pts) {
    if (SDL_AtomicGet(&p->paused)) {
        SDL_LockMutex(p->clock_mu);
        double pc = p->pause_clock;
        SDL_UnlockMutex(p->clock_mu);
        if (!isnan(pc)) return pc;
    }
    if (p->has_audio) {
        SDL_LockMutex(p->clock_mu);
        double c = p->audio_clock_written;
        SDL_UnlockMutex(p->clock_mu);
        if (!isnan(c)) {
            int queued = SDL_GetQueuedAudioSize(p->adec.dev);
            c -= (double)queued / (double)p->adec.byte_rate;
            return c;
        }
    }
    return (av_gettime_relative() - start_wall_us) / 1000000.0 + start_pts;
}

static void push_video_frame(Player *p, uint8_t *rgba, double pts, int expected_serial) {
    SDL_LockMutex(p->vq_mu);
    while (p->vq_count == VQ_SIZE && !SDL_AtomicGet(&p->quit) && SDL_AtomicGet(&p->seek_serial) == expected_serial) {
        SDL_CondWaitTimeout(p->vq_not_full, p->vq_mu, 20);
    }
    if (SDL_AtomicGet(&p->quit) || SDL_AtomicGet(&p->seek_serial) != expected_serial) {
        SDL_UnlockMutex(p->vq_mu);
        return;
    }
    VFrame *f = &p->vq[p->vq_tail];
    int size = p->width * p->height * 4;
    if (f->size < size) {
        f->buf = realloc(f->buf, size);
        f->size = size;
    }
    memcpy(f->buf, rgba, size);
    f->pts = pts;
    p->vq_tail = (p->vq_tail + 1) % VQ_SIZE;
    p->vq_count++;
    SDL_UnlockMutex(p->vq_mu);
}

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

    if (p->dmx.fmt_ctx->duration > 0) {
        p->duration_sec = (double)p->dmx.fmt_ctx->duration / AV_TIME_BASE;
    }

    if (p->dmx.video_stream_idx >= 0) {
        AVCodecParameters *cp = p->dmx.fmt_ctx->streams[p->dmx.video_stream_idx]->codecpar;
        p->width = cp->width;
        p->height = cp->height;
        if (video_decoder_init(&p->vdec, &p->dmx, p->width, p->height) == 0) {
            p->has_video = 1;
            fprintf(stdout, "video: %s (%dx%d @ %.3f fps)\n", p->vdec.codec->name, p->width, p->height, av_q2d(p->vdec.frame_rate));
        } else {
            fprintf(stderr, "warning: video stream found but decoder init failed\n");
        }
    }

    if (p->dmx.audio_stream_idx >= 0) {
        if (audio_decoder_init(&p->adec, &p->dmx) == 0) {
            p->has_audio = 1;
            fprintf(stdout, "audio: %s (%d Hz, %d ch)\n", p->adec.codec->name, p->adec.codec_ctx->sample_rate, p->adec.codec_ctx->ch_layout.nb_channels);
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

    int local_serial = SDL_AtomicGet(&p->seek_serial);
    int seek_failed = 0;

    while (!SDL_AtomicGet(&p->quit)) {
        int s = SDL_AtomicGet(&p->seek_serial);
        if (s != local_serial) {
            double tgt;
            SDL_LockMutex(p->clock_mu);
            tgt = p->seek_target_sec;
            SDL_UnlockMutex(p->clock_mu);

            int sidx = p->has_video ? p->dmx.video_stream_idx : p->has_audio ? p->dmx.audio_stream_idx : -1;
            int64_t ts;
            if (sidx >= 0) {
                AVStream *st = p->dmx.fmt_ctx->streams[sidx];
                ts = av_rescale_q((int64_t)(tgt * AV_TIME_BASE), AV_TIME_BASE_Q, st->time_base);
            } else {
                ts = (int64_t)(tgt * AV_TIME_BASE);
            }

            int sret = av_seek_frame(p->dmx.fmt_ctx, sidx, ts, AVSEEK_FLAG_BACKWARD);
            if (sret < 0) {
                sret = avformat_seek_file(p->dmx.fmt_ctx, sidx, INT64_MIN, ts, INT64_MAX, AVSEEK_FLAG_BACKWARD);
            }
            if (sret < 0) {
                sret = av_seek_frame(p->dmx.fmt_ctx, sidx, ts, AVSEEK_FLAG_ANY);
            }

            if (sret < 0) {
                fprintf(stderr, "warning: seek to %.3fs failed: %s\n", tgt, av_err2str(sret));
                seek_failed = 1;
                local_serial = s;
                continue;
            }

            seek_failed = 0;
            if (p->has_video) avcodec_flush_buffers(p->vdec.codec_ctx);
            if (p->has_audio) {
                avcodec_flush_buffers(p->adec.codec_ctx);
                SDL_ClearQueuedAudio(p->adec.dev);
                p->adec.clock_written = NAN;
            }
            SDL_LockMutex(p->vq_mu);
            p->vq_head = p->vq_tail = p->vq_count = 0;
            SDL_UnlockMutex(p->vq_mu);
            SDL_CondSignal(p->vq_not_full);
            SDL_LockMutex(p->clock_mu);
            p->audio_clock_written = NAN;
            SDL_UnlockMutex(p->clock_mu);
            SDL_AtomicSet(&p->eof, 0);
            local_serial = s;
            continue;
        }

        int ret = demuxer_read(&p->dmx);
        if (ret < 0) {
            if (seek_failed) {
                seek_failed = 0;
                while (!SDL_AtomicGet(&p->quit) && SDL_AtomicGet(&p->seek_serial) == local_serial) {
                    SDL_Delay(20);
                }
                continue;
            }
            SDL_AtomicSet(&p->eof, 1);
            while (!SDL_AtomicGet(&p->quit) && SDL_AtomicGet(&p->seek_serial) == local_serial) {
                SDL_Delay(20);
            }
            continue;
        }

        if (p->has_video && p->dmx.pkt->stream_index == p->dmx.video_stream_idx) {
            video_decoder_send(&p->vdec, p->dmx.pkt);
            for (;;) {
                uint8_t *rgba = NULL;
                double pts = 0.0;
                int rr = video_decoder_receive(&p->vdec, &rgba, &pts);
                if (rr < 0) break;
                if (rgba) push_video_frame(p, rgba, pts, local_serial);
                if (SDL_AtomicGet(&p->quit)) break;
                if (SDL_AtomicGet(&p->seek_serial) != local_serial) break;
            }
        } else if (p->has_audio && p->dmx.pkt->stream_index == p->dmx.audio_stream_idx) {
            int vol = SDL_AtomicGet(&p->muted) ? 0 : SDL_AtomicGet(&p->volume);
            audio_decoder_send(&p->adec, p->dmx.pkt);
            audio_decoder_receive(&p->adec, vol);
            if (SDL_AtomicGet(&p->seek_serial) == local_serial) {
                SDL_LockMutex(p->clock_mu);
                p->audio_clock_written = p->adec.clock_written;
                SDL_UnlockMutex(p->clock_mu);
            }
        }
        av_packet_unref(p->dmx.pkt);
    }

    return 0;
}

typedef struct {
    TTF_Font *font;
    int line_h;
} OSD;

static int osd_init(OSD *osd) {
    osd->font = NULL;
    osd->line_h = 20;
    if (TTF_Init() != 0) {
        fprintf(stderr, "warning: TTF_Init failed: %s\n", TTF_GetError());
        return -1;
    }
    static const char *paths[] = {
        "/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", NULL,
    };
    for (int i = 0; paths[i]; i++) {
        osd->font = TTF_OpenFont(paths[i], 20);
        if (osd->font) break;
    }
    if (!osd->font) {
        fprintf(stderr, "warning: no system font found, OSD text disabled\n");
        return -1;
    }
    osd->line_h = TTF_FontLineSkip(osd->font);
    return 0;
}

static void osd_deinit(OSD *osd) {
    if (osd->font) {
        TTF_CloseFont(osd->font);
        osd->font = NULL;
    }
    if (TTF_WasInit()) TTF_Quit();
}

static void osd_draw_text(SDL_Renderer *r, OSD *osd, const char *txt, int x, int y, SDL_Color col) {
    if (!osd->font || !txt || !*txt) return;
    SDL_Surface *s = TTF_RenderUTF8_Blended(osd->font, txt, col);
    if (!s) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
    if (t) {
        SDL_Rect dst = {x, y, s->w, s->h};
        SDL_RenderCopy(r, t, NULL, &dst);
        SDL_DestroyTexture(t);
    }
    SDL_FreeSurface(s);
}

static void fmt_time(double sec, char *out, size_t n) {
    if (!isfinite(sec) || sec < 0) sec = 0;
    int s = (int)sec;
    int h = s / 3600;
    int m = (s / 60) % 60;
    int ss = s % 60;
    if (h > 0)
        snprintf(out, n, "%d:%02d:%02d", h, m, ss);
    else
        snprintf(out, n, "%d:%02d", m, ss);
}

static void osd_draw(SDL_Renderer *r, OSD *osd, Player *p, double clock, int64_t now_us, int64_t vol_until_us, int64_t seek_until_us, int win_w, int win_h) {
    SDL_Color white = {255, 255, 255, 255};
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    if (SDL_AtomicGet(&p->paused)) {
        SDL_SetRenderDrawColor(r, 255, 255, 255, 210);
        int bw = 14, bh = 60, gap = 10;
        int cx = win_w / 2;
        int cy = 60;
        SDL_Rect a = {cx - bw - gap / 2, cy - bh / 2, bw, bh};
        SDL_Rect b = {cx + gap / 2, cy - bh / 2, bw, bh};
        SDL_RenderFillRect(r, &a);
        SDL_RenderFillRect(r, &b);
    }

    if (now_us < vol_until_us) {
        int vol = SDL_AtomicGet(&p->volume);
        int muted = SDL_AtomicGet(&p->muted);
        char buf[64];
        int pct = (int)((vol * 100 + SDL_MIX_MAXVOLUME / 2) / SDL_MIX_MAXVOLUME);
        if (muted)
            snprintf(buf, sizeof(buf), "MUTED");
        else
            snprintf(buf, sizeof(buf), "vol %d%%", pct);

        int bar_w = 200, bar_h = 12;
        int bar_x = win_w - bar_w - 20;
        int bar_y = win_h - bar_h - 30;
        SDL_SetRenderDrawColor(r, 255, 255, 255, 70);
        SDL_Rect bg = {bar_x, bar_y, bar_w, bar_h};
        SDL_RenderFillRect(r, &bg);
        if (!muted) {
            SDL_SetRenderDrawColor(r, 255, 255, 255, 220);
            SDL_Rect fg = {bar_x, bar_y, (bar_w * vol) / SDL_MIX_MAXVOLUME, bar_h};
            SDL_RenderFillRect(r, &fg);
        } else {
            SDL_SetRenderDrawColor(r, 220, 60, 60, 220);
            SDL_RenderDrawLine(r, bar_x, bar_y, bar_x + bar_w, bar_y + bar_h);
            SDL_RenderDrawLine(r, bar_x, bar_y + bar_h, bar_x + bar_w, bar_y);
        }
        osd_draw_text(r, osd, buf, bar_x, bar_y - osd->line_h - 4, white);
    }

    if (now_us < seek_until_us) {
        char a[32], b[32], line[80];
        fmt_time(clock, a, sizeof(a));
        if (p->duration_sec > 0) {
            fmt_time(p->duration_sec, b, sizeof(b));
            snprintf(line, sizeof(line), "%s / %s", a, b);
        } else {
            snprintf(line, sizeof(line), "%s", a);
        }
        osd_draw_text(r, osd, line, 20, 20, white);
    }
}

int main(int argc, char **argv) {
    int log_level = AV_LOG_TRACE;
    int opt;

    while ((opt = getopt(argc, argv, "l:h")) != -1) {
        switch (opt) {
        case 'l': {
            if (strcmp(optarg, "QUIET") == 0)
                log_level = AV_LOG_QUIET;
            else if (strcmp(optarg, "ERROR") == 0)
                log_level = AV_LOG_ERROR;
            else if (strcmp(optarg, "WARNING") == 0)
                log_level = AV_LOG_WARNING;
            else if (strcmp(optarg, "INFO") == 0)
                log_level = AV_LOG_INFO;
            else if (strcmp(optarg, "DEBUG") == 0)
                log_level = AV_LOG_DEBUG;
            else if (strcmp(optarg, "TRACE") == 0)
                log_level = AV_LOG_TRACE;
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
    SDL_AtomicSet(&player.paused, 0);
    SDL_AtomicSet(&player.seek_serial, 0);
    SDL_AtomicSet(&player.volume, SDL_MIX_MAXVOLUME);
    SDL_AtomicSet(&player.muted, 0);
    SDL_AtomicSet(&player.fullscreen, 0);
    player.seek_target_sec = 0.0;
    player.duration_sec = 0.0;
    player.vq_mu = SDL_CreateMutex();
    player.vq_not_full = SDL_CreateCond();
    player.clock_mu = SDL_CreateMutex();
    player.audio_clock_written = NAN;
    player.pause_clock = NAN;

    SDL_Thread *io = SDL_CreateThread(io_thread, "io", &player);

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    OSD osd = {0};
    int osd_ready = 0;
    int running = 1;

    int64_t start_wall_us = 0;
    double start_pts = 0.0;
    int clock_origin_set = 0;
    int64_t osd_vol_until_us = 0;
    int64_t osd_seek_until_us = 0;

    while (running) {
        int64_t now_us = av_gettime_relative();

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = 0;
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                int shift = (ev.key.keysym.mod & KMOD_SHIFT) != 0;
                if (k == SDLK_q) {
                    running = 0;
                } else if (k == SDLK_SPACE) {
                    int was_paused = SDL_AtomicGet(&player.paused);
                    if (!was_paused) {
                        double cur = get_master_clock(&player, start_wall_us, start_pts);
                        SDL_LockMutex(player.clock_mu);
                        player.pause_clock = cur;
                        SDL_UnlockMutex(player.clock_mu);
                        SDL_AtomicSet(&player.paused, 1);
                        if (player.has_audio) SDL_PauseAudioDevice(player.adec.dev, 1);
                        fprintf(stdout, "paused at %.3fs\n", cur);
                    } else {
                        SDL_AtomicSet(&player.paused, 0);
                        SDL_LockMutex(player.clock_mu);
                        player.pause_clock = NAN;
                        SDL_UnlockMutex(player.clock_mu);
                        if (player.has_audio) SDL_PauseAudioDevice(player.adec.dev, 0);
                        if (!player.has_audio) clock_origin_set = 0;
                        fprintf(stdout, "resumed\n");
                    }
                } else if (k == SDLK_LEFT || k == SDLK_RIGHT) {
                    double base;
                    if (SDL_AtomicGet(&player.paused)) {
                        SDL_LockMutex(player.clock_mu);
                        base = player.pause_clock;
                        SDL_UnlockMutex(player.clock_mu);
                        if (!isfinite(base)) base = 0;
                    } else {
                        base = get_master_clock(&player, start_wall_us, start_pts);
                    }
                    double step = shift ? SEEK_STEP_BIG_SEC : SEEK_STEP_SEC;
                    double delta = (k == SDLK_LEFT) ? -step : step;
                    double target = base + delta;
                    if (target < 0) target = 0;
                    if (player.duration_sec > 0 && target > player.duration_sec - 0.5) target = player.duration_sec - 0.5;
                    SDL_LockMutex(player.clock_mu);
                    player.seek_target_sec = target;
                    if (SDL_AtomicGet(&player.paused)) player.pause_clock = target;
                    SDL_UnlockMutex(player.clock_mu);
                    SDL_AtomicAdd(&player.seek_serial, 1);
                    SDL_LockMutex(player.vq_mu);
                    SDL_CondSignal(player.vq_not_full);
                    SDL_UnlockMutex(player.vq_mu);
                    clock_origin_set = 0;
                    osd_seek_until_us = now_us + OSD_HOLD_US;
                    fprintf(stdout, "seek to %.3fs\n", target);
                } else if (k == SDLK_UP || k == SDLK_DOWN) {
                    int v = SDL_AtomicGet(&player.volume);
                    v += (k == SDLK_UP ? VOLUME_STEP : -VOLUME_STEP);
                    if (v < 0) v = 0;
                    if (v > SDL_MIX_MAXVOLUME) v = SDL_MIX_MAXVOLUME;
                    SDL_AtomicSet(&player.volume, v);
                    if (v > 0) SDL_AtomicSet(&player.muted, 0);
                    osd_vol_until_us = now_us + OSD_HOLD_US;
                    fprintf(stdout, "volume %d%%\n", (v * 100 + SDL_MIX_MAXVOLUME / 2) / SDL_MIX_MAXVOLUME);
                } else if (k == SDLK_m) {
                    int m = !SDL_AtomicGet(&player.muted);
                    SDL_AtomicSet(&player.muted, m);
                    osd_vol_until_us = now_us + OSD_HOLD_US;
                    fprintf(stdout, m ? "muted\n" : "unmuted\n");
                } else if (k == SDLK_f) {
                    int fs = !SDL_AtomicGet(&player.fullscreen);
                    SDL_AtomicSet(&player.fullscreen, fs);
                    if (window) {
                        SDL_SetWindowFullscreen(window, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    }
                }
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
            window = SDL_CreateWindow("hpl", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, player.width, player.height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
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
            SDL_RenderSetLogicalSize(renderer, player.width, player.height);

            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, player.width, player.height);
            if (!texture) {
                fprintf(stderr, "error: SDL_CreateTexture: %s\n", SDL_GetError());
                running = 0;
                break;
            }

            osd_ready = (osd_init(&osd) == 0);
        }

        if (player.has_video) {
            int paused = SDL_AtomicGet(&player.paused);
            SDL_LockMutex(player.vq_mu);
            VFrame *display = NULL;
            if (!paused) {
                while (player.vq_count > 0) {
                    VFrame *f = &player.vq[player.vq_head];
                    if (!clock_origin_set) {
                        start_wall_us = av_gettime_relative();
                        start_pts = f->pts;
                        clock_origin_set = 1;
                    }
                    double clock = get_master_clock(&player, start_wall_us, start_pts);
                    if (f->pts > clock + SYNC_THRESHOLD_SEC) break;

                    display = f;
                    int is_last = (player.vq_count == 1);
                    int next_idx = (player.vq_head + 1) % VQ_SIZE;
                    int too_late = 0;
                    if (!is_last) {
                        VFrame *nxt = &player.vq[next_idx];
                        if (nxt->pts <= clock - DROP_THRESHOLD_SEC) too_late = 1;
                    }
                    player.vq_head = next_idx;
                    player.vq_count--;
                    if (!too_late) break;
                }
            }
            if (display && texture) {
                SDL_UpdateTexture(texture, NULL, display->buf, player.width * 4);
            }
            SDL_UnlockMutex(player.vq_mu);
            SDL_CondSignal(player.vq_not_full);

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, NULL, NULL);

            int win_w = player.width, win_h = player.height;
            double clk;
            if (paused) {
                SDL_LockMutex(player.clock_mu);
                clk = player.pause_clock;
                SDL_UnlockMutex(player.clock_mu);
                if (!isfinite(clk)) clk = 0;
            } else {
                clk = get_master_clock(&player, start_wall_us, start_pts);
            }
            if (osd_ready) {
                osd_draw(renderer, &osd, &player, clk, now_us, osd_vol_until_us, osd_seek_until_us, win_w, win_h);
            }
            SDL_RenderPresent(renderer);

            if (!paused && SDL_AtomicGet(&player.eof) && player.vq_count == 0) running = 0;
        } else {
            if (!SDL_AtomicGet(&player.paused) && SDL_AtomicGet(&player.eof) && (!player.has_audio || SDL_GetQueuedAudioSize(player.adec.dev) == 0)) {
                running = 0;
            }
            SDL_Delay(10);
        }
    }

    SDL_AtomicSet(&player.quit, 1);
    if (player.has_audio) SDL_PauseAudioDevice(player.adec.dev, 0);
    SDL_LockMutex(player.vq_mu);
    SDL_CondBroadcast(player.vq_not_full);
    SDL_UnlockMutex(player.vq_mu);
    SDL_WaitThread(io, NULL);

    if (osd_ready) osd_deinit(&osd);
    if (player.has_video) video_decoder_deinit(&player.vdec);
    if (player.has_audio) audio_decoder_deinit(&player.adec);
    demuxer_deinit(&player.dmx);
    for (int i = 0; i < VQ_SIZE; i++)
        free(player.vq[i].buf);
    if (player.vq_not_full) SDL_DestroyCond(player.vq_not_full);
    if (player.vq_mu) SDL_DestroyMutex(player.vq_mu);
    if (player.clock_mu) SDL_DestroyMutex(player.clock_mu);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
