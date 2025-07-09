PREFIX ?= $(HOME)/.local
BINDIR := $(PREFIX)/bin

default: hpl

hpl: hpl.o
	clang -O3 -o hpl hpl.o \
		-lavcodec -lavformat -lavutil -lswscale -lswresample -lglfw3 -lx264 -lx265 -lc++ -lvpx -lz -lbz2 -llzma -liconv \
		-framework OpenGL -framework Cocoa -framework IOKit -framework VideoToolbox -framework AudioToolbox -framework CoreVideo -framework CoreMedia -framework Security \

hpl.o: hpl.c
	clang -O3 -g -c -o hpl.o hpl.c -DGLFW_INCLUDE_GLCOREARB -DMINIAUDIO_IMPLEMENTATION -DGL_SILENCE_DEPRECATION 

install: hpl
	mkdir -p $(BINDIR)
	cp hpl $(BINDIR)
