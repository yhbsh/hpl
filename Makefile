PREFIX ?= $(HOME)/.local
BINDIR := $(PREFIX)/bin

default: hpl

hpl: main.c
	clang -o hpl main.c -lavcodec -lavformat -lavutil -lswscale -lswresample -lglfw3 -framework OpenGL -framework Cocoa -framework IOKit -DGLFW_INCLUDE_GLCOREARB -DMINIAUDIO_IMPLEMENTATION -DGL_SILENCE_DEPRECATION 

install: hpl
	mkdir -p $(BINDIR)
	cp hpl $(BINDIR)
