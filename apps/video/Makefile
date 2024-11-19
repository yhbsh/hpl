CFLAGS     = -D__STDC_CONSTANT_MACROS -I../../deps/include -DGL_SILENCE_DEPRECATION
LIBS       = -L../../deps/lib -lglfw -lavformat -lavcodec -lswscale -lswresample -lavutil -lportaudio
FRAMEWORKS = -framework CoreMedia -framework CoreVideo -framework Videotoolbox -framework OpenGL -framework Cocoa -framework IOKit -framework CoreAudio -framework AudioToolbox

all: main

main: main.cpp
	clang++ -std=c++11 $(CFLAGS) main.cpp gl.cpp -o main $(LIBS) $(FRAMEWORKS)

clean:
	rm -f main
