#include <stdbool.h>
#include <string.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <GLFW/glfw3.h>
#include <miniaudio.h>
}

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "USAGE: %s <url>\n", argv[0]);
        return -1;
    }

    AVFormatContext *format_context = NULL;
    const AVCodec *video_codec = NULL;
    const AVCodec *audio_codec = NULL;
    AVCodecContext *video_codec_context = NULL;
    AVCodecContext *audio_codec_context = NULL;
    AVStream *video_stream = NULL;
    AVStream *audio_stream = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    AVFrame *pcm_frame = NULL;
    AVFrame *rgb_frame = NULL;
    struct SwrContext *swr_context = NULL;
    struct SwsContext *sws_context = NULL;
    ma_device audio_device;
    ma_rb ring_buffer;

    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow *window = glfwCreateWindow(1280, 720, "WINDOW", NULL, NULL);
    glfwMakeContextCurrent(window);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    GLuint indices[] = {0, 1, 2, 1, 2, 3};
    GLfloat vertices[] = {
        -1.0, -1.0, +0.0, +1.0,
        -1.0, +1.0, +0.0, +0.0,
        +1.0, -1.0, +1.0, +1.0,
        +1.0, +1.0, +1.0, +0.0,
    };

    const char *vertSource = R"(
        #version 410
        layout(location = 0) in vec2 position;
        layout(location = 1) in vec2 texCoord;

        out vec2 TexCoord;

        void main() {
            gl_Position = vec4(position, 0.0, 1.0);
            TexCoord = texCoord;
        }
    )";

    const char *fragSource = R"(
        #version 410 core
        in vec2 TexCoord;
        uniform sampler2D Texture;
        out vec4 Color;
        void main() {
            Color = texture(Texture, TexCoord);
        }
    )";

    GLuint VAO, VBO, EBO;
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)0);

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));

    glGenBuffers(1, &EBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    GLuint vertShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertShader, 1, &vertSource, NULL);
    glCompileShader(vertShader);

    GLuint fragShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragShader, 1, &fragSource, NULL);
    glCompileShader(fragShader);

    GLuint program = glCreateProgram();
    glAttachShader(program, vertShader);
    glAttachShader(program, fragShader);
    glLinkProgram(program);
    glUseProgram(program);

    av_log_set_level(AV_LOG_INFO);
    if (avformat_open_input(&format_context, argv[1], NULL, NULL) < 0) {
        fprintf(stderr, "ERROR: cannot open input.\n");
        return -1;
    }

    if (avformat_find_stream_info(format_context, NULL) < 0) {
        fprintf(stderr, "ERROR: cannot find stream information.\n");
        return -1;
    }

    int video_stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, &video_codec, 0);
    if (video_stream_index >= 0) {
        video_stream = format_context->streams[video_stream_index];
        printf("%s\n", avcodec_get_name(video_stream->codecpar->codec_id));
        video_codec_context = avcodec_alloc_context3(video_codec);
        if (!video_codec_context) {
            fprintf(stderr, "ERROR: cannot allocate video codec context\n");
            return -1;
        }

        if (avcodec_parameters_to_context(video_codec_context, video_stream->codecpar) < 0) {
            fprintf(stderr, "ERROR: cannot copy video codec parameters to context.\n");
            return -1;
        }

        if (avcodec_open2(video_codec_context, video_codec, NULL) < 0) {
            fprintf(stderr, "ERROR: cannot open video codec.\n");
            return -1;
        }

        sws_context = sws_getContext(video_codec_context->width, video_codec_context->height, video_codec_context->pix_fmt, video_codec_context->width, video_codec_context->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws_context) {
            fprintf(stderr, "ERROR: cannot allocate sws context\n");
            return -1;
        }
    }

    int audio_stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_codec, 0);
    if (audio_stream_index >= 0) {
        audio_stream = format_context->streams[audio_stream_index];
        audio_codec_context = avcodec_alloc_context3(audio_codec);
        if (!audio_codec_context) {
            fprintf(stderr, "ERROR: cannot allocate audio codec context\n");
            return -1;
        }

        if (avcodec_parameters_to_context(audio_codec_context, audio_stream->codecpar) < 0) {
            fprintf(stderr, "ERROR: cannot copy audio codec parameters to context.\n");
            return -1;
        }

        if (avcodec_open2(audio_codec_context, audio_codec, NULL) < 0) {
            fprintf(stderr, "ERROR: cannot open audio codec.\n");
            return -1;
        }

        const AVChannelLayout dst_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        enum AVSampleFormat dst_sample_format = AV_SAMPLE_FMT_FLT;
        int dst_sample_rate = 44100;
        if (swr_alloc_set_opts2(&swr_context, &dst_ch_layout, dst_sample_format, dst_sample_rate, &audio_codec_context->ch_layout, audio_codec_context->sample_fmt, audio_codec_context->sample_rate, 0, NULL) < 0) {
            fprintf(stderr, "ERROR: cannot create a swr context.\n");
            return -1;
        }

        if (swr_init(swr_context) < 0) {
            fprintf(stderr, "ERROR: cannot initialize swr context.\n");
            return -1;
        }

        if (ma_rb_init(1024 * 1024 * 64, NULL, NULL, &ring_buffer) != MA_SUCCESS) {
            return -1;
        }

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 2;
        config.sampleRate = 44100;
        config.dataCallback = [](ma_device *device, void *output, const void *input, ma_uint32 number_of_frames) {
            ma_rb *rb = (ma_rb *)device->pUserData;
            size_t bytes_to_read = number_of_frames * 2 * sizeof(float);
            void *ptr;
            if (ma_rb_acquire_read(rb, &bytes_to_read, &ptr) != MA_SUCCESS) {
                memset(output, 0, number_of_frames * 2 * sizeof(float));
                return;
            }
            if (bytes_to_read > 0) {
                memcpy(output, ptr, bytes_to_read);
                if (bytes_to_read < number_of_frames * 2 * sizeof(float)) {
                    memset((char *)output + bytes_to_read, 0, (number_of_frames * 2 * sizeof(float)) - bytes_to_read);
                }
                ma_rb_commit_read(rb, bytes_to_read);
            } else {
                memset(output, 0, number_of_frames * 2 * sizeof(float));
            }
        };
        config.pUserData = &ring_buffer;

        if (ma_device_init(NULL, &config, &audio_device) != MA_SUCCESS) {
            return -1;
        }

        if (ma_device_start(&audio_device) != MA_SUCCESS) {
            return -1;
        }
    }

    packet = av_packet_alloc();
    if (!packet) {
        return -1;
    }

    frame = av_frame_alloc();
    if (!frame) {
        return -1;
    }

    pcm_frame = av_frame_alloc();
    if (!pcm_frame) {
        return -1;
    }

    rgb_frame = av_frame_alloc();
    if (!rgb_frame) {
        return -1;
    }

    pcm_frame->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    pcm_frame->sample_rate = 44100;
    pcm_frame->format = AV_SAMPLE_FMT_FLT;

    if (video_codec) {
        printf("video codec = %s\n", video_codec->name);
    }

    if (audio_codec) {
        printf("audio codec = %s\n", audio_codec->name);
    }

    while (!glfwWindowShouldClose(window)) {
        if (av_read_frame(format_context, packet) < 0) break;

        if (video_stream && packet->stream_index == video_stream->index) {
            if (avcodec_send_packet(video_codec_context, packet) == 0) {
                while (avcodec_receive_frame(video_codec_context, frame) == 0) {
                    if (sws_scale_frame(sws_context, rgb_frame, frame) < 0) {
                        fprintf(stderr, "ERROR: cannot convert frame to rgb frame.\n");
                        return -1;
                    }

                    glClear(GL_COLOR_BUFFER_BIT);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, rgb_frame->width, rgb_frame->height, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb_frame->data[0]);
                    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
                    glfwSwapBuffers(window);
                    glfwPollEvents();
                }
            }
        }

        if (audio_stream && packet->stream_index == audio_stream->index) {
            if (avcodec_send_packet(audio_codec_context, packet) == 0) {
                while (avcodec_receive_frame(audio_codec_context, frame) == 0) {
                    int dst_nb_samples = av_rescale_rnd(frame->nb_samples, pcm_frame->sample_rate, audio_codec_context->sample_rate, AV_ROUND_UP);
                    pcm_frame->nb_samples = dst_nb_samples;
                    if (swr_convert_frame(swr_context, pcm_frame, frame) < 0) {
                        fprintf(stderr, "ERROR: cannot convert frame to pcm frame.\n");
                        return -1;
                    }
                    size_t bytes_to_write = pcm_frame->nb_samples * 2 * sizeof(float);
                    void *ptr;
                    if (ma_rb_acquire_write(&ring_buffer, &bytes_to_write, &ptr) == MA_SUCCESS && bytes_to_write > 0) {
                        memcpy(ptr, pcm_frame->data[0], bytes_to_write);
                        ma_rb_commit_write(&ring_buffer, bytes_to_write);
                    } else {
                        ma_rb_commit_write(&ring_buffer, 0);
                    }
                }
            }
        }

        av_packet_unref(packet);
    }

    return 0;
}
