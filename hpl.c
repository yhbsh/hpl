#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <GLFW/glfw3.h>

#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "miniaudio.h"

typedef struct {
    AVFormatContext *format_context;
    const AVCodec *video_codec;
    const AVCodec *audio_codec;
    AVCodecContext *video_codec_context;
    AVCodecContext *audio_codec_context;
    AVStream *video_stream;
    AVStream *audio_stream;
    AVPacket *packet;
    AVFrame *frame;
    AVFrame *pcm_frame;
    AVFrame *rgb_frame;
    struct SwrContext *swr_context;
    struct SwsContext *sws_context;
    ma_device audio_device;
    ma_rb rb;
    GLFWwindow *window;
    int64_t its;
} Context;

void audio_callback(ma_device *device, void *output, const void *input, ma_uint32 number_of_frames) {
    (void)input;
    int ret;

    ma_rb *rb = (ma_rb *)device->pUserData;
    size_t bytes_to_read = number_of_frames * 2 * sizeof(float);
    void *ptr;

    if ((ret = ma_rb_acquire_read(rb, &bytes_to_read, &ptr)) != MA_SUCCESS) {
        fprintf(stderr, "ERROR: cannot acquire miniaudio ring buffer for read: %s\n", ma_result_description(ret));
        memset(output, 0, number_of_frames * 2 * sizeof(float));
        return;
    }

    if (bytes_to_read > 0) {
        memcpy(output, ptr, bytes_to_read);
        if (bytes_to_read < number_of_frames * 2 * sizeof(float)) {
            memset((char *)output + bytes_to_read, 0, (number_of_frames * 2 * sizeof(float)) - bytes_to_read);
        }

        if ((ret = ma_rb_commit_read(rb, bytes_to_read)) != MA_SUCCESS) {
            fprintf(stderr, "ERROR: cannot commit miniaudio ring buffer for read: %s\n", ma_result_description(ret));
        }
    } else {
        memset(output, 0, number_of_frames * 2 * sizeof(float));
    }
}

int init_audio_resampler(Context *context) {
    int ret;
    const AVChannelLayout dst_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    enum AVSampleFormat dst_sample_format = AV_SAMPLE_FMT_FLT;
    int dst_sample_rate = 44100;

    const AVChannelLayout src_ch_layout = context->audio_codec_context->ch_layout;
    enum AVSampleFormat srt_sample_format = context->audio_codec_context->sample_fmt;
    int src_sample_rate = context->audio_codec_context->sample_rate;

    if ((ret = swr_alloc_set_opts2(&context->swr_context, &dst_ch_layout, dst_sample_format, dst_sample_rate, &src_ch_layout, srt_sample_format, src_sample_rate, 0, NULL)) < 0) {
        fprintf(stderr, "ERROR: cannot allocate swr context: %s\n", av_err2str(ret));
        return -1;
    }

    if ((ret = swr_init(context->swr_context)) < 0) {
        fprintf(stderr, "ERROR: cannot initialize swr context: %s\n", av_err2str(ret));
        return -1;
    }

    context->pcm_frame->ch_layout = dst_ch_layout;
    context->pcm_frame->sample_rate = dst_sample_rate;
    context->pcm_frame->format = dst_sample_format;
    return 0;
}

