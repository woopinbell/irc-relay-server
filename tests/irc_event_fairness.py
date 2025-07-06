#!/usr/bin/env python3
"""Exercise high file-descriptor counts and non-blocking output isolation."""

from __future__ import annotations

import argparse
import selectors
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Dict, List, Optional


HOST = "127.0.0.1"
PASSWORD = "event-fairness-secret"
PEER_COUNT = 160
FLOOD_FRAME_COUNT = 4096
FLOOD_PAYLOAD = "x" * 400


class Peer:
    def __init__(self, port: int, label: str, receive_buffer: Optional[int] = None):
        self.label = label
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if receive_buffer is not None:
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        self.socket.settimeout(10.0)
        self.socket.connect((HOST, port))
        self.buffer = b""

    def send_line(self, line: str) -> None:
        self.socket.sendall(line.encode("utf-8") + b"\r\n")

    def send_raw(self, data: bytes) -> None:
        self.socket.sendall(data)

    def expect_line(self, expected: str, timeout: float = 10.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while b"\n" in self.buffer:
                raw, self.buffer = self.buffer.split(b"\n", 1)
                if raw.endswith(b"\r"):
                    raw = raw[:-1]
                actual = raw.decode("utf-8", "replace")
                if actual == expected:
                    return
            self.socket.settimeout(max(0.05, deadline - time.monotonic()))
            chunk = self.socket.recv(65536)
            if not chunk:
                raise AssertionError(
                    f"{self.label}: connection closed before {expected!r}"
                )
            self.buffer += chunk
        raise AssertionError(f"{self.label}: did not receive {expected!r}")

    def close(self) -> None:
        try:
            self.socket.close()
        except OSError:
            pass


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as candidate:
        candidate.bind((HOST, 0))
        return int(candidate.getsockname()[1])


def wait_until_listening(process: subprocess.Popen[str], port: int) -> None:
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"server exited during startup with {process.returncode}")
        try:
            with socket.create_connection((HOST, port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.02)
    raise AssertionError("server did not start listening")


def check_many_connections(port: int) -> None:
    peers: List[Peer] = []
    selector = selectors.DefaultSelector()
    buffers: Dict[socket.socket, bytes] = {}
    expected: Dict[socket.socket, bytes] = {}
    try:
        for index in range(PEER_COUNT):
            peer = Peer(port, f"peer-{index}")
            peer.socket.setblocking(False)
            peers.append(peer)
            selector.register(peer.socket, selectors.EVENT_READ)
            buffers[peer.socket] = b""
            expected[peer.socket] = (
                f":irc.relay.local PONG irc.relay.local fanout-{index}".encode("ascii")
            )

        for index, peer in enumerate(peers):
            payload = f"PING :fanout-{index}\r\n".encode("ascii")
            peer.socket.setblocking(True)
            peer.socket.settimeout(10.0)
            peer.socket.sendall(payload)
            peer.socket.setblocking(False)

        pending = set(expected)
        deadline = time.monotonic() + 15.0
        while pending and time.monotonic() < deadline:
            for key, _ in selector.select(timeout=0.25):
                sock = key.fileobj
                if sock not in pending:
                    continue
                chunk = sock.recv(4096)
                if not chunk:
                    raise AssertionError("a fan-out peer closed before receiving PONG")
                buffers[sock] += chunk
                lines = buffers[sock].split(b"\n")
                buffers[sock] = lines.pop()
                if any(line.rstrip(b"\r") == expected[sock] for line in lines):
                    pending.remove(sock)

        if pending:
            labels = [
                peers[index].label
                for index, peer in enumerate(peers)
                if peer.socket in pending
            ]
            raise AssertionError(
                f"{len(pending)} of {PEER_COUNT} peers did not receive PONG: "
                + ", ".join(labels[:10])
            )
    finally:
        selector.close()
        for peer in peers:
            peer.close()


def register(peer: Peer, nick: str) -> None:
    peer.send_raw(
        (
            f"PASS {PASSWORD}\r\n"
            f"NICK {nick}\r\n"
            f"USER {nick} 0 * :{nick} fairness peer\r\n"
        ).encode("ascii")
    )
    peer.expect_line(
        f":irc.relay.local 003 {nick} :This server is running a C++17 event backend"
    )


def check_slow_receiver_isolation(port: int) -> None:
    slow = Peer(port, "slow", receive_buffer=1024)
    sender = Peer(port, "sender")
    probe = Peer(port, "probe")
    try:
        register(slow, "slow")
        register(sender, "sender")
        register(probe, "probe")

        flood = (
            f"PRIVMSG slow :{FLOOD_PAYLOAD}\r\n".encode("ascii")
            * FLOOD_FRAME_COUNT
        )
        sender.send_raw(flood)

        token = "unrelated-connection-still-progresses"
        probe.send_line(f"PING :{token}")
        probe.expect_line(
            f":irc.relay.local PONG irc.relay.local {token}",
            timeout=15.0,
        )
    finally:
        slow.close()
        sender.close()
        probe.close()


def stop_server(process: subprocess.Popen[str]) -> None:
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=10.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5.0)
        raise AssertionError("server did not stop after SIGTERM")
    if process.returncode != 0:
        raise AssertionError(f"server exited with {process.returncode}")


def run(binary: Path) -> None:
    port = reserve_port()
    with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as log:
        process = subprocess.Popen(
            [
                str(binary),
                str(port),
                PASSWORD,
                "--idle-timeout=60",
                "--ping-timeout=10",
                "--registration-timeout=30",
                "--rate-limit=10000:60",
                "--max-pending-bytes=16777216",
                "--max-connections=256",
            ],
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            wait_until_listening(process, port)
            check_many_connections(port)
            check_slow_receiver_isolation(port)
            stop_server(process)
        except BaseException:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5.0)
            log.seek(0)
            output = log.read()
            if output:
                print("server output:", flush=True)
                print(output, flush=True)
            raise


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    run(args.binary.resolve())
    print(
        f"event fairness passed with {PEER_COUNT} simultaneous connections "
        "and one unread recipient"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
