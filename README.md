# modemsimulator PTY handler

`modemsimulator-pty-handler` lets a Linux process that expects a serial modem on a `/dev/tty*` path talk to any executable that implements a modem protocol on standard input and standard output.

The bridge creates a pseudo-terminal, prints the slave path, starts the simulator command, and relays bytes in both directions:

```text
original process <-> /dev/pts/N <-> modemsimulator-pty-handler <-> simulator stdin/stdout
```

## Build

```sh
make
```

This builds:

- `bin/modemsimulator-pty-handler`: PTY to stdio bridge
- `bin/libmodemsimulator-pty-ioctl.so`: optional `LD_PRELOAD` ioctl shim for DTR/DSR/CD on Linux PTYs
- `bin/modemsimulator-hayes-bridge`: Hayes-compatible AT command simulator

## Use

Start the bridge with any simulator command after `--`:

```sh
bin/modemsimulator-pty-handler -- bin/modemsimulator-hayes-bridge
```

The bridge prints a tty path on stderr, for example:

```text
/dev/pts/7
```

Configure the original process to open that path instead of the real modem device.

For scripts, write the tty path to a file:

```sh
bin/modemsimulator-pty-handler --tty-file /tmp/modem-sim.tty -- bin/modemsimulator-hayes-bridge
```

Then read `/tmp/modem-sim.tty` and pass that path to the original process.

## Hayes Simulator Data Mode

`modemsimulator-hayes-bridge` starts a data-mode child process when it receives an `ATD...` dial command. The child process receives serial input on stdin and its stdout is sent back to the serial line:

```sh
bin/modemsimulator-pty-handler --tty-file /tmp/modem-sim.tty -- bin/modemsimulator-hayes-bridge -- /bin/cat
```

If no child command is provided, the demo uses `/bin/cat`.

Accepted commands in command mode:

- `AT`: returns `OK`.
- `ATZ`: resets modem settings and returns `OK`.
- `ATE0`: disables command echo and returns `OK`.
- `ATE1`: enables command echo and returns `OK`.
- `ATI`: returns the simulator identification and `OK`.
- `ATD...`: starts the data child when needed, raises CD, returns `CONNECT 9600`, and enters data mode.
- `ATO`: returns to data mode if the child is still running; otherwise returns `NO CARRIER`.
- `ATH`: kills the child if it is running, drops CD, and returns `NO CARRIER`.
- `AT&D0`: accepts DTR drops without changing the connection state.
- `AT&D1`: switches from data mode to command mode when DTR drops, without killing the child.
- `AT&D2`: hangs up when DTR drops. This is the default.
- `AT&D3`: hangs up and resets modem settings when DTR drops.

While in data mode, serial input is forwarded to the child process. The escape sequence `+++` switches back to command mode without killing the child. If the child exits by itself, CD is dropped and the modem returns `NO CARRIER`.

## DTR, DSR, and CD

Linux pseudo-terminals do not reliably implement serial modem-control ioctls such as `TIOCMGET` and `TIOCMSET`. When the original process depends on DTR/DSR/CD, run it with the provided preload shim and the same control file used by the bridge:

```sh
bin/modemsimulator-pty-handler --tty-file /tmp/modem-sim.tty --control-file /tmp/modem-sim.ctrl -- bin/modemsimulator-hayes-bridge
```

In another process:

```sh
MODEM_PTY_HANDLER_CONTROL=/tmp/modem-sim.ctrl \
LD_PRELOAD=$PWD/bin/libmodemsimulator-pty-ioctl.so \
your-original-process --serial-device "$(cat /tmp/modem-sim.tty)"
```

The shim emulates `TIOCMGET`, `TIOCMSET`, `TIOCMBIS`, `TIOCMBIC`, and `TIOCMIWAIT` for the original process. The bridge applies these policies:

- DSR follows DTR.
- CD follows the Hayes simulator data child: high while the child process is running, low when there is no child process.
- `CONNECT` is returned after CD goes high.
- `NO CARRIER` is returned after CD goes low.

## Simulator contract

The simulator executable reads serial bytes from stdin and writes serial bytes to stdout. Keep diagnostics on stderr so they do not enter the simulated serial stream.

The bridge configures the pseudo-terminal in raw mode, so bytes are relayed without canonical line processing or terminal echo on the PTY side.