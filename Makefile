PREFIX ?= $(HOME)/.local
BINDIR := $(PREFIX)/bin

default: hpl

hpl: hpl.o
	clang -o hpl hpl.o $(shell pkg-config --static --libs libavformat libavcodec libswscale libswresample glfw3) -framework OpenGL


hpl.o: hpl.cc
	clang++ $(shell pkg-config --cflags libavformat libavcodec libswscale libswresample glfw3) -g -c -o hpl.o hpl.cc -DGLFW_INCLUDE_GLCOREARB -DMINIAUDIO_IMPLEMENTATION -DGL_SILENCE_DEPRECATION

install: hpl
	mkdir -p $(BINDIR)
	cp hpl $(BINDIR)
