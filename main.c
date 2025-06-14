#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>

#include <stdio.h>

GLuint init_opengl() {
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
                                              "}\n"
                                              "\n";

    static const char *fragment_shader_source = "#version 410 core\n"
                                                "in vec2 TexCoord;\n"
                                                "\n"
                                                "uniform sampler2D Texture;\n"
                                                "uniform int Filter;\n"
                                                "\n"
                                                "out vec4 FragColor;\n"
                                                "\n"
                                                "void main() {\n"
                                                "    vec4 color = texture(Texture, TexCoord);\n"
                                                "\n"
                                                "    switch (Filter) {\n"
                                                "        case 1: // Sepia\n"
                                                "            color.rgb = vec3(\n"
                                                "                clamp(color.r * 0.393 + color.g * 0.769 + color.b * 0.189, 0.0, 1.0),\n"
                                                "                clamp(color.r * 0.349 + color.g * 0.686 + color.b * 0.168, 0.0, 1.0),\n"
                                                "                clamp(color.r * 0.272 + color.g * 0.534 + color.b * 0.131, 0.0, 1.0)\n"
                                                "            );\n"
                                                "            break;\n"
                                                "        case 2: // Grayscale\n"
                                                "            float gray = dot(color.rgb, vec3(0.299, 0.587, 0.114));\n"
                                                "            color.rgb = vec3(gray, gray, gray);\n"
                                                "            break;\n"
                                                "        case 3: // Invert\n"
                                                "            color.rgb = vec3(1.0) - color.rgb;\n"
                                                "            break;\n"
                                                "        case 4: // Adjust Brightness\n"
                                                "            color.rgb = clamp(color.rgb * 1.2, 0.0, 1.0);\n"
                                                "            break;\n"
                                                "        case 5: // Adjust Saturation\n"
                                                "            float luminance = dot(color.rgb, vec3(0.299, 0.587, 0.114));\n"
                                                "            color.rgb = mix(vec3(luminance), color.rgb, 1.5);\n"
                                                "            break;\n"
                                                "    }\n"
                                                "\n"
                                                "    FragColor = color;\n"
                                                "}\n";

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

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
    return prog;
}

GLFWwindow *init_window(void) {
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    GLFWwindow *window = glfwCreateWindow(1280, 720, "WINDOW", NULL, NULL);
    glfwSetWindowSizeLimits(window, 480, 270, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwSetWindowAspectRatio(window, 1280, 720);
    glfwMakeContextCurrent(window);
    return window;
}

static int ret;

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        printf("USAGE: %s <url>\n", argv[0]);
        return 1;
    }

    av_log_set_level(AV_LOG_DEBUG);
    AVFormatContext *format_context = NULL;
    if ((ret = avformat_open_input(&format_context, argv[1], NULL, NULL)) < 0) {
        fprintf(stderr, "ERROR: cannot open input %s\n", av_err2str(ret));
        return 1;
    }

    if ((ret = avformat_find_stream_info(format_context, NULL)) < 0) {
        fprintf(stderr, "ERROR: cannot read stream info %s\n", av_err2str(ret));
        return 1;
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet) return 1;

    AVFrame *frame = av_frame_alloc();
    if (!frame) return 1;

    AVFrame *sw_vframe = av_frame_alloc();
    if (!frame) return 1;

    const AVCodec *vc = NULL;
    int vci = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, &vc, 0);
    if (vci < 0) {
        fprintf(stderr, "ERROR: cannot find video stream %s\n", av_err2str(ret));
        return 1;
    }

    AVCodecContext *vcodec_ctx = avcodec_alloc_context3(vc);
    if ((ret = avcodec_parameters_to_context(vcodec_ctx, format_context->streams[vci]->codecpar)) < 0) {
        fprintf(stderr, "ERROR: cannot copy stream codec parameters to codec context %s\n", av_err2str(ret));
        return 1;
    }

    if ((ret = avcodec_open2(vcodec_ctx, vc, NULL)) < 0) {
        fprintf(stderr, "ERROR: cannot open codec %s\n", av_err2str(ret));
        return 1;
    }

    struct SwsContext *sws_ctx = sws_getContext(vcodec_ctx->width, vcodec_ctx->height, vcodec_ctx->pix_fmt, 1280, 720, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        fprintf(stderr, "ERROR: cannot create software scaling context\n");
    }

    int64_t its = av_gettime_relative();
    GLFWwindow *window = init_window();
    GLuint prog = init_opengl();

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

        for (int key = GLFW_KEY_0; key < GLFW_KEY_9; key++) {
            if (glfwGetKey(window, key) == GLFW_PRESS) {
                glUniform1i(glGetUniformLocation(prog, "Filter"), key - GLFW_KEY_0);
            }
        }

        if (vcodec_ctx && packet->stream_index == vci && avcodec_send_packet(vcodec_ctx, packet) == 0) {
            while (avcodec_receive_frame(vcodec_ctx, frame) == 0) {
                if ((ret = sws_scale_frame(sws_ctx, sw_vframe, frame)) < 0) {
                    fprintf(stderr, "ERROR: sws_scale_frame %s\n", av_err2str(ret));
                    return 1;
                }

                AVStream *vstream = format_context->streams[vci];
                int64_t fts = (1e6 * frame->pts * vstream->time_base.num) / vstream->time_base.den;
                int64_t rts = av_gettime_relative() - its;
                if (fts > rts) {
                    av_usleep(fts - rts);
                }

                glClear(GL_COLOR_BUFFER_BIT);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, sw_vframe->width, sw_vframe->height, 0, GL_RGB, GL_UNSIGNED_BYTE, sw_vframe->data[0]);
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
                glfwSwapBuffers(window);
                glfwPollEvents();
            }
        }

        av_packet_unref(packet);
    }

    return 0;
}
