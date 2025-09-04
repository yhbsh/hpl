extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/threadmessage.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <GLFW/glfw3.h>
}

#include <stdio.h>
#include <thread>
#include <unistd.h>

struct Context {
    const char *url;
    bool running;
    AVThreadMessageQueue *video_queue;
    AVThreadMessageQueue *audio_queue;
    AVStream *videoStream;
    AVStream *audioStream;
    int video_queue_size = 100;
    int audio_queue_size = 100;

    Context(const char *url) : url(url), running(false) {
        if (int ret = av_thread_message_queue_alloc(&video_queue, video_queue_size, sizeof(AVFrame)) < 0) {
            fprintf(stderr, "ERROR: cannot allocate thread message queue. %s\n", av_err2str(ret));
            exit(1);
        }

        if (int ret = av_thread_message_queue_alloc(&audio_queue, audio_queue_size, sizeof(AVFrame)) < 0) {
            fprintf(stderr, "ERROR: cannot allocate thread message queue. %s\n", av_err2str(ret));
            exit(1);
        }
    }

    ~Context() {
        av_thread_message_queue_free(&video_queue);
        av_thread_message_queue_free(&audio_queue);
        running = false;
        video_queue = NULL;
    }
};

void run(Context *context) {
    av_log_set_level(AV_LOG_TRACE);
    
    const char *url = context->url;
    AVFormatContext *formatContext = NULL;
    if (int ret = avformat_open_input(&formatContext, url, NULL, NULL) < 0) {
        fprintf(stderr, "ERROR: cannot open input. %s\n", av_err2str(ret));
        return;
    }

    if (int ret = avformat_find_stream_info(formatContext, NULL) < 0) {
        fprintf(stderr, "ERROR: cannot find stream information. %s\n", av_err2str(ret));
        return;
    }

    const AVCodec *videoCodec = NULL;
    AVCodecContext *videoCodecContext = NULL;
    SwsContext *swsContext = NULL;

    const AVCodec *audioCodec = NULL;
    AVCodecContext *audioCodecContext = NULL;

    for (size_t i = 0; i < formatContext->nb_streams; i++) {
        AVStream *stream = formatContext->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoCodec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (!videoCodec) {
                fprintf(stderr, "ERROR: cannot find video codec %s\n", avcodec_get_name(stream->codecpar->codec_id));
                return;
            }

            videoCodecContext = avcodec_alloc_context3(videoCodec);
            if (!videoCodecContext) {
                fprintf(stderr, "ERROR: cannot allocate video codec context\n");
                return;
            }

            if (int ret = avcodec_parameters_to_context(videoCodecContext, stream->codecpar) < 0) {
                fprintf(stderr, "ERROR: cannot copy codec parameters to context. %s\n", av_err2str(ret));
                return;
            }

            if (int ret = avcodec_open2(videoCodecContext, videoCodec, NULL) < 0) {
                fprintf(stderr, "ERROR: cannot open video context. %s\n", av_err2str(ret));
                return;
            }

            swsContext = sws_getContext(videoCodecContext->width, videoCodecContext->height, videoCodecContext->pix_fmt, videoCodecContext->width, videoCodecContext->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
            if (!swsContext) {
                fprintf(stderr, "ERROR: cannot allocate sws context\n");
                return;
            }

            context->videoStream = stream;
        }

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioCodec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (!audioCodec) {
                fprintf(stderr, "ERROR: cannot find audio codec %s\n", avcodec_get_name(stream->codecpar->codec_id));
                return;
            }

            audioCodecContext = avcodec_alloc_context3(audioCodec);
            if (!audioCodecContext) {
                fprintf(stderr, "ERROR: cannot allocate audio codec context\n");
                return;
            }

            if (int ret = avcodec_parameters_to_context(audioCodecContext, stream->codecpar) < 0) {
                fprintf(stderr, "ERROR: cannot copy codec parameters to context. %s\n", av_err2str(ret));
                return;
            }

            if (int ret = avcodec_open2(audioCodecContext, audioCodec, NULL) < 0) {
                fprintf(stderr, "ERROR: cannot open audio context. %s\n", av_err2str(ret));
                return;
            }

            context->audioStream = stream;
        }
    }

    if (videoCodec) {
        printf("videoCodec = %s\n", videoCodec->long_name);
    }

    if (audioCodec) {
        printf("audioCodec = %s\n", audioCodec->long_name);
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *rgb_frame = av_frame_alloc();
    while (true) {
        int ret = av_read_frame(formatContext, packet);
        if (ret == AVERROR_EOF) break;

        if (context->videoStream && packet->stream_index == context->videoStream->index) {
            if (avcodec_send_packet(videoCodecContext, packet) == 0) {
                while (avcodec_receive_frame(videoCodecContext, frame) == 0) {
                    if (sws_scale_frame(swsContext, rgb_frame, frame) < 0) {
                        fprintf(stderr, "ERROR: cannot convert frame to rgb frame.\n");
                        return;
                    }

                    rgb_frame->pts = frame->pts;

                    AVFrame *copy = av_frame_alloc();
                    av_frame_ref(copy, rgb_frame);
                    if (int ret = av_thread_message_queue_send(context->video_queue, copy, 0) < 0) {
                        fprintf(stderr, "ERROR: cannot queue video frame. %s\n", av_err2str(ret));
                        return;
                    }
                }
            }
        }

        if (context->audioStream && packet->stream_index == context->audioStream->index) {
            if (avcodec_send_packet(audioCodecContext, packet) == 0) {
                while (avcodec_receive_frame(audioCodecContext, frame) == 0) {
                    AVFrame *copy = av_frame_alloc();
                    av_frame_ref(copy, frame);
                    if (int ret = av_thread_message_queue_send(context->audio_queue, copy, 0) < 0) {
                        fprintf(stderr, "ERROR: cannot queue audio frame. %s\n", av_err2str(ret));
                        return;
                    }
                }
            }
        }

        av_packet_unref(packet);
    }

    avformat_close_input(&formatContext);
    avcodec_free_context(&videoCodecContext);
    avcodec_free_context(&audioCodecContext);
    av_packet_free(&packet);
    av_frame_free(&frame);
}

