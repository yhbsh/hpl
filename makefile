CFLAGS=-g3 `pkg-config --cflags glfw3 libavcodec libavformat sdl2 sdl2_image`
LDFLAGS=`pkg-config --libs glfw3 libavcodec libavformat sdl2 sdl2_image` -framework opengl

CC=clang

all: ffprobe opengl hplayer

hplayer: hplayer.c
	$(CC) $(CFLAGS) hplayer.c -o hplayer $(LDFLAGS)

opengl: opengl.c
	$(CC) $(CFLAGS) opengl.c  -o opengl $(LDFLAGS)

ffprobe: ffprobe.c
	$(CC) $(CFLAGS) ffprobe.c -o ffprobe $(LDFLAGS)
