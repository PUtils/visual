PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1
CC ?= clang
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wshadow -Wno-unused-parameter -Iinclude
CFLAGS += $(shell pkg-config --cflags libssh2 sdl2 vterm SDL2_ttf)
LDLIBS += $(shell pkg-config --libs libssh2 sdl2 vterm SDL2_ttf) -lpthread

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/%.o)

all: build/visual

VTERM_LIBDIR := $(shell pkg-config --variable=libdir vterm)

build/visual: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)
	@# Homebrew's libvterm has an install_name pointing at /usr/local even
	@# on Apple Silicon; rewrite it to the actual libdir so the binary runs.
	@if [ -f "$(VTERM_LIBDIR)/libvterm.0.dylib" ] && command -v install_name_tool >/dev/null 2>&1; then \
		install_name_tool -change /usr/local/lib/libvterm.0.dylib "$(VTERM_LIBDIR)/libvterm.0.dylib" $@ 2>/dev/null || true; \
	fi

build/%.o: src/%.c include/vis.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

install: build/visual
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	install -m 755 build/visual $(DESTDIR)$(BINDIR)/visual
	install -m 644 man/visual.1 $(DESTDIR)$(MANDIR)/visual.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/visual
	rm -f $(DESTDIR)$(MANDIR)/visual.1

clean:
	rm -rf build

.PHONY: all install uninstall clean config