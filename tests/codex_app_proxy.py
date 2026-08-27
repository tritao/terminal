#!/usr/bin/env python3
"""Run Codex behind a PTY proxy and record terminal negotiation bytes."""

import errno
import os
import pty
import select
import signal
import sys
import termios
import tty


CODEX_PROMPT = "Start and wait for further input. Do not modify files."
PROBE = b"\x1b[?u"
KEYBOARD_PUSH = b"\x1b[>7u"
PROBE_RESPONSE = b"\x1b[?7u"
CAPTURE_LIMIT = 256 * 1024


def append_bytes(destination: bytearray, data: bytes) -> None:
    if len(destination) < CAPTURE_LIMIT:
        destination.extend(data[:CAPTURE_LIMIT - len(destination)])


def write_all(fd: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        try:
            written = os.write(fd, view)
        except OSError as error:
            if error.errno == errno.EINTR:
                continue
            raise
        view = view[written:]


def write_status(
    path: str, output: bytearray, input_data: bytearray, stop_reason: str
) -> None:
    content = (
        f"stop_reason={stop_reason}\n"
        f"codex_output_bytes={len(output)}\n"
        f"terminal_input_bytes={len(input_data)}\n"
        f"codex_probe={int(PROBE in output)}\n"
        f"codex_keyboard_push={int(KEYBOARD_PUSH in output)}\n"
        f"libtsm_probe_response={int(PROBE_RESPONSE in input_data)}\n"
        f"codex_output_hex={bytes(output).hex()}\n"
        f"terminal_input_hex={bytes(input_data).hex()}\n"
    )
    temporary_path = f"{path}.tmp"
    with open(temporary_path, "w", encoding="ascii") as status:
        status.write(content)
    os.replace(temporary_path, path)


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} CODEX_BIN STATUS_PATH", file=sys.stderr)
        return 2

    codex_bin, status_path = sys.argv[1:3]
    output = bytearray()
    input_data = bytearray()
    child_pid = -1
    child_fd = -1
    original_terminal = None
    status_written = False
    stop_reason = "running"

    try:
        child_pid, child_fd = pty.fork()
        if child_pid == 0:
            os.execv(
                codex_bin,
                [
                    codex_bin,
                    "--no-alt-screen",
                    "--sandbox",
                    "read-only",
                    "--ask-for-approval",
                    "never",
                    CODEX_PROMPT,
                ],
            )

        original_terminal = termios.tcgetattr(sys.stdin.fileno())
        tty.setraw(sys.stdin.fileno())
        os.set_blocking(child_fd, False)

        while True:
            stop = False
            readable, _, _ = select.select(
                [sys.stdin.fileno(), child_fd], [], [], 0.1
            )
            for ready_fd in readable:
                try:
                    data = os.read(ready_fd, 4096)
                except OSError as error:
                    if error.errno in (errno.EIO, errno.EBADF):
                        data = b""
                    elif error.errno == errno.EINTR:
                        continue
                    else:
                        raise

                if not data:
                    stop_reason = (
                        "terminal-input-eof"
                        if ready_fd == sys.stdin.fileno()
                        else "codex-output-eof"
                    )
                    stop = True
                    break
                if ready_fd == sys.stdin.fileno():
                    append_bytes(input_data, data)
                    write_all(child_fd, data)
                else:
                    append_bytes(output, data)
                    write_all(sys.stdout.fileno(), data)

                if (
                    not status_written
                    and PROBE in output
                    and KEYBOARD_PUSH in output
                    and PROBE_RESPONSE in input_data
                ):
                    write_status(status_path, output, input_data, "negotiated")
                    status_written = True
                    stop_reason = "negotiated"
                    stop = True
                    break

            if stop:
                break
            waited_pid, _ = os.waitpid(child_pid, os.WNOHANG)
            if waited_pid == child_pid:
                stop_reason = "codex-exited"
                break
    except (OSError, select.error) as error:
        if getattr(error, "errno", None) not in (errno.EIO, errno.EBADF):
            stop_reason = f"error-{getattr(error, 'errno', 'unknown')}"
            print(f"codex app proxy: {error}", file=sys.stderr)
            return 1
    finally:
        if original_terminal is not None:
            termios.tcsetattr(
                sys.stdin.fileno(), termios.TCSANOW, original_terminal
            )
        if child_pid > 0:
            try:
                os.kill(child_pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(child_pid, 0)
            except ChildProcessError:
                pass
        if status_path:
            write_status(status_path, output, input_data, stop_reason)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
