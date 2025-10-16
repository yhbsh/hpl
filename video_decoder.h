#ifndef VIDEO_DECODER_H
#define VIDEO_DECODER_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    AVFormatContext *ifmt_ctx;

    const AVCodec *vcodec;
    AVCodecContext *vcodec_ctx;
    AVStream *vstream;

    AVFrame *frame;
    AVFrame *rgba_frame;
    AVPacket *pkt;
    SwsContext *sws_ctx;
} VideoDecoder;

static int vd_decoder_init(VideoDecoder *dec, const char *url, int out_width, int out_height) {
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

static int vd_decoder_next_frame(VideoDecoder *dec, uint8_t **out_rgba) {
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

static void vd_decoder_close(VideoDecoder *dec) {
    if (dec->sws_ctx) sws_freeContext(dec->sws_ctx);
    if (dec->rgba_frame) av_frame_free(&dec->rgba_frame);
    if (dec->frame) av_frame_free(&dec->frame);
    if (dec->pkt) av_packet_free(&dec->pkt);
    if (dec->vcodec_ctx) avcodec_free_context(&dec->vcodec_ctx);
    if (dec->ifmt_ctx) avformat_close_input(&dec->ifmt_ctx);
}

#endif // VIDEO_DECODER_H
