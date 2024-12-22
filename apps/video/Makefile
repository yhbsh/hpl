CFLAGS     = -D__STDC_CONSTANT_MACROS -DGL_SILENCE_DEPRECATION $(shell pkg-config --cflags libavformat libavcodec libswscale libavutil glfw3)
LIBS       = $(shell pkg-config --libs libavformat libavcodec libswscale libavutil glfw3)
FRAMEWORKS = -framework CoreMedia -framework CoreVideo -framework Videotoolbox -framework OpenGL -framework Cocoa -framework IOKit -framework CoreAudio -framework AudioToolbox

all: main

main: main.cpp
	clang++ -std=c++11 $(CFLAGS) main.cpp gl.cpp -o main $(LIBS) $(FRAMEWORKS)

clean:
	rm -f main
