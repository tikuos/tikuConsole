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
# The BLE transport (ble_mac.m) is Objective-C + CoreBluetooth.
BLE_FRAMEWORKS := -framework CoreBluetooth -framework Foundation

all: slmux tikuconsole

slmux: slmux.c bridge.c bridge.h
	$(CC) $(CFLAGS) -o $@ slmux.c bridge.c

GUI_SRC := gui.c gui_net.c gui_wifi.c gui_ping.c gui_build.c gui_splash.c \
           gui_files.c bridge.c ports.c packets.c fs.c
GUI_HDR := gui.h bridge.h ports.h packets.h fs.h

# Objective-C CoreBluetooth transport (compiled with ARC, linked into the GUI).
ble_mac.o: ble_mac.m ble_mac.h
	$(CC) $(CFLAGS) -fobjc-arc -c -o $@ ble_mac.m

tikuconsole: $(GUI_SRC) $(GUI_HDR) ble_mac.o
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ $(GUI_SRC) ble_mac.o \
	    $(GTK_LIBS) $(FRAMEWORKS) $(BLE_FRAMEWORKS)

# Standalone CoreBluetooth transport smoke test (no GTK):
#   make ble_test && ./ble_test tikuOS   (with `ble uart` running on the board)
ble_test: ble_mac.m ble_mac.h
	$(CC) $(CFLAGS) -DBLE_MAC_TEST -fobjc-arc -o $@ ble_mac.m $(BLE_FRAMEWORKS)

# Headless self-test for the /data transfer protocol (no GTK needed):
#   make fstest && ./fstest /dev/cu.usbmodemXXXX
fstest: fs.c bridge.c fs.h bridge.h
	$(CC) $(CFLAGS) -DFS_TEST -o $@ fs.c bridge.c $(FRAMEWORKS)

clean:
	rm -f slmux tikuconsole fstest ble_test ble_mac.o

.PHONY: all clean
