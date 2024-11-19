extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/time.h>
}

#include "gl.h"

#include <stdio.h>

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

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        printf("USAGE: %s <url>\n", argv[0]);
        return 1;
    }

    av_log_set_level(AV_LOG_DEBUG);
    int ret;
    AVFormatContext *format_context = NULL;
    avformat_open_input(&format_context, argv[1], NULL, NULL);
    avformat_find_stream_info(format_context, NULL);

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *sw_vframe = av_frame_alloc();

    const AVCodec *vc = NULL;
    int vci = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, &vc, 0);

    AVCodecContext *vcc = NULL;
    SwsContext *sws_ctx = NULL;

    if (vci >= 0) {
        vcc = avcodec_alloc_context3(vc);
        avcodec_parameters_to_context(vcc, format_context->streams[vci]->codecpar);
        avcodec_open2(vcc, vc, NULL);

        sws_ctx = sws_getContext(vcc->width, vcc->height, vcc->pix_fmt, 1280, 720, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    }

    int64_t its = av_gettime_relative();

    GLFWwindow *window = init_window();
    GLuint prog = init_opengl();


    while (!glfwWindowShouldClose(window)) {
        ret = av_read_frame(format_context, packet);
        if (ret == AVERROR_EOF) break;
        if (ret == AVERROR(EAGAIN)) continue;

        for (int key = GLFW_KEY_0; key < GLFW_KEY_9; key++) {
            if (glfwGetKey(window, key) == GLFW_PRESS) {
                glUniform1i(glGetUniformLocation(prog, "Filter"), key - GLFW_KEY_0);
            }
        }

        if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) break;

        if (vcc && packet->stream_index == vci) {
            ret = avcodec_send_packet(vcc, packet);
            while (ret >= 0) {
                ret = avcodec_receive_frame(vcc, frame);
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;

                if ((ret = sws_scale_frame(sws_ctx, sw_vframe, frame)) < 0) {
                    fprintf(stderr, "ERROR: sws_scale_frame %s\n", av_err2str(ret));
                    return 1;
                }

                AVStream *vstream = format_context->streams[vci];
                int64_t fts = (1e6 * frame->pts * vstream->time_base.num) / vstream->time_base.den;
                int64_t rts = av_gettime_relative() - its;
                if (fts > rts) av_usleep(fts - rts);

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
