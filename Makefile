PREFIX ?= $(HOME)/.local
BINDIR := $(PREFIX)/bin
CFLAGS := `pkg-config --cflags libavformat libavcodec libswscale raylib` -g
LDFLAGS := `pkg-config --static --libs libavformat libavcodec libswscale raylib`

hpl: hpl.o
	clang -o $@ $^ $(LDFLAGS)

hpl.o: hpl.c
	clang $(CFLAGS) -c -o $@ $<

install: hpl
	mkdir -p $(BINDIR)
	cp $< $(BINDIR)

clean:
	rm -f hpl.o hpl
