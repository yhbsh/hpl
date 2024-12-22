CFLAGS     = -D__STDC_CONSTANT_MACROS -DGL_SILENCE_DEPRECATION $(shell pkg-config --cflags libavcodec libavformat libavutil portaudio-2.0)
LIBS       = $(shell pkg-config --libs libavcodec libavformat libavutil portaudio-2.0)
FRAMEWORKS = -framework CoreMedia -framework CoreVideo -framework Videotoolbox -framework OpenGL -framework Cocoa -framework IOKit -framework CoreAudio -framework AudioToolbox

all: main

main: main.c
	clang $(CFLAGS) main.c -o main $(LIBS) $(FRAMEWORKS)

clean:
	rm -f main
