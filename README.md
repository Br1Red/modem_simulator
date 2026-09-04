# modemsimulator PTY handler

`modemsimulator-pty-handler` lets a Linux process that expects a serial modem on a `/dev/tty*` path talk to any executable that implements a modem protocol on standard input and standard output.

The PTY handler creates a pseudo-terminal, prints the slave path, starts the simulator command, and relays bytes in both directions:

```text
original process <-> /dev/pts/N <-> modemsimulator-pty-handler <-> simulator stdin/stdout
```

## Build

```sh
make
```

This builds:

- `bin/modemsimulator-pty-handler`: PTY to stdio handler
- `bin/libmodemsimulator-pty-ioctl.so`: optional `LD_PRELOAD` ioctl shim for DTR/DSR/CD on Linux PTYs
- `bin/modemsimulator-hayes-bridge`: Hayes-compatible AT command bridge

## Use

Start the PTY handler with any simulator command after `--`:

```sh
bin/modemsimulator-pty-handler -- bin/modemsimulator-hayes-bridge
```

The PTY handler prints a tty path on stderr, for example:

```text
/dev/pts/7
```

Configure the original process to open that path instead of the real modem device.

For scripts, write the tty path to a file:

```sh
bin/modemsimulator-pty-handler --tty-file /tmp/modem-sim.tty -- bin/modemsimulator-hayes-bridge
```

Then read `/tmp/modem-sim.tty` and pass that path to the original process.

## Hayes Simulator Bridge

`modemsimulator-hayes-bridge` starts a data-mode child process when it receives an `ATD...` dial command. The child process receives serial input on stdin and its stdout is sent back to the serial line:

```sh
bin/modemsimulator-pty-handler --tty-file /tmp/modem-sim.tty -- bin/modemsimulator-hayes-bridge -- /bin/cat
```

If no child command is provided, the demo uses `/bin/cat`.

Two different simulation processes can be configured: one for outgoing calls started by `ATD...`, and one for incoming calls answered with `ATA`:

```sh
bin/modemsimulator-pty-handler --tty-file /tmp/modem-sim.tty --control-file /tmp/modem-sim.ctrl -- \
	bin/modemsimulator-hayes-bridge \
		--dial-command /path/to/outgoing-simulator arg... \
		--incoming-command /path/to/incoming-simulator arg...
```

The older form after `--` is still accepted and uses the same child command for both outgoing and incoming calls.

Accepted commands in command mode:

- `AT`: returns `OK`.
- `ATZ`: resets modem settings and returns `OK`.
- `ATE0`: disables command echo and returns `OK`.
- `ATE1`: enables command echo and returns `OK`.
- `ATI`: returns the simulator identification and `OK`.
- `ATD...`: starts the data child when needed, raises CD, returns `CONNECT 9600`, and enters data mode.
- `ATA`: answers an incoming call, raises CD, returns `CONNECT 9600`, and enters data mode.
- `ATO`: returns to data mode if the child is still running; otherwise returns `NO CARRIER`.
- `ATH` or `ATH0`: rejects an incoming call, or kills the child if it is running, drops CD, and returns `NO CARRIER`.
- `ATS0=n`: sets auto-answer after `n` rings. `0` disables auto-answer.
- `ATS0?`: returns the current `S0` value.
- `AT&D0`: accepts DTR drops without changing the connection state.
- `AT&D1`: switches from data mode to command mode when DTR drops, without killing the child.
- `AT&D2`: hangs up when DTR drops. This is the default.
- `AT&D3`: hangs up and resets modem settings when DTR drops.

While in data mode, serial input is forwarded to the child process. The escape sequence `+++` switches back to command mode without killing the child. If the child exits by itself, CD is dropped and the modem returns `NO CARRIER`.

To simulate an incoming call immediately at startup, start the Hayes bridge with `--incoming-call` before the child command:

```sh
bin/modemsimulator-pty-handler --tty-file /tmp/modem-sim.tty --control-file /tmp/modem-sim.ctrl -- bin/modemsimulator-hayes-bridge --incoming-call -- /bin/cat
```

While the call is ringing, the modem periodically returns `RING` and raises the RI/RNG modem-control bit. `ATA` answers the call manually, `ATH` or `ATH0` rejects it, and `ATS0=n` answers automatically after `n` rings.

To trigger an incoming call while the bridge is already running, send `SIGUSR1` to the `modemsimulator-hayes-bridge` process. `SIGUSR1` is honored only in command mode; while the modem is in data mode it is ignored.

## DTR, DSR, and CD

Linux pseudo-terminals do not reliably implement serial modem-control ioctls such as `TIOCMGET` and `TIOCMSET`. When the original process depends on DTR/DSR/CD, run it with the provided preload shim and the same control file used by the PTY handler:

```sh
bin/modemsimulator-pty-handler --tty-file /tmp/modem-sim.tty --control-file /tmp/modem-sim.ctrl -- bin/modemsimulator-hayes-bridge
```

In another process:

```sh
MODEM_PTY_HANDLER_CONTROL=/tmp/modem-sim.ctrl \
LD_PRELOAD=$PWD/bin/libmodemsimulator-pty-ioctl.so \
your-original-process --serial-device "$(cat /tmp/modem-sim.tty)"
```

The shim emulates `TIOCMGET`, `TIOCMSET`, `TIOCMBIS`, `TIOCMBIC`, and `TIOCMIWAIT` for the original process. The PTY handler applies these policies:

- DSR follows DTR.
- CD follows the Hayes simulator data child: high while the child process is running, low when there is no child process.
- RI/RNG is raised while an incoming call ring indication is active.

## Simulator contract

The simulator executable reads serial bytes from stdin and writes serial bytes to stdout. Keep diagnostics on stderr so they do not enter the simulated serial stream.

The bridge configures the pseudo-terminal in raw mode, so bytes are relayed without canonical line processing or terminal echo on the PTY side.