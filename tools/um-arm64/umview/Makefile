# umview -- SDL2 viewer for a umfb sink (UML on-screen framebuffer).
#
# The wire format header (umfb.h) is a verbatim copy of
# arch/um/drivers/umfb.h; keep them in sync (the viewer checks magic +
# version at runtime, so a stale copy fails loudly, not silently).
#
#   make            build ./umview
#   make clean
#
# Needs libsdl2-dev (Debian/Ubuntu: apt install libsdl2-dev).

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += $(shell sdl2-config --cflags)
LDLIBS  += $(shell sdl2-config --libs)

umview: umview.c umfb.h
	$(CC) $(CFLAGS) -o $@ umview.c $(LDLIBS)

clean:
	rm -f umview

.PHONY: clean