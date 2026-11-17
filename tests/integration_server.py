#!/usr/bin/env python3
"""End-to-end protocol, multi-client and snapshot checks over loopback TCP."""

from __future__ import annotations

import concurrent.futures
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import tempfile


def read_responses(port: int, commands: list[str]) -> list[str]:
    """Send sequential commands, collecting one response header per command."""
    with socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
        connection.settimeout(5)
        responses: list[str] = []
        with connection.makefile("rwb", buffering=0) as stream:
            for command in commands:
                stream.write((command + "\n").encode())
                response = stream.readline()
                if not response:
                    raise ConnectionError("connection closed before response")
                first = response.decode().rstrip("\r\n")
                responses.append(first)

                if first.startswith("ITEMS "):
                    while True:
                        row = stream.readline()
                        if not row:
                            raise ConnectionError("connection closed during SCAN response")
                        if row.rstrip(b"\r\n") == b"END":
                            break
    return responses


def scan_rows(port: int, prefix: str) -> list[tuple[str, str]]:
    """Read and validate a complete multirow SCAN response."""
    with socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
        connection.settimeout(5)
        connection.sendall((f"SCAN {prefix} 10\n").encode())
        with connection.makefile("rb") as stream:
            header = stream.readline().decode().strip()
            assert header.startswith("ITEMS "), header
            rows = []
            for _ in range(int(header.split()[1])):
                row = stream.readline().decode().rstrip("\n")
                key, escaped_value = row.split("\t", 1)
                rows.append((key, escaped_value))
            assert stream.readline() == b"END\n"
        return rows


def main(server_executable: str) -> None:
    """Exercise TCP commands, concurrent clients, directory locking, and snapshots."""
    with tempfile.TemporaryDirectory(prefix="voltstore-integration-") as directory:
        process = subprocess.Popen(
            [server_executable, "--data-dir", directory, "--port", "0",
             "--threads", "8", "--no-fsync"],
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
                    raise TimeoutError("server did not emit readiness within 10 seconds")
            announcement = process.stdout.readline().strip()
            if not announcement.startswith("READY "):
                raise RuntimeError(f"unexpected readiness output: {announcement!r}")
            port = int(announcement.rsplit(":", 1)[1])

            assert read_responses(port, ["PING"]) == ["PONG"]
            assert read_responses(port, ["PUT hello 0 world", "GET hello"]) == [
                "OK", "VALUE 5 world"
            ]

            def client_work(index: int) -> str:
                """Write and immediately read a client-specific key."""
                responses = read_responses(
                    port, [f"PUT client:{index} 0 value-{index}", f"GET client:{index}"]
                )
                return responses[1]

            with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
                values = list(executor.map(client_work, range(40)))
            assert all(value.startswith("VALUE ") for value in values)
            assert "live_keys=41" in read_responses(port, ["STATS"])[0]
            special_value = "left\tright\\path\rreturn"
            assert read_responses(port, [f"PUT special:1 0 {special_value}"]) == ["OK"]
            assert scan_rows(port, "special:") == [
                ("special:1", r"left\tright\\path\rreturn")
            ]
            assert read_responses(port, ["GET special:1"]) == [
                f"VALUE {len(special_value)} {special_value}"
            ]
            # Competing server must fail before opening the shared WAL.
            contender = subprocess.run(
                [server_executable, "--data-dir", directory, "--port", "0"],
                capture_output=True, text=True, timeout=5,
            )
            assert contender.returncode != 0
            assert "already in use" in contender.stderr
            assert read_responses(port, ["PING"]) == ["PONG"]
            assert read_responses(port, ["SNAPSHOT"]) == ["SNAPSHOT_OK"]

            # A failed snapshot returns a generic error and leaves the connection usable.
            obstruction = Path(directory) / "snapshot.vlt.tmp"
            obstruction.mkdir()
            try:
                assert read_responses(port, ["SNAPSHOT", "PING"]) == [
                    "ERR storage operation failed", "PONG"
                ]
            finally:
                obstruction.rmdir()
            assert read_responses(port, ["SNAPSHOT"]) == ["SNAPSHOT_OK"]
            print(
                "TCP integration: multi-client, directory lock, "
                "SCAN escapes, snapshot errors PASS"
            )
        finally:
            process.terminate()
            try:
                _, stderr = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                _, stderr = process.communicate(timeout=5)
            if process.returncode not in (0, -15):
                sys.stderr.write(stderr)


if __name__ == "__main__":
    main(sys.argv[1])
