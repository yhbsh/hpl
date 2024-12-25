CFLAGS     = -D__STDC_CONSTANT_MACROS -DGL_SILENCE_DEPRECATION $(shell pkg-config --cflags libavformat libavcodec libswscale libavutil glfw3)
LIBS       = $(shell pkg-config --libs libavformat libavcodec libswscale libavutil glfw3)
FRAMEWORKS = -framework CoreMedia -framework CoreVideo -framework Videotoolbox -framework OpenGL -framework Cocoa -framework IOKit -framework CoreAudio -framework AudioToolbox

all: main

main.o: main.c
	clang $(CFLAGS) -c main.c

gl.o: gl.cpp
	clang++ -std=c++11 $(CFLAGS) -c gl.cpp

main: main.o gl.o
	clang++ main.o gl.o -o main $(LIBS) $(FRAMEWORKS)

clean:
	rm -f *.o main
