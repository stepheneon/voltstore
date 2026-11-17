#!/usr/bin/env python3
"""Process-kill recovery and graceful-shutdown checks over real loopback sockets."""

from __future__ import annotations

import argparse
import selectors
import signal
import socket
import subprocess
import tempfile
from pathlib import Path


def start_server(binary: str, directory: Path) -> tuple[subprocess.Popen[str], int]:
    """Launch a server on an ephemeral port and read its readiness message."""
    process = subprocess.Popen(
        [binary, "--data-dir", str(directory), "--port", "0", "--threads", "2"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    try:
        assert process.stdout is not None
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            if not selector.select(timeout=10):
                raise RuntimeError("server did not become ready within 10 seconds")
        announcement = process.stdout.readline().strip()
        if not announcement.startswith("READY "):
            raise RuntimeError(f"unexpected server startup response: {announcement!r}")
        return process, int(announcement.rsplit(":", 1)[1])
    except BaseException:
        process.kill()
        process.communicate(timeout=5)
        raise


def command(port: int, text: str) -> str:
    """Send one command and return the first response line."""
    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        connection.sendall((text + "\n").encode())
        stream = connection.makefile("rb")
        try:
            return stream.readline().decode().rstrip("\r\n")
        finally:
            stream.close()


def stop_server(process: subprocess.Popen[str], abruptly: bool) -> None:
    """Terminate a server cleanly or abruptly and collect its exit status."""
    if process.poll() is None:
        if abruptly:
            process.kill()
        else:
            process.send_signal(signal.SIGTERM)
        process.communicate(timeout=5)
    if not abruptly and process.returncode != 0:
        raise AssertionError(f"graceful server shutdown returned {process.returncode}")


def main(binary: str) -> None:
    """Verify that WAL recovery survives process loss and orderly shutdown."""
    with tempfile.TemporaryDirectory(prefix="voltstore-crash-recovery-") as directory:
        data_directory = Path(directory)
        server, port = start_server(binary, data_directory)
        try:
            assert command(port, "PUT stable 0 record") == "OK"
            assert command(port, "GET stable") == "VALUE 6 record"
        finally:
            # SIGKILL bypasses shutdown; recovery must use the WAL.
            stop_server(server, abruptly=True)

        server, port = start_server(binary, data_directory)
        try:
            assert command(port, "GET stable") == "VALUE 6 record"
            assert command(port, "PING") == "PONG"
            # An idle client must not prevent timely shutdown.
            with socket.create_connection(("127.0.0.1", port), timeout=3):
                stop_server(server, abruptly=False)
        finally:
            if server.poll() is None:
                server.kill()
                server.communicate(timeout=5)

        print("crash recovery and graceful server shutdown PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("server_binary")
    args = parser.parse_args()
    main(args.server_binary)
