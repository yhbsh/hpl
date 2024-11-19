CFLAGS     = -D__STDC_CONSTANT_MACROS -I../../deps/include -DGL_SILENCE_DEPRECATION
LIBS       = -L../../deps/lib -lglfw -lavformat -lavcodec -lswscale -lswresample -lavutil -lportaudio
FRAMEWORKS = -framework CoreMedia -framework CoreVideo -framework Videotoolbox -framework OpenGL -framework Cocoa -framework IOKit -framework CoreAudio -framework AudioToolbox

all: main

main: main.c
	clang $(CFLAGS) main.c -o main $(LIBS) $(FRAMEWORKS)

clean:
	rm -f main
