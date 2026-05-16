PREFIX  := ~/.local/bin

CFLAGS  := `pkg-config --cflags libavformat libavcodec libavutil libswscale libswresample SDL2 SDL2_ttf`
LDFLAGS := `pkg-config --libs libavformat libavcodec libavutil libswscale libswresample SDL2 SDL2_ttf`

main: main.c
	clang -O3 -o main main.c $(CFLAGS) $(LDFLAGS)

install: main
	cp main $(PREFIX)/hpl