int init_video_scaler(Context *context) {
    context->sws_context = sws_getContext(context->video_codec_context->width, context->video_codec_context->height, context->video_codec_context->pix_fmt, context->video_codec_context->width, context->video_codec_context->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    if (!context->sws_context) {
        fprintf(stderr, "ERROR: cannot create software scaling context\n");
        return -1;
    }

    return 0;
}

int init_input(Context *context, const char *url) {
    int ret;
    av_log_set_level(AV_LOG_TRACE);

    AVDictionary *options = NULL;

    if ((ret = avformat_open_input(&context->format_context, url, NULL, &options)) < 0) {
        fprintf(stderr, "ERROR: cannot open input: %s\n", av_err2str(ret));
        return -1;
    }

    if ((ret = avformat_find_stream_info(context->format_context, NULL)) < 0) {
        fprintf(stderr, "ERROR: cannot find stream info: %s\n", av_err2str(ret));
        return -1;
    }

    return 0;
}

int init_video_codec(Context *context) {
    int ret;

    if ((ret = av_find_best_stream(context->format_context, AVMEDIA_TYPE_VIDEO, -1, -1, &context->video_codec, 0)) < 0) {
        fprintf(stderr, "WARN: cannot find video stream: %s\n", av_err2str(ret));
        return -1;
    }

    context->video_stream = context->format_context->streams[ret];
    context->video_codec_context = avcodec_alloc_context3(context->video_codec);
    if (!context->video_codec_context) {
        fprintf(stderr, "ERROR: cannot allocate video codec context\n");
        return -1;
    }

    if ((ret = avcodec_parameters_to_context(context->video_codec_context, context->video_stream->codecpar)) < 0) {
        fprintf(stderr, "ERROR: cannot copy video codec parameters: %s\n", av_err2str(ret));
        return -1;
    }

    if ((ret = avcodec_open2(context->video_codec_context, context->video_codec, NULL)) < 0) {
        fprintf(stderr, "ERROR: cannot open video codec: %s\n", av_err2str(ret));
        return -1;
    }

    return 0;
}

int init_audio_codec(Context *context) {
    int ret;

    if ((ret = av_find_best_stream(context->format_context, AVMEDIA_TYPE_AUDIO, -1, -1, &context->audio_codec, 0)) < 0) {
        fprintf(stderr, "WARN: cannot find audio stream: %s\n", av_err2str(ret));
        return -1;
    }

    context->audio_stream = context->format_context->streams[ret];
    context->audio_codec_context = avcodec_alloc_context3(context->audio_codec);
    if (!context->audio_codec_context) {
        fprintf(stderr, "ERROR: cannot allocate audio codec context\n");
        return -1;
    }

    if ((ret = avcodec_parameters_to_context(context->audio_codec_context, context->audio_stream->codecpar)) < 0) {
        fprintf(stderr, "ERROR: cannot copy audio codec parameters: %s\n", av_err2str(ret));
        return -1;
    }

    if ((ret = avcodec_open2(context->audio_codec_context, context->audio_codec, NULL)) < 0) {
        fprintf(stderr, "ERROR: cannot open audio codec: %s\n", av_err2str(ret));
        return -1;
    }

    return 0;
}

int init_frames(Context *context) {
    context->packet = av_packet_alloc();
    if (!context->packet) {
        fprintf(stderr, "ERROR: cannot allocate packet\n");
        return -1;
    }

    context->frame = av_frame_alloc();
    if (!context->frame) {
        fprintf(stderr, "ERROR: cannot allocate frame\n");
        return -1;
    }

    context->pcm_frame = av_frame_alloc();
    if (!context->pcm_frame) {
        fprintf(stderr, "ERROR: cannot allocate pcm frame\n");
        return -1;
    }

    context->rgb_frame = av_frame_alloc();
    if (!context->rgb_frame) {
        fprintf(stderr, "ERROR: cannot allocate rgb frame\n");
        return -1;
    }

    return 0;
}

int init_audio_device(Context *context) {
    int ret;

    if ((ret = ma_rb_init(1024 * 1024 * 64, NULL, NULL, &context->rb)) != MA_SUCCESS) {
        fprintf(stderr, "ERROR: cannot initialize miniaudio ring buffer\n");
        return -1;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = 44100;
    config.dataCallback = audio_callback;
    config.pUserData = &context->rb;

    if ((ret = ma_device_init(NULL, &config, &context->audio_device)) != MA_SUCCESS) {
        fprintf(stderr, "ERROR: cannot initialize miniaudio device\n");
        return -1;
    }

    if ((ret = ma_device_start(&context->audio_device)) != MA_SUCCESS) {
        fprintf(stderr, "ERROR: cannot start miniaudio device\n");
        return -1;
    }

    return 0;
}

int init_window(Context *context) {
    context->its = av_gettime_relative();

    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    context->window = glfwCreateWindow(1280, 720, "Window", NULL, NULL);
    if (!context->window) {
        fprintf(stderr, "ERROR: cannot initialize glfw window\n");
        return -1;
    }

    glfwMakeContextCurrent(context->window);
    glClearColor(0, 0, 0, 1);
    return 0;
}

int init_opengl(Context *context) {
    // clang-format off
    GLfloat vertices[] = {
        -1.0, -1.0, +0.0, +1.0,
        -1.0, +1.0, +0.0, +0.0,
        +1.0, -1.0, +1.0, +1.0,
        +1.0, +1.0, +1.0, +0.0,
    };
    GLuint indices[] = {
        0, 1, 2,
        1, 2, 3,
    };
    // clang-format on

    const char *vert_shader_source = "#version 410\n"
                                     "layout(location = 0) in vec2 position;\n"
                                     "layout(location = 1) in vec2 texCoord;\n"
                                     "out vec2 TexCoord;\n"
                                     "void main() {\n"
                                     "    gl_Position = vec4(position, 0.0, 1.0);\n"
                                     "    TexCoord = texCoord;\n"
                                     "}";

    const char *frag_shader_source = "#version 410 core\n"
                                     "in vec2 TexCoord;\n"
                                     "uniform sampler2D Texture;\n"
                                     "out vec4 Color;\n"
                                     "void main() {\n"
                                     "    Color = texture(Texture, TexCoord);\n"
                                     "}";

    GLuint VAO;
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    GLuint VBO;
    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));

    GLuint EBO;
    glGenBuffers(1, &EBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &vert_shader_source, NULL);
    glCompileShader(vertex_shader);

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &frag_shader_source, NULL);
    glCompileShader(fragment_shader);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vertex_shader);
    glAttachShader(prog, fragment_shader);
    glLinkProgram(prog);
    glUseProgram(prog);
    return 0;
}

