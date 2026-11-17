#!/usr/bin/env python3
"""Interactive client for VoltStore's newline-delimited TCP protocol."""

import argparse
import socket


def main() -> None:
    """Connect to the server and exchange commands in an interactive loop."""
    parser = argparse.ArgumentParser(description="Connect to a local VoltStore server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7379)
    args = parser.parse_args()

    with socket.create_connection((args.host, args.port), timeout=5) as connection:
        connection.settimeout(10)
        with connection.makefile("rwb", buffering=0) as stream:
            print("VoltStore: PUT, GET, DELETE, EXISTS, SCAN, STATS, SNAPSHOT, PING, QUIT")
            while True:
                try:
                    command = input("volt> ")
                except EOFError:
                    break
                if not command:
                    continue

                stream.write((command + "\n").encode())
                response = stream.readline()
                if not response:
                    print("Connection closed by server")
                    break
                first_line = response.decode(errors="replace").rstrip("\r\n")
                print(first_line)

                if first_line.startswith("ITEMS "):
                    while True:
                        row = stream.readline()
                        if not row:
                            raise ConnectionError("server closed during SCAN response")
                        text = row.decode(errors="replace").rstrip("\r\n")
                        print(text)
                        if text == "END":
                            break

                if command.strip() == "QUIT":
                    break


if __name__ == "__main__":
    main()
