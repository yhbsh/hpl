extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#define GLFW_INCLUDE_GLCOREARB
#include <glfw.h>

#include <stdio.h>

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

static const char *vertex_shader_source = R"(#version 410
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texCoord;

out vec2 TexCoord;

void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    TexCoord = texCoord;
}
)";

static const char *fragment_shader_source = R"(#version 410 core
in vec2 TexCoord;

uniform sampler2D TextureY;
uniform sampler2D TextureU;
uniform sampler2D TextureV;
uniform int Filter;

out vec4 FragColor;


vec3 sepia(vec3 color) {
    return vec3(
        clamp(color.r * 0.393 + color.g * 0.769 + color.b * 0.189, 0.0, 1.0),
        clamp(color.r * 0.349 + color.g * 0.686 + color.b * 0.168, 0.0, 1.0),
        clamp(color.r * 0.272 + color.g * 0.534 + color.b * 0.131, 0.0, 1.0)
    );
}

vec3 grayscale(vec3 color) {
    float gray = dot(color, vec3(0.299, 0.587, 0.114));
    return vec3(gray, gray, gray);
}

vec3 invert(vec3 color) {
    return vec3(1.0) - color;
}

vec3 adjust_brightness(vec3 color, float brightness) {
    return clamp(color * brightness, 0.0, 1.0);
}

vec3 adjust_saturation(vec3 color, float saturation) {
    float gray = dot(color, vec3(0.299, 0.587, 0.114));
    return mix(vec3(gray), color, saturation);
}


void main() {
    float y = texture(TextureY, TexCoord).r;
    float u = texture(TextureU, TexCoord).r - 0.5;
    float v = texture(TextureV, TexCoord).r - 0.5;

    y = 1.1643 * (y - 0.0625);

    float r = y + 1.403 * v;
    float g = y - 0.344 * u - 0.714 * v;
    float b = y + 1.770 * u;
    vec3 color = vec3(clamp(r, 0.0, 1.0), clamp(g, 0.0, 1.0), clamp(b, 0.0, 1.0));

    if (Filter == 1) {
        color = sepia(color);
    } else if (Filter == 2) {
        color = grayscale(color);
    } else if (Filter == 3) {
        color = invert(color);
    } else if (Filter == 4) {
        color = adjust_brightness(color, 1.2);
    } else if (Filter == 5) {
        color = adjust_saturation(color, 1.5);
    }

    FragColor = vec4(color, 1.0);
})";

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        printf("USAGE: %s <url>\n", argv[0]);
        return 1;
    }

    glfwInit();

