#include <stdio.h>

#include <GLFW/glfw3.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
#include <libavutil/threadmessage.h>
#include <libavutil/time.h>

#include <pthread.h>

static AVFormatContext *format_context = NULL;
static const AVCodec *video_codec = NULL;
static AVCodecContext *video_codec_context = NULL;
static AVStream *video_stream = NULL;

static const AVCodec *audio_codec = NULL;
static AVCodecContext *audio_codec_context = NULL;
static AVStream *audio_stream = NULL;

static SwsContext *sws_context = NULL;

static AVThreadMessageQueue *video_frame_queue = NULL;

void *demux_thread_proc(void *arg) {
    int ret = 0;

    //av_log_set_level(AV_LOG_TRACE);
    const char *url = (const char *)arg;
    if ((ret = avformat_open_input(&format_context, url, NULL, NULL)) < 0) {
        return NULL;
    }

    if ((ret = avformat_find_stream_info(format_context, NULL)) < 0) {
        fprintf(stderr, "ERROR: cannot find stream information. %s\n", av_err2str(ret));
        return NULL;
    }

    int video_stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, &video_codec, 0);
    if (video_stream_index >= 0) {
        video_stream = format_context->streams[video_stream_index];
        video_codec_context = avcodec_alloc_context3(video_codec);
        if ((ret = avcodec_parameters_to_context(video_codec_context, video_stream->codecpar)) < 0) {
            fprintf(stderr, "ERROR: cannot copy video codec parameters to context. %s\n", av_err2str(ret));
            return NULL;
        }

        if ((ret = avcodec_open2(video_codec_context, video_codec, NULL)) < 0) {
            fprintf(stderr, "ERROR: cannot open video codec. %s\n", av_err2str(ret));
            return NULL;
        }

        sws_context = sws_getContext(video_codec_context->width, video_codec_context->height, video_codec_context->pix_fmt,
                video_codec_context->width, video_codec_context->height, AV_PIX_FMT_RGB24,
                SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws_context) {
            fprintf(stderr, "ERROR: cannot initialiez sws context\n");
            return NULL;
        }
    }

    int audio_stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_codec, 0);
    if (audio_stream_index >= 0) {
        audio_stream = format_context->streams[audio_stream_index];
        audio_codec_context = avcodec_alloc_context3(audio_codec);
        if ((ret = avcodec_parameters_to_context(audio_codec_context, audio_stream->codecpar)) < 0) {
            fprintf(stderr, "ERROR: cannot copy audio codec parameters to context. %s\n", av_err2str(ret));
            return NULL;
        }

        if ((ret = avcodec_open2(audio_codec_context, audio_codec, NULL)) < 0) {
            fprintf(stderr, "ERROR: cannot open audio codec. %s\n", av_err2str(ret));
            return NULL;
        }
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    for (;;) {
        ret = av_read_frame(format_context, packet);
        if (ret == AVERROR(EAGAIN)) {
            continue;
        } else if (ret == AVERROR_EOF || ret < 0) {
            break;
        }

        if (video_codec_context && video_stream && packet->stream_index == video_stream->index) {
            ret = avcodec_send_packet(video_codec_context, packet);
            while (ret >= 0) {
                ret = avcodec_receive_frame(video_codec_context, frame);
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
                    break;
                } else if (ret < 0) {
                    fprintf(stderr, "ERROR: cannot receive video frame from codec. %s\n", av_err2str(ret));
                    return NULL;
                }

                frame->pts = frame->best_effort_timestamp;

                AVFrame *copy = av_frame_alloc();
                copy->width = frame->width;
                copy->height = frame->height;
                copy->format = AV_PIX_FMT_RGB24;
                copy->pts = frame->pts;
                //if ((ret = av_frame_get_buffer(copy, 32)) < 0) {
                //    fprintf(stderr, "ERROR: cannot allocate frame buffers. %s\n", av_err2str(ret));
                //    return NULL;
                //}

                if ((ret = sws_scale_frame(sws_context, copy, frame)) < 0) {
                    fprintf(stderr, "ERROR: cannot scale frame. %s\n", av_err2str(ret));
                    return NULL;
                }

                av_thread_message_queue_send(video_frame_queue, &copy, AV_THREAD_MESSAGE_NONBLOCK);
            }
        }

        //if (audio_codec_context && audio_stream && packet->stream_index == audio_stream->index) {
        //    ret = avcodec_send_packet(audio_codec_context, packet);
        //    while (ret >= 0) {
        //        ret = avcodec_receive_frame(audio_codec_context, frame);
        //        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
        //            break;
        //        } else if (ret < 0) {
        //            fprintf(stderr, "ERROR: cannot receive audio frame from codec. %s\n", av_err2str(ret));
        //            return NULL;
        //        }
        //    }
        //}

        av_packet_unref(packet);
    }

    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "USAGE: %s <url>\n", argv[0]);
        return -1;
    }

    int ret = 0;
    if ((ret = av_thread_message_queue_alloc(&video_frame_queue, 10000, sizeof(AVFrame*))) < 0) {
        fprintf(stderr, "ERROR: cannot allocate video frame queue. %s\n", av_err2str(ret));
        return -1;
    }

    pthread_t demux_thread;
    pthread_create(&demux_thread, NULL, demux_thread_proc, argv[1]);
    pthread_detach(demux_thread);

    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow *window = glfwCreateWindow(1280, 720, "WINDOW", NULL, NULL);
    glfwMakeContextCurrent(window);

    GLuint indices[] = {0, 1, 2, 1, 2, 3};
    GLfloat vertices[] = {
        -1.0, -1.0, +0.0, +1.0,
        -1.0, +1.0, +0.0, +0.0,
        +1.0, -1.0, +1.0, +1.0,
        +1.0, +1.0, +1.0, +0.0,
    };

    const char *vertSource = R"(
        #version 330
        layout(location = 0) in vec2 position;
        layout(location = 1) in vec2 texCoord;

        out vec2 TexCoord;

        void main() {
            gl_Position = vec4(position, 0.0, 1.0);
            TexCoord = texCoord;
        }
    )";

    const char *fragSource = R"(
        #version 330 core
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

    glfwSwapInterval(0);

    int64_t i = av_gettime_relative();
    while (!glfwWindowShouldClose(window)) {
        AVFrame *frame = NULL;
        if (av_thread_message_queue_recv(video_frame_queue, &frame, AV_THREAD_MESSAGE_NONBLOCK) == 0) {
            printf("%lld\n", frame->pts);
            glClear(GL_COLOR_BUFFER_BIT);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame->width, frame->height, 0, GL_RGB, GL_UNSIGNED_BYTE, frame->data[0]);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
            av_frame_free(&frame);
            glfwPollEvents();
            glfwSwapBuffers(window);
        }
    }
    return 0;
}
