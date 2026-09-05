#!/usr/bin/env python3
"""ptyrun.py - run a command with a real terminal on stdin, stdout and stderr.

The guards' harness has no terminal and expect(1) is not always installed.

    ptyrun.py [--rows N] [--cols N] [--wait MARKER --send TEXT]
              [--send-after SECONDS] [--pause SECONDS] [--resize ROWS,COLS]
              [--timeout SECONDS]
              OUTFILE COMMAND [ARGS...]

Output goes to OUTFILE and the terminal's name to OUTFILE.tty; the exit status
is the command's own (124 on a timeout).  --pause leaves the terminal unread for
that long; --resize changes the window when the --wait marker appears;
--send-after is for a command whose only terminal is stdin, where a marker
would never reach the master.  TEXT may use backslash escapes (\\n).
"""
import fcntl
import os
import pty
import select
import struct
import sys
import termios
import time


def parse(argv):
    opts = {"rows": 24, "cols": 80, "timeout": 60.0, "wait": None, "send": None,
            "pause": 0.0, "resize": None, "send-after": None}
    while argv and argv[0].startswith("--"):
        name = argv.pop(0)[2:]
        if name not in opts or not argv:
            sys.exit("ptyrun: bad option --" + name)
        val = argv.pop(0)
        if name in ("rows", "cols"):
            val = int(val)
        elif name in ("timeout", "pause", "send-after"):
            val = float(val)
        elif name == "resize":
            val = tuple(int(x) for x in val.split(","))
        elif name == "send":
            val = val.encode().decode("unicode_escape").encode()
        else:
            val = val.encode()
        opts[name] = val
    if len(argv) < 2:
        sys.exit("usage: ptyrun.py [options] OUTFILE COMMAND [ARGS...]")
    return opts, argv[0], argv[1:]


def main():
    opts, outfile, cmd = parse(sys.argv[1:])
    pid, master = pty.fork()
    if pid == 0:
        # Child: the slave is its stdin, stdout, stderr and controlling
        # terminal.  Say which one, and give it its size, before it starts.
        with open(outfile + ".tty", "w") as f:
            f.write(os.ttyname(0))
        fcntl.ioctl(0, termios.TIOCSWINSZ,
                    struct.pack("HHHH", opts["rows"], opts["cols"], 0, 0))
        try:
            os.execvp(cmd[0], cmd)
        except OSError as e:
            os.write(2, ("ptyrun: %s: %s\n" % (cmd[0], e)).encode())
            os._exit(127)

    # A slow reader: the command's output backs up in the terminal while
    # nobody reads it, which is what a stalled or busy emulator looks like.
    time.sleep(opts["pause"])
    out = bytearray()
    pending = opts["send"]
    start = time.time()
    deadline = start + opts["timeout"]
    status = None
    while status is None:
        if time.time() > deadline:
            os.kill(pid, 9)
            os.waitpid(pid, 0)
            status = 124 << 8
            break
        if select.select([master], [], [], 0.1)[0]:
            try:
                data = os.read(master, 65536)
            except OSError:  # the slave is closed everywhere: the command is gone
                data = b""
            out += data
        # --send-after is for a command whose only terminal is stdin, where
        # the marker is written to the pty and discarded: nothing arrives on
        # the master, so this cannot live inside the read above.
        ready = opts["wait"] is not None and opts["wait"] in out
        if (opts["send-after"] is not None and
                time.time() - start >= opts["send-after"]):
            ready = True
        if pending is not None and ready:
            if opts["resize"] is not None:
                # The window changes while the command waits; give
                # whatever relays it time to notice before the input.
                fcntl.ioctl(master, termios.TIOCSWINSZ,
                            struct.pack("HHHH", *opts["resize"], 0, 0))
                time.sleep(1.0)
            os.write(master, pending)
            pending = None
        got, st = os.waitpid(pid, os.WNOHANG)
        if got == pid:
            status = st
    # What the command wrote last may still be in the pty.
    while status != 124 << 8 and select.select([master], [], [], 0.3)[0]:
        try:
            data = os.read(master, 65536)
        except OSError:
            break
        if not data:
            break
        out += data

    with open(outfile, "wb") as f:
        f.write(out)
    if status == 124 << 8:
        sys.exit(124)
    if os.WIFEXITED(status):
        sys.exit(os.WEXITSTATUS(status))
    sys.exit(128 + os.WTERMSIG(status) if os.WIFSIGNALED(status) else 1)


main()
