PREFIX ?= $(HOME)/.local
BINDIR := $(PREFIX)/bin

default: hpl

hpl: main.c
	clang -O2 -DGLFW_INCLUDE_GLCOREARB -DMINIAUDIO_IMPLEMENTATION -DGL_SILENCE_DEPRECATION -o hpl main.c $(shell pkg-config --cflags --libs libavformat libavcodec libswscale glfw3) -framework OpenGL

install: hpl
	mkdir -p $(BINDIR)
	cp hpl $(BINDIR)