void run_audio(Context *context) {
    AVFrame *frame = av_frame_alloc();
    while (true) {
        if (av_thread_message_queue_nb_elems(context->audio_queue) > 0) {
            if (int ret = av_thread_message_queue_recv(context->audio_queue, frame, 0) < 0) {
                fprintf(stderr, "ERROR: cannot dequeue audio frame. %s\n", av_err2str(ret));
                exit(1);
            }

            //printf("audio frame = %d\n", frame->sample_rate);
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "USAGE: %s <url>\n", argv[0]);
        return -1;
    }
    Context *context = new Context(argv[1]);

    std::thread tid(run, context);
    std::thread audio_tid(run_audio, context);

    tid.detach();
    audio_tid.detach();

    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window = glfwCreateWindow(1280, 720, "WINDOW", NULL, NULL);
    if (!window) {
        fprintf(stderr, "ERROR: cannot initialize glfw window\n");
        return -1;
    }

    glfwMakeContextCurrent(window);

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

    int64_t begin = av_gettime();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        int nb_elems = av_thread_message_queue_nb_elems(context->video_queue);
        if (nb_elems <= 0) {
            continue;
        }

        AVFrame frame;
        if (int ret = av_thread_message_queue_recv(context->video_queue, &frame, 0) < 0) {
            fprintf(stderr, "ERROR: cannot dequeue video frame. %s\n", av_err2str(ret));
            exit(1);
        }

        glClear(GL_COLOR_BUFFER_BIT);

        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.linesize[0] / 3); // 3 for RGB24
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame.width, frame.height, 0, GL_RGB, GL_UNSIGNED_BYTE, frame.data[0]);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); // reset

        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

        glfwSwapBuffers(window);

        //printf("video frame = %dx%d %s\n", frame.width, frame.height, av_get_pix_fmt_name((enum AVPixelFormat)frame.format));
    }

    return 0;
}

