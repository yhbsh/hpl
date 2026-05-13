PREFIX  := ~/.local/bin

CFLAGS  := `pkg-config --cflags libavformat libavcodec libavutil libswscale libswresample sdl2`
LDFLAGS := `pkg-config --libs libavformat libavcodec libavutil libswscale libswresample sdl2`

main: main.c
	clang -O3 -o main main.c $(CFLAGS) $(LDFLAGS)

install: main
	cp main $(PREFIX)/hpl
