CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

BUILD_DIR := build
BIN_DIR := bin

.PHONY: all clean

all: $(BIN_DIR)/modemsimulator-pty-handler $(BIN_DIR)/libmodemsimulator-pty-ioctl.so $(BIN_DIR)/modemsimulator-hayes-bridge

$(BIN_DIR):
	mkdir -p $@

$(BIN_DIR)/modemsimulator-pty-handler: src/modem_simulator_pty_handler.c src/modem_simulator_lib.c src/modem_simulator_lib.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/modem_simulator_pty_handler.c src/modem_simulator_lib.c -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/libmodemsimulator-pty-ioctl.so: src/modem_simulator_pty_ioctl.c src/modem_simulator_lib.c src/modem_simulator_lib.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared src/modem_simulator_pty_ioctl.c src/modem_simulator_lib.c -o $@ -ldl

$(BIN_DIR)/modemsimulator-hayes-bridge: src/modem_simulator_hayes_bridge.c src/modem_simulator_lib.c src/modem_simulator_lib.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/modem_simulator_hayes_bridge.c src/modem_simulator_lib.c -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf $(BIN_DIR) $(BUILD_DIR)