#!/usr/bin/env python3
"""Record the first Kitty keyboard events received from a terminal PTY."""

import os
import select
import sys
import termios
import time
import tty


def main() -> int:
    output_path, ready_path = sys.argv[1:3]
    fd = sys.stdin.fileno()
    original = termios.tcgetattr(fd)
    data = bytearray()

    try:
        tty.setraw(fd)
        os.write(sys.stdout.fileno(), b"\x1b[>7u")
        with open(ready_path, "w", encoding="ascii"):
            pass

        deadline = time.monotonic() + 5.0
        while len(data) < 16 and time.monotonic() < deadline:
            readable, _, _ = select.select([fd], [], [], 0.1)
            if readable:
                data.extend(os.read(fd, 64))

        with open(output_path, "w", encoding="ascii") as output:
            output.write(bytes(data).hex())
    finally:
        termios.tcsetattr(fd, termios.TCSANOW, original)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
