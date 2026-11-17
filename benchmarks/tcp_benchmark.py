#!/usr/bin/env python3
"""Synthetic TCP PUT benchmark against an already running local server."""

import argparse
import concurrent.futures
import socket
import time


def worker(host: str, port: int, client_id: int, operations: int) -> int:
    """Issue PUT operations through one persistent TCP connection."""
    with socket.create_connection((host, port), timeout=5) as connection:
        connection.settimeout(10)
        with connection.makefile("rwb", buffering=0) as stream:
            for index in range(operations):
                stream.write(f"PUT load:{client_id}:{index} 0 v{index}\n".encode())
                if stream.readline() != b"OK\n":
                    raise RuntimeError("unexpected PUT response from server")
    return operations


def main() -> None:
    """Run the configured client workload and report its observed rate."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7379)
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--ops-per-client", type=int, default=1000)
    args = parser.parse_args()
    if args.clients < 1 or args.ops_per_client < 1:
        parser.error("clients and ops-per-client must be positive")

    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.clients) as executor:
        futures = [
            executor.submit(worker, args.host, args.port, client, args.ops_per_client)
            for client in range(args.clients)
        ]
        completed = sum(future.result() for future in futures)

    elapsed = time.perf_counter() - started
    rate = completed / elapsed if elapsed > 0 else 0.0
    print("VoltStore TCP PUT microbenchmark")
    print(f"clients={args.clients} operations={completed}")
    print(f"elapsed_seconds={elapsed:.6f} ops_per_sec={rate:.2f}")
    print("Record server fsync mode, build type, hardware, and workload with any result.")


if __name__ == "__main__":
    main()