#define WINDOWS_COUNT 16
    int columns       = 4;
    int window_width  = 480;
    int window_height = 270;

    GLFWwindow *windows[WINDOWS_COUNT];
    GLuint VAO[WINDOWS_COUNT];
    GLuint VBO[WINDOWS_COUNT];
    GLuint EBO[WINDOWS_COUNT];
    GLuint textures[WINDOWS_COUNT][3];

    for (int i = 0; i < WINDOWS_COUNT; i++) {
        int row   = i / columns;
        int col   = i % columns;
        int pos_x = col * (window_width);
        int pos_y = row * (window_height);

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        windows[i] = glfwCreateWindow(window_width, window_height, "Window", NULL, NULL);
        glfwSetWindowSizeLimits(windows[i], window_width, window_height, GLFW_DONT_CARE, GLFW_DONT_CARE);
        glfwSetWindowAspectRatio(windows[i], 16, 9);

        // Set the position of the window
        glfwSetWindowPos(windows[i], pos_x, pos_y);

        glfwMakeContextCurrent(windows[i]);

        // Initialize OpenGL buffers and shaders
        glGenVertexArrays(1, &VAO[i]);
        glBindVertexArray(VAO[i]);

        glGenBuffers(1, &VBO[i]);
        glBindBuffer(GL_ARRAY_BUFFER, VBO[i]);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));

        glGenBuffers(1, &EBO[i]);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO[i]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        glGenTextures(3, textures[i]);

        GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
        glCompileShader(vertex_shader);

        GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragment_shader, 1, &fragment_shader_source, NULL);
        glCompileShader(fragment_shader);

        GLuint program = glCreateProgram();
        glAttachShader(program, vertex_shader);
        glAttachShader(program, fragment_shader);
        glLinkProgram(program);
        glUseProgram(program);

        glUniform1i(glGetUniformLocation(program, "TextureY"), 0); // Texture unit 0
        glUniform1i(glGetUniformLocation(program, "TextureU"), 1); // Texture unit 1
        glUniform1i(glGetUniformLocation(program, "TextureV"), 2); // Texture unit 2
    }

    int ret;
    AVDictionary *options = NULL;
    av_dict_set(&options, "rtmp_buffer", "0", 0);           // Minimal buffering for low latency
    av_dict_set(&options, "rtmp_live", "live", 0);          // Live stream
    av_dict_set(&options, "tcp_nodelay", "1", 0);           // Disable Nagle’s algorithm
    av_dict_set(&options, "analyzeduration", "1000000", 0); // Lower analyze duration
    av_dict_set(&options, "probesize", "500000", 0);        // Lower probe size

    AVFormatContext *format_context = avformat_alloc_context();
    avformat_open_input(&format_context, argv[1], NULL, &options);
    avformat_find_stream_info(format_context, NULL);

    AVPacket *pkt  = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    const AVCodec *vc   = NULL;
    const AVCodec *ac   = NULL;
    int vci             = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, &vc, 0);
    int aci             = av_find_best_stream(format_context, AVMEDIA_TYPE_AUDIO, -1, -1, &ac, 0);
    AVCodecContext *vcc = NULL;
    AVCodecContext *acc = NULL;

    if (vci >= 0) {
        vcc = avcodec_alloc_context3(vc);
        avcodec_parameters_to_context(vcc, format_context->streams[vci]->codecpar);
        av_opt_set_int(vcc, "threads", 16, 0);
        avcodec_open2(vcc, vc, NULL);
    }

    if (aci >= 0) {
        acc = avcodec_alloc_context3(ac);
        avcodec_parameters_to_context(acc, format_context->streams[aci]->codecpar);
        avcodec_open2(acc, ac, NULL);
    }

    int curr_windows = WINDOWS_COUNT;
    while (curr_windows > 0 && av_read_frame(format_context, pkt) >= 0) {
        if (vcc && pkt->stream_index == vci) {
            ret = avcodec_send_packet(vcc, pkt);
            while (ret >= 0) {
                ret = avcodec_receive_frame(vcc, frame);
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;

                for (int i = 0; i < curr_windows; i++) {
                    GLFWwindow *window = windows[i];

                    if (window && !glfwWindowShouldClose(window)) {
                        glfwMakeContextCurrent(window);
                        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                        glClear(GL_COLOR_BUFFER_BIT);

                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, textures[i][0]);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame->width / 1, frame->height / 1, 0, GL_RED, GL_UNSIGNED_BYTE, frame->data[0]);

                        glActiveTexture(GL_TEXTURE1);
                        glBindTexture(GL_TEXTURE_2D, textures[i][1]);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame->width / 2, frame->height / 2, 0, GL_RED, GL_UNSIGNED_BYTE, frame->data[1]);

                        glActiveTexture(GL_TEXTURE2);
                        glBindTexture(GL_TEXTURE_2D, textures[i][2]);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame->width / 2, frame->height / 2, 0, GL_RED, GL_UNSIGNED_BYTE, frame->data[2]);
                        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

                        glfwPollEvents();
                        glfwSwapBuffers(window);
                    } else if (window) {
                        glfwDestroyWindow(window);
                        window = NULL;
                        curr_windows--;
                    }
                }
            }
        }

        av_packet_unref(pkt);
    }

    return 0;
}
