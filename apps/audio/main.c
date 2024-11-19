#include <stdio.h>
#include <stdlib.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#include <portaudio.h>

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        printf("USAGE: %s <url>\n", argv[0]);
        return 1;
    }

    int ret;

    AVFormatContext *format_context = NULL;
    if ((ret = avformat_open_input(&format_context, argv[1], NULL, NULL)) < 0) {
        fprintf(stderr, "ERROR: avformat_open_input %s\n", av_err2str(ret));
        goto cleanup_ffmpeg;
    }

    if ((ret = avformat_find_stream_info(format_context, NULL)) < 0) {
        fprintf(stderr, "ERROR: avformat_find_stream_info %s\n", av_err2str(ret));
        goto cleanup_ffmpeg;
    }

    // Find audio stream
    const AVCodec *codec = NULL;
    int stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream_index < 0) {
        fprintf(stderr, "ERROR: Could not find audio stream\n");
        goto cleanup_ffmpeg;
    }

    AVCodecContext *codec_context = avcodec_alloc_context3(codec);
    if ((ret = avcodec_parameters_to_context(codec_context, format_context->streams[stream_index]->codecpar)) < 0) {
        fprintf(stderr, "ERROR: avcodec_parameters_to_context %s\n", av_err2str(ret));
        goto cleanup_codec_context;
    }

    if ((ret = avcodec_open2(codec_context, codec, NULL)) < 0) {
        fprintf(stderr, "ERROR: avcodec_open2 %s\n", av_err2str(ret));
        goto cleanup_codec_context;
    }

    // Initialize resampler
    SwrContext *swr_ctx = swr_alloc();
    if (!swr_ctx) {
        fprintf(stderr, "ERROR: swr_alloc failed\n");
        goto cleanup_codec_context;
    }

    // Set options
    av_opt_set_chlayout(swr_ctx, "in_chlayout", &codec_context->ch_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", codec_context->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", codec_context->sample_fmt, 0);

    // Output is stereo 16-bit signed integer PCM at 44100 Hz
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, 2); // Stereo output
    int out_sample_rate = 44100;
    enum AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;

    av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_ch_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", out_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", out_sample_fmt, 0);

    if ((ret = swr_init(swr_ctx)) < 0) {
        fprintf(stderr, "ERROR: swr_init %s\n", av_err2str(ret));
        goto cleanup_swr;
    }

    // Allocate frames and packet
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        fprintf(stderr, "ERROR: av_packet_alloc failed\n");
        goto cleanup_swr;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "ERROR: av_frame_alloc failed\n");
        goto cleanup_packet;
    }

    // Initialize PortAudio
    PaStream *pa_stream;
    PaError pa_err;

    pa_err = Pa_Initialize();
    if (pa_err != paNoError) {
        fprintf(stderr, "ERROR: Failed to initialize PortAudio: %s\n", Pa_GetErrorText(pa_err));
        goto cleanup_frame;
    }

    PaStreamParameters outputParameters;
    outputParameters.device = Pa_GetDefaultOutputDevice(); /* default output device */
    if (outputParameters.device == paNoDevice) {
        fprintf(stderr, "ERROR: No default output device.\n");
        goto cleanup_pa;
    }
    outputParameters.channelCount = 2;       /* stereo output */
    outputParameters.sampleFormat = paInt16; /* 16 bit integer output */
    outputParameters.suggestedLatency = Pa_GetDeviceInfo(outputParameters.device)->defaultLowOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = NULL;

    pa_err = Pa_OpenStream(
        &pa_stream,
        NULL, /* no input */
        &outputParameters,
        out_sample_rate,
        paFramesPerBufferUnspecified,
        paClipOff,
        NULL, /* no callback */
        NULL  /* no user data */
    );
    if (pa_err != paNoError) {
        fprintf(stderr, "ERROR: Failed to open PortAudio stream: %s\n", Pa_GetErrorText(pa_err));
        goto cleanup_pa;
    }

    pa_err = Pa_StartStream(pa_stream);
    if (pa_err != paNoError) {
        fprintf(stderr, "ERROR: Failed to start PortAudio stream: %s\n", Pa_GetErrorText(pa_err));
        goto cleanup_stream;
    }

    // Main decoding and playback loop
    while (1) {
        ret = av_read_frame(format_context, packet);
        if (ret == AVERROR_EOF) break;
        if (ret == AVERROR(EAGAIN)) continue;
        if (ret < 0) {
            fprintf(stderr, "ERROR: av_read_frame %s\n", av_err2str(ret));
            break;
        }

        if (packet->stream_index == stream_index) {
            ret = avcodec_send_packet(codec_context, packet);
            if (ret < 0) {
                fprintf(stderr, "ERROR: avcodec_send_packet %s\n", av_err2str(ret));
                av_packet_unref(packet);
                break;
            }
            while (ret >= 0) {
                ret = avcodec_receive_frame(codec_context, frame);
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
                if (ret < 0) {
                    fprintf(stderr, "ERROR: avcodec_receive_frame %s\n", av_err2str(ret));
                    goto cleanup_stream;
                }

                // Update resampler if input params change
                if (!av_channel_layout_compare(&frame->ch_layout, &codec_context->ch_layout) ||
                    frame->sample_rate != codec_context->sample_rate ||
                    frame->format != codec_context->sample_fmt) {

                    av_channel_layout_copy(&codec_context->ch_layout, &frame->ch_layout);
                    codec_context->sample_rate = frame->sample_rate;
                    codec_context->sample_fmt = frame->format;

                    swr_free(&swr_ctx);
                    swr_ctx = swr_alloc();
                    if (!swr_ctx) {
                        fprintf(stderr, "ERROR: swr_alloc failed\n");
                        goto cleanup_stream;
                    }

                    av_opt_set_chlayout(swr_ctx, "in_chlayout", &codec_context->ch_layout, 0);
                    av_opt_set_int(swr_ctx, "in_sample_rate", codec_context->sample_rate, 0);
                    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", codec_context->sample_fmt, 0);

                    av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_ch_layout, 0);
                    av_opt_set_int(swr_ctx, "out_sample_rate", out_sample_rate, 0);
                    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", out_sample_fmt, 0);

                    if ((ret = swr_init(swr_ctx)) < 0) {
                        fprintf(stderr, "ERROR: swr_init %s\n", av_err2str(ret));
                        goto cleanup_stream;
                    }
                }

                // Allocate buffer for resampled data
                uint8_t **resampled_data = NULL;
                int resampled_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, codec_context->sample_rate) + frame->nb_samples,
                                                          out_sample_rate, codec_context->sample_rate, AV_ROUND_UP);
                int resampled_nb_channels = out_ch_layout.nb_channels;
                ret = av_samples_alloc_array_and_samples(&resampled_data, NULL, resampled_nb_channels,
                                                         resampled_nb_samples, out_sample_fmt, 0);
                if (ret < 0) {
                    fprintf(stderr, "ERROR: av_samples_alloc_array_and_samples %s\n", av_err2str(ret));
                    goto cleanup_stream;
                }

                // Resample
                ret = swr_convert(swr_ctx, resampled_data, resampled_nb_samples,
                                  (const uint8_t **)frame->extended_data, frame->nb_samples);
                if (ret < 0) {
                    fprintf(stderr, "ERROR: swr_convert %s\n", av_err2str(ret));
                    av_freep(&resampled_data[0]);
                    av_freep(&resampled_data);
                    goto cleanup_stream;
                }

                int resampled_data_size = av_samples_get_buffer_size(NULL, resampled_nb_channels, ret, out_sample_fmt, 1);
                if (resampled_data_size < 0) {
                    fprintf(stderr, "ERROR: av_samples_get_buffer_size %s\n", av_err2str(resampled_data_size));
                    av_freep(&resampled_data[0]);
                    av_freep(&resampled_data);
                    goto cleanup_stream;
                }

                // Write data to PortAudio stream
                pa_err = Pa_WriteStream(pa_stream, resampled_data[0], ret);
                if (pa_err != paNoError) {
                    fprintf(stderr, "ERROR: Pa_WriteStream failed: %s\n", Pa_GetErrorText(pa_err));
                    av_freep(&resampled_data[0]);
                    av_freep(&resampled_data);
                    goto cleanup_stream;
                }

                av_freep(&resampled_data[0]);
                av_freep(&resampled_data);

                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }

    // Flush decoder
    avcodec_send_packet(codec_context, NULL);
    while (1) {
        ret = avcodec_receive_frame(codec_context, frame);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
        if (ret < 0) {
            fprintf(stderr, "ERROR: avcodec_receive_frame %s\n", av_err2str(ret));
            goto cleanup_stream;
        }

        // Resample and play the remaining frames (same as above)
        uint8_t **resampled_data = NULL;
        int resampled_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, codec_context->sample_rate) + frame->nb_samples,
                                                  out_sample_rate, codec_context->sample_rate, AV_ROUND_UP);
        int resampled_nb_channels = out_ch_layout.nb_channels;
        ret = av_samples_alloc_array_and_samples(&resampled_data, NULL, resampled_nb_channels,
                                                 resampled_nb_samples, out_sample_fmt, 0);
        if (ret < 0) {
            fprintf(stderr, "ERROR: av_samples_alloc_array_and_samples %s\n", av_err2str(ret));
            goto cleanup_stream;
        }

        // Resample
        ret = swr_convert(swr_ctx, resampled_data, resampled_nb_samples,
                          (const uint8_t **)frame->extended_data, frame->nb_samples);
        if (ret < 0) {
            fprintf(stderr, "ERROR: swr_convert %s\n", av_err2str(ret));
            av_freep(&resampled_data[0]);
            av_freep(&resampled_data);
            goto cleanup_stream;
        }

        int resampled_data_size = av_samples_get_buffer_size(NULL, resampled_nb_channels, ret, out_sample_fmt, 1);
        if (resampled_data_size < 0) {
            fprintf(stderr, "ERROR: av_samples_get_buffer_size %s\n", av_err2str(resampled_data_size));
            av_freep(&resampled_data[0]);
            av_freep(&resampled_data);
            goto cleanup_stream;
        }

        // Write data to PortAudio stream
        pa_err = Pa_WriteStream(pa_stream, resampled_data[0], ret);
        if (pa_err != paNoError) {
            fprintf(stderr, "ERROR: Pa_WriteStream failed: %s\n", Pa_GetErrorText(pa_err));
            av_freep(&resampled_data[0]);
            av_freep(&resampled_data);
            goto cleanup_stream;
        }

        av_freep(&resampled_data[0]);
        av_freep(&resampled_data);

        av_frame_unref(frame);
    }

    // Stop PortAudio stream
    pa_err = Pa_StopStream(pa_stream);
    if (pa_err != paNoError) {
        fprintf(stderr, "ERROR: Pa_StopStream failed: %s\n", Pa_GetErrorText(pa_err));
        goto cleanup_stream;
    }

    // Cleanup and exit
cleanup_stream:
    Pa_CloseStream(pa_stream);
cleanup_pa:
    Pa_Terminate();
cleanup_frame:
    av_frame_free(&frame);
cleanup_packet:
    av_packet_free(&packet);
cleanup_swr:
    swr_free(&swr_ctx);
    av_channel_layout_uninit(&out_ch_layout);
cleanup_codec_context:
    avcodec_free_context(&codec_context);
cleanup_ffmpeg:
    avformat_close_input(&format_context);
    avformat_network_deinit();

    return 0;
}
