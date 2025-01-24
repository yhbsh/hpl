all: audio video

CFLAGS := -O3 -DGL_SILENCE_DEPRECATION $(shell pkg-config --cflags libavformat libavcodec libswscale libavutil glfw3 portaudio-2.0)
LIBS := $(shell pkg-config --libs libavformat libavcodec libswscale libavutil glfw3 portaudio-2.0)

video: video.c
	clang $(CFLAGS) video.c -o video $(LIBS) -framework OpenGL

audio: audio.c
	clang $(CFLAGS) audio.c -o audio $(LIBS) -framework OpenGL

clean:
	rm -f video audio
