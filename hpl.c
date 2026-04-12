#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <stdio.h>
#include <stdlib.h>

#include <raylib.h>

#include <getopt.h>
#include <string.h>

#define DEFAULT_W 1280
#define DEFAULT_H 720
#define DEFAULT_FPS 180

typedef struct {
    AVFormatContext *ifmt_ctx;

    const AVCodec *vcodec;
    AVCodecContext *vcodec_ctx;
    AVStream *vstream;

    AVFrame *frame;
    AVFrame *rgba_frame;
    AVPacket *pkt;
    SwsContext *sws_ctx;
} Decoder;

static int decoder_init(Decoder *dec, const char *url, int out_width, int out_height) {
    int ret;

    av_log_set_level(AV_LOG_TRACE);

    dec->ifmt_ctx = NULL;
    dec->vcodec_ctx = NULL;
    dec->frame = av_frame_alloc();
    dec->rgba_frame = av_frame_alloc();
    dec->pkt = av_packet_alloc();
    dec->sws_ctx = NULL;
    if (!dec->frame || !dec->rgba_frame) return AVERROR(ENOMEM);

    if ((ret = avformat_open_input(&dec->ifmt_ctx, url, NULL, NULL)) < 0) { return ret; }
    if ((ret = avformat_find_stream_info(dec->ifmt_ctx, NULL)) < 0) { return ret; }
    if ((ret = av_find_best_stream(dec->ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec->vcodec, 0)) < 0) return ret;
    dec->vstream = dec->ifmt_ctx->streams[ret];
    dec->vcodec_ctx = avcodec_alloc_context3(dec->vcodec);
    if (!dec->vcodec_ctx) return AVERROR(ENOMEM);
    if ((ret = avcodec_parameters_to_context(dec->vcodec_ctx, dec->vstream->codecpar)) < 0) return ret;
    if ((ret = avcodec_open2(dec->vcodec_ctx, dec->vcodec, NULL)) < 0) return ret;

    dec->sws_ctx = sws_getContext(dec->vcodec_ctx->width, dec->vcodec_ctx->height, dec->vcodec_ctx->pix_fmt, out_width, out_height, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    if (!dec->sws_ctx) return AVERROR(EINVAL);

    dec->rgba_frame->format = AV_PIX_FMT_RGBA;
    dec->rgba_frame->width = out_width;
    dec->rgba_frame->height = out_height;

    return 0;
}

static int decoder_read_frame(Decoder *dec, uint8_t **out_rgba) {
    int ret;
    while ((ret = av_read_frame(dec->ifmt_ctx, dec->pkt)) >= 0) {
        if (dec->pkt->stream_index != dec->vstream->index) {
            av_packet_unref(dec->pkt);
            continue;
        }

        if ((ret = avcodec_send_packet(dec->vcodec_ctx, dec->pkt)) < 0) {
            av_packet_unref(dec->pkt);
            return ret;
        }

        while ((ret = avcodec_receive_frame(dec->vcodec_ctx, dec->frame)) >= 0) {
            sws_scale_frame(dec->sws_ctx, dec->rgba_frame, dec->frame);

            *out_rgba = dec->rgba_frame->data[0];

            av_packet_unref(dec->pkt);
            return 1;
        }

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ret = 0;
        av_packet_unref(dec->pkt);
        if (ret < 0) return ret;
    }

    return AVERROR_EOF;
}

static void decoder_deinit(Decoder *dec) {
    if (dec->sws_ctx) sws_freeContext(dec->sws_ctx);
    if (dec->rgba_frame) av_frame_free(&dec->rgba_frame);
    if (dec->frame) av_frame_free(&dec->frame);
    if (dec->pkt) av_packet_free(&dec->pkt);
    if (dec->vcodec_ctx) avcodec_free_context(&dec->vcodec_ctx);
    if (dec->ifmt_ctx) avformat_close_input(&dec->ifmt_ctx);
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
    SetTraceLogLevel(LOG_NONE);
    InitWindow(width, height, "hpl");
    SetTargetFPS(fps);

    fprintf(stdout, "loading: %s (output: %dx%d @ %d fps)\n", url, width, height, fps);

    Decoder decoder;
    if (decoder_init(&decoder, url, width, height) != 0) {
        fprintf(stderr, "error: failed to initialize decoder\n");
        CloseWindow();
        return 1;
    }

    fprintf(stdout, "codec: %s (%dx%d)\n", decoder.vcodec->name, decoder.vcodec_ctx->width, decoder.vcodec_ctx->height);

    uint8_t *data = NULL;
    Image img = {
        .data = NULL,
        .width = width,
        .height = height,
        .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
    };

    Texture2D texture = LoadTextureFromImage(img);
    while (!WindowShouldClose()) {
        if (decoder_read_frame(&decoder, &data) == 1 && data) { UpdateTexture(texture, data); }

        BeginDrawing();
        DrawTexture(texture, 0, 0, WHITE);
        EndDrawing();
    }

    UnloadTexture(texture);
    decoder_deinit(&decoder);
    CloseWindow();
    return 0;
}