int video_decode_render(Context *context) {
    if (!context->video_stream || !context->video_codec_context || context->packet->stream_index != context->video_stream->index) {
        return 0;
    }

    int ret;
    if ((ret = avcodec_send_packet(context->video_codec_context, context->packet)) < 0) {
        fprintf(stderr, "ERROR: cannot send packet to video codec. %s\n", av_err2str(ret));
        return -1;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(context->video_codec_context, context->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "ERROR: cannot receive frame from video codec. %s\n", av_err2str(ret));
            return -1;
        }

        int64_t fts = (1e6 * context->frame->pts * context->video_stream->time_base.num) / context->video_stream->time_base.den;
        int64_t rts = av_gettime_relative() - context->its;
        if (fts > rts) av_usleep(fts - rts);

        if ((ret = sws_scale_frame(context->sws_context, context->rgb_frame, context->frame)) < 0) {
            fprintf(stderr, "ERROR: cannot convert frame to rgb frame. %s\n", av_err2str(ret));
            return -1;
        }

        glClear(GL_COLOR_BUFFER_BIT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, context->rgb_frame->width, context->rgb_frame->height, 0, GL_RGB, GL_UNSIGNED_BYTE, context->rgb_frame->data[0]);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        glfwSwapBuffers(context->window);
        glfwPollEvents();
    }

    return 0;
}

