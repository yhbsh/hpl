CFLAGS := -DGL_SILENCE_DEPRECATION $(shell pkg-config --cflags libavformat libavcodec libswscale libavutil glfw3)
LIBS := $(shell pkg-config --libs libavformat libavcodec libswscale libavutil glfw3)

video: video.c
	clang $(CFLAGS) video.c -o video $(LIBS) -framework OpenGL

clean:
	rm -f video
