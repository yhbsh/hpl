#include "gl.h"

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

uniform sampler2D Texture;
uniform int Filter;

out vec4 FragColor;

void main() {
    vec4 color = texture(Texture, TexCoord);

    switch (Filter) {
        case 1: // Sepia
            color.rgb = vec3(
                clamp(color.r * 0.393 + color.g * 0.769 + color.b * 0.189, 0.0, 1.0),
                clamp(color.r * 0.349 + color.g * 0.686 + color.b * 0.168, 0.0, 1.0),
                clamp(color.r * 0.272 + color.g * 0.534 + color.b * 0.131, 0.0, 1.0)
            );
            break;
        case 2: // Grayscale
            float gray = dot(color.rgb, vec3(0.299, 0.587, 0.114));
            color.rgb = vec3(gray, gray, gray);
            break;
        case 3: // Invert
            color.rgb = vec3(1.0) - color.rgb;
            break;
        case 4: // Adjust Brightness
            color.rgb = clamp(color.rgb * 1.2, 0.0, 1.0);
            break;
        case 5: // Adjust Saturation
            float luminance = dot(color.rgb, vec3(0.299, 0.587, 0.114));
            color.rgb = mix(vec3(luminance), color.rgb, 1.5);
            break;
    }

    FragColor = color;
})";
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