int audio_decode_render(Context *context) {
    if (!context->audio_stream || !context->audio_codec_context || context->packet->stream_index != context->audio_stream->index) {
        return 0;
    }

    int ret;
    if ((ret = avcodec_send_packet(context->audio_codec_context, context->packet)) < 0) {
        fprintf(stderr, "ERROR: cannot send packet to audio codec. %s\n", av_err2str(ret));
        return -1;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(context->audio_codec_context, context->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "ERROR: cannot receive frame from audio codec. %s\n", av_err2str(ret));
            return -1;
        }

        int dst_nb_samples = av_rescale_rnd(context->frame->nb_samples, context->pcm_frame->sample_rate, context->audio_codec_context->sample_rate, AV_ROUND_UP);
        context->pcm_frame->nb_samples = dst_nb_samples;

        if ((ret = swr_convert_frame(context->swr_context, context->pcm_frame, context->frame)) < 0) {
            fprintf(stderr, "ERROR: cannot resample frame: %s\n", av_err2str(ret));
            return -1;
        }

        size_t bytes_to_write = context->pcm_frame->nb_samples * 2 * sizeof(float);
        void *ptr;
        if ((ret = ma_rb_acquire_write(&context->rb, &bytes_to_write, &ptr)) != MA_SUCCESS) {
            fprintf(stderr, "ERROR: cannot acquire miniaudio ring buffer for write: %s\n", ma_result_description(ret));
            continue;
        }

        if (bytes_to_write > 0) {
            memcpy(ptr, context->pcm_frame->data[0], bytes_to_write);
            if ((ret = ma_rb_commit_write(&context->rb, bytes_to_write)) != MA_SUCCESS) {
                fprintf(stderr, "ERROR: cannot commit miniaudio ring buffer for write: %s\n", ma_result_description(ret));
            }
        } else {
            ma_rb_commit_write(&context->rb, 0);
        }
    }
    return 0;
}

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "USAGE: %s <url>\n", argv[0]);
        return -1;
    }

    int ret;

    Context *context = (Context *)malloc(sizeof(Context));
    memset(context, 0, sizeof(Context));

    if ((ret = init_window(context)) < 0) {
        fprintf(stderr, "ERROR: cannot initialize window\n");
        return -1;
    }

    if ((ret = init_opengl(context)) < 0) {
        fprintf(stderr, "ERROR: cannot initialize opengl\n");
        return -1;
    }

    if ((ret = init_input(context, argv[1])) < 0) {
        fprintf(stderr, "ERROR: cannot initialize input\n");
        return -1;
    }

    if ((ret = init_frames(context)) < 0) {
        fprintf(stderr, "ERROR: cannot initialize frames\n");
        return -1;
    }

    if ((ret = init_video_codec(context)) < 0) {
        fprintf(stderr, "WARN: cannot initialize video codec\n");
    } else {
        if ((ret = init_video_scaler(context)) < 0) {
            fprintf(stderr, "ERROR: cannot initialize video scaler\n");
            return -1;
        }
    }

    if ((ret = init_audio_codec(context)) < 0) {
        fprintf(stderr, "WARN: cannot initialize audio codec\n");
    } else {
        if ((ret = init_audio_resampler(context)) < 0) {
            fprintf(stderr, "ERROR: cannot initialize audio resampler\n");
            return -1;
        }

        if ((ret = init_audio_device(context)) < 0) {
            fprintf(stderr, "ERROR: cannot initialize audio device\n");
            return -1;
        }

    }

    if (context->video_codec) printf("video codec = %s\n", context->video_codec->name);
    if (context->audio_codec) printf("audio codec = %s\n", context->audio_codec->name);

    while (!glfwWindowShouldClose(context->window)) {
        if ((ret = av_read_frame(context->format_context, context->packet)) < 0) {
            if (ret == AVERROR_EOF) break;
            fprintf(stderr, "ERROR: cannot read packet. %s\n", av_err2str(ret));
            return -1;
        }

        if (ret == AVERROR_EOF) {
            break;
        }

        if (((ret = video_decode_render(context)) < 0) || ((ret = audio_decode_render(context)) < 0)) {
            return -1;
        }

        av_packet_unref(context->packet);
    }

    return 0;
}
