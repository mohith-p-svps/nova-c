# Nova build
#
# Native module sources live under natives/ (see natives/natives.h),
# including the natives/graphics/ subfolder — this Makefile picks all of
# it up automatically via the wildcards below, so `make` keeps working
# without listing files by hand as more get added.
#
# Dependencies beyond a plain C toolchain:
#   - libcurl (the net module) — e.g. `apt install libcurl4-openssl-dev`
#     on Debian/Ubuntu, or MSYS2's `mingw-w64-ucrt-x86_64-curl` on Windows.
#   - raylib + raygui (the graphics module) — see the project roadmap's
#     §4 for the full per-platform setup. Short version:
#       Windows (MSYS2 UCRT64): pacman -S mingw-w64-ucrt-x86_64-raylib
#       macOS:                  brew install raylib
#       Linux:                  build raylib from source (see raylib's
#                                own wiki — apt has the X11/GL/ALSA dev
#                                packages raylib needs, but not raylib
#                                itself on most distros)
#     raygui.h itself is header-only and isn't packaged anywhere — see
#     natives/graphics/vendor/README.txt for where to get it and where
#     it goes. The build will not compile without it.
#
# Usage:
#   make            # builds ./nova (nova.exe on Windows/MinGW)
#   make clean

CC     := gcc
CFLAGS := -Wall -O2

SRCS := $(wildcard *.c) $(wildcard natives/*.c) $(wildcard natives/graphics/*.c)
OBJS := $(SRCS:.c=.o)

ifeq ($(OS),Windows_NT)
TARGET := nova.exe
LDLIBS := -lgmp -lmpfr -lm -lcurl -lraylib -lopengl32 -lgdi32 -lwinmm
else
TARGET  := nova
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
CFLAGS  += $(shell pkg-config --cflags raylib 2>/dev/null)
LDLIBS  := -lgmp -lmpfr -lm -lcurl -framework IOKit -framework Cocoa -framework OpenGL $(shell pkg-config --libs raylib 2>/dev/null || echo -lraylib)
else
LDLIBS  := -lgmp -lmpfr -lm -lcurl -lraylib -lGL -lpthread -ldl -lrt -lX11
endif
endif

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
