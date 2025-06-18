default: main

main: main.c
	clang -DMINIAUDIO_IMPLEMENTATION -DGL_SILENCE_DEPRECATION main.c -o main $(shell pkg-config --cflags --libs libavformat libavcodec libswscale glfw3) -framework OpenGL
