#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>

#include <stdio.h>

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        printf("USAGE: %s <url>\n", argv[0]);
        return 1;
    }

    int ret;

    av_log_set_level(AV_LOG_DEBUG);
    AVFormatContext *format_context = NULL;
    AVDictionary *options = NULL;
    av_dict_set(&options, "protocol_whitelist", "tcp,rtmp", 0);

    if ((ret = avformat_open_input(&format_context, argv[1], NULL, &options)) < 0) {
        fprintf(stderr, "ERROR: cannot open input %s\n", av_err2str(ret));
        return 1;
    }

    if ((ret = avformat_find_stream_info(format_context, NULL)) < 0) {
        fprintf(stderr, "ERROR: cannot read stream info %s\n", av_err2str(ret));
        return 1;
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet) return 1;

    AVFrame *frame0 = av_frame_alloc();
    if (!frame0) return 1;

    AVFrame *frame1 = av_frame_alloc();
    if (!frame0) return 1;

    // Video
    const AVCodec *video_codec = NULL;
    if ((ret = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, &video_codec, 0)) < 0) {
        fprintf(stderr, "ERROR: cannot find video stream. %s\n", av_err2str(ret));
        return 1;
    }

    AVStream *video_stream = format_context->streams[ret];
    AVCodecContext *video_codec_context = avcodec_alloc_context3(video_codec);
    if ((ret = avcodec_parameters_to_context(video_codec_context, video_stream->codecpar)) < 0) {
        fprintf(stderr, "ERROR: cannot copy stream codec parameters to codec context %s\n", av_err2str(ret));
        return 1;
    }

    if ((ret = avcodec_open2(video_codec_context, video_codec, NULL)) < 0) {
        fprintf(stderr, "ERROR: cannot open codec %s\n", av_err2str(ret));
        return 1;
    }

    struct SwsContext *sws_ctx = sws_getContext(video_codec_context->width, video_codec_context->height, video_codec_context->pix_fmt, 1280, 720, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        fprintf(stderr, "ERROR: cannot create software scaling context\n");
    }

    // Audio
    const AVCodec *audio_codec = NULL;
    if ((ret = av_find_best_stream(format_context, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_codec, 0)) < 0) {
        fprintf(stderr, "ERROR: cannot find audio stream. %s\n", av_err2str(ret));
        return 1;
    }

    AVStream *audio_stream = format_context->streams[ret];
    AVCodecContext *audio_codec_context = avcodec_alloc_context3(audio_codec);
    if ((ret = avcodec_parameters_to_context(audio_codec_context, audio_stream->codecpar)) < 0) {
        fprintf(stderr, "ERROR: cannot copy stream codec parameters to codec context %s\n", av_err2str(ret));
        return 1;
    }

    if ((ret = avcodec_open2(audio_codec_context, audio_codec, NULL)) < 0) {
        fprintf(stderr, "ERROR: cannot open codec %s\n", av_err2str(ret));
        return 1;
    }

    int64_t its = av_gettime_relative();

    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    GLFWwindow *window = glfwCreateWindow(1280, 720, "WINDOW", NULL, NULL);
    glfwSetWindowSizeLimits(window, 480, 270, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwSetWindowAspectRatio(window, 1280, 720);
    glfwMakeContextCurrent(window);
    glClearColor(0, 0, 0, 1);


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

    static const char *vertex_shader_source = "#version 410\n"
                                              "layout(location = 0) in vec2 position;\n"
                                              "layout(location = 1) in vec2 texCoord;\n"
                                              "\n"
                                              "out vec2 TexCoord;\n"
                                              "\n"
                                              "void main() {\n"
                                              "    gl_Position = vec4(position, 0.0, 1.0);\n"
                                              "    TexCoord = texCoord;\n"
                                              "}";

    static const char *fragment_shader_source = "#version 410 core\n"
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
    glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
    glCompileShader(vertex_shader);

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &fragment_shader_source, NULL);
    glCompileShader(fragment_shader);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vertex_shader);
    glAttachShader(prog, fragment_shader);
    glLinkProgram(prog);
    glUseProgram(prog);


    while (!glfwWindowShouldClose(window)) {
        if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
            break;
        }

        ret = av_read_frame(format_context, packet);
        if (ret == AVERROR_EOF) {
            break;
        }

        if (ret == AVERROR(EAGAIN)) {
            av_packet_unref(packet);
            continue;
        }

        if (video_codec_context && packet->stream_index == video_stream->index && avcodec_send_packet(video_codec_context, packet) == 0) {
            while (avcodec_receive_frame(video_codec_context, frame0) == 0) {
                if ((ret = sws_scale_frame(sws_ctx, frame1, frame0)) < 0) {
                    fprintf(stderr, "ERROR: sws_scale_frame %s\n", av_err2str(ret));
                    return 1;
                }

                int64_t fts = (1e6 * frame0->pts * video_stream->time_base.num) / video_stream->time_base.den;
                int64_t rts = av_gettime_relative() - its;
                if (fts > rts) {
                    av_usleep(fts - rts);
                }

                glClear(GL_COLOR_BUFFER_BIT);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame1->width, frame1->height, 0, GL_RGB, GL_UNSIGNED_BYTE, frame1->data[0]);
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
                glfwSwapBuffers(window);
                glfwPollEvents();

                printf("Video Frame %lld %s...\n", video_codec_context->frame_num, video_codec->name);
            }
        }

        if (audio_codec_context && packet->stream_index == audio_stream->index && avcodec_send_packet(audio_codec_context, packet) == 0) {
            while (avcodec_receive_frame(audio_codec_context, frame0) == 0) {
                int64_t fts = 1000 * 1000 * frame0->pts * audio_stream->time_base.num / audio_stream->time_base.den;
                int64_t rts = av_gettime_relative() - its;
                if (fts > rts) {
                    av_usleep(fts - rts);
                }

                printf("Audio Frame %lld %s...\n", audio_codec_context->frame_num, audio_codec->name);
            }
        }

        av_packet_unref(packet);
    }

    return 0;
}
