# tikuConsole (macOS) — the command-line slmux bridge + the GTK4 GUI.
#
#   make            both: ./slmux (CLI) and ./tikuconsole (GUI)
#   make slmux      just the CLI (libc only, no dependencies)
#   make tikuconsole just the GUI (needs GTK4: `brew install gtk4 pkg-config`)
#
# GTK4 from Homebrew lives under /opt/homebrew; make sure that's on PATH (or
# PKG_CONFIG_PATH) so pkg-config resolves gtk4.

CC     ?= clang
CFLAGS ?= -O2 -Wall -Wextra
PKG    ?= pkg-config

GTK_CFLAGS := $(shell $(PKG) --cflags gtk4)
GTK_LIBS   := $(shell $(PKG) --libs gtk4)
FRAMEWORKS := -framework IOKit -framework CoreFoundation

all: slmux tikuconsole

slmux: slmux.c bridge.c bridge.h
	$(CC) $(CFLAGS) -o $@ slmux.c bridge.c

GUI_SRC := gui.c gui_net.c gui_ping.c gui_build.c gui_splash.c \
           bridge.c ports.c packets.c
GUI_HDR := gui.h bridge.h ports.h packets.h

tikuconsole: $(GUI_SRC) $(GUI_HDR)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ $(GUI_SRC) \
	    $(GTK_LIBS) $(FRAMEWORKS)

clean:
	rm -f slmux tikuconsole

.PHONY: all clean
