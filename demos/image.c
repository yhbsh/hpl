#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>

#include <dirent.h>

#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

GLuint load_shader(const char *file_path, GLenum shader_type) {
    FILE *file = fopen(file_path, "rb");
    if (!file) {
        fprintf(stderr, "Failed to open shader file: %s\n", file_path);
        return 0;
    }

    fseek(file, 0, SEEK_END);
    int size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *data = (char *)malloc(size + 1);
    if (!data) {
        fprintf(stderr, "Failed to allocate memory for shader file\n");
        fclose(file);
        return 0;
    }

    fread(data, 1, size, file);
    data[size] = '\0';
    fclose(file);

    GLuint shader = glCreateShader(shader_type);
    glShaderSource(shader, 1, (const char **)&data, NULL);
    glCompileShader(shader);
    free(data);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, sizeof(infoLog), NULL, infoLog);
        fprintf(stderr, "Shader compilation error: %s\n", infoLog);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

GLuint load_program(GLuint vert_shader, GLuint frag_shader) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vert_shader);
    glAttachShader(program, frag_shader);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, sizeof(infoLog), NULL, infoLog);
        fprintf(stderr, "Program linking error: %s\n", infoLog);

        glDeleteProgram(program);
        return 0;
    }

    glDeleteShader(vert_shader);
    glDeleteShader(frag_shader);

    return program;
}

int load_images(const char *directory, GLuint **textures, int *textures_count) {
    DIR *dir;
    struct dirent *entry;
    int count    = 0;
    char **paths = NULL;

    dir = opendir(directory);
    if (!dir) {
        fprintf(stderr, "Failed to open directory: %s\n", directory);
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".jpeg") || strstr(entry->d_name, ".jpg") || strstr(entry->d_name, ".png") || strstr(entry->d_name, ".bmp") || strstr(entry->d_name, ".gif") || strstr(entry->d_name, ".psd") || strstr(entry->d_name, ".tga") || strstr(entry->d_name, ".pic") ||
            strstr(entry->d_name, ".ppm") || strstr(entry->d_name, ".pgm")) {
            count++;
        }
    }

    paths     = (char **)malloc(count * sizeof(char *));
    *textures = (GLuint *)malloc(count * sizeof(GLuint));

    rewinddir(dir);
    int index = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".jpeg") || strstr(entry->d_name, ".jpg") || strstr(entry->d_name, ".png") || strstr(entry->d_name, ".bmp") || strstr(entry->d_name, ".gif") || strstr(entry->d_name, ".psd") || strstr(entry->d_name, ".tga") || strstr(entry->d_name, ".pic") ||
            strstr(entry->d_name, ".ppm") || strstr(entry->d_name, ".pgm")) {
            char filepath[1024];
            snprintf(filepath, sizeof(filepath), "%s/%s", directory, entry->d_name);
            paths[index] = strdup(filepath);
            index++;
        }
    }

    closedir(dir);

    glGenTextures(count, *textures);
    for (int i = 0; i < count; i++) {
        int x, y, n;
        stbi_uc *data = stbi_load(paths[i], &x, &y, &n, 0);

        if (data) {
            GLenum format;
            if (n == 1)
                format = GL_RED;
            else if (n == 3)
                format = GL_RGB;
            else if (n == 4)
                format = GL_RGBA;
            else {
                fprintf(stderr, "Unknown image format with %d components for: %s\n", n, paths[i]);
                stbi_image_free(data);
                continue;
            }

            glBindTexture(GL_TEXTURE_2D, (*textures)[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, format, x, y, 0, format, GL_UNSIGNED_BYTE, data);
            glBindTexture(GL_TEXTURE_2D, 0);
            stbi_image_free(data);
        } else {
            fprintf(stderr, "Failed to load image: %s\n", paths[i]);
        }

        free(paths[i]);
    }

    free(paths);
    *textures_count = count;

    return 0;
}

int main(void) {

    if (!glfwInit()) {
        return 1;
    }

    const int w = 1280, h = 720;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    GLFWwindow *window = glfwCreateWindow(w, h, "Window", NULL, NULL);
    if (!window) {
        glfwTerminate();
        return 1;
    }

    glfwSetWindowAspectRatio(window, 16, 9);
    glfwMakeContextCurrent(window);

    GLuint vert_shader = load_shader("assets/image_vert.glsl", GL_VERTEX_SHADER);
    GLuint frag_shader = load_shader("assets/image_frag.glsl", GL_FRAGMENT_SHADER);
    GLuint program     = load_program(vert_shader, frag_shader);

    GLuint *textures   = NULL;
    int textures_count = 0;

    int ret;
    if ((ret = load_images("/Users/home/Pictures", &textures, &textures_count)) < 0) {
        return 1;
    }
    assert(textures != NULL);
    assert(textures_count > 0);

    // clang-format off
    GLfloat vertices[] = {
        // positions      // texture coords
        -1.0f, +1.0f, +0.0f, +0.0f,
        -1.0f, -1.0f, +0.0f, +1.0f,
        +1.0f, +1.0f, +1.0f, +0.0f,
        +1.0f, -1.0f, +1.0f, +1.0f,
    };
    // clang-format on

    GLuint VAO;
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    GLuint VBO;
    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    GLuint position_attribute = glGetAttribLocation(program, "position");
    glEnableVertexAttribArray(position_attribute);
    glVertexAttribPointer(position_attribute, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void *)0);

    GLuint texcoords_attribute = glGetAttribLocation(program, "texCoord");
    glEnableVertexAttribArray(texcoords_attribute);
    glVertexAttribPointer(texcoords_attribute, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (GLvoid *)(2 * sizeof(GLfloat)));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    double lastSwapTime   = glfwGetTime();
    int currentImageIndex = 0;

    while (!glfwWindowShouldClose(window)) {
        glClear(GL_COLOR_BUFFER_BIT);

        double currentTime = glfwGetTime();
        if (currentTime - lastSwapTime >= 2.0) {
            currentImageIndex++;
            currentImageIndex %= textures_count;

            lastSwapTime = currentTime;
        }

        glBindTexture(GL_TEXTURE_2D, textures[currentImageIndex]);
        glUseProgram(program);
        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
