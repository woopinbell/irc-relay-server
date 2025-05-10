#!/usr/bin/env python3
"""Small IRC smoke client for the educational reference server."""

import socket
import sys
import time
from typing import List


class IrcPeer:
    def __init__(self, host: str, port: int, label: str):
        self.label = label
        self.sock = socket.create_connection((host, port), timeout=3.0)
        self.sock.settimeout(0.2)
        self.buffer = b""
        self.lines: List[str] = []

    def send_line(self, line: str) -> None:
        self.sock.sendall(line.encode("utf-8") + b"\r\n")

    def send_raw(self, data: bytes) -> None:
        self.sock.sendall(data)

    def read_available(self, duration: float = 0.05) -> List[str]:
        deadline = time.time() + duration
        while time.time() < deadline:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            self.buffer += chunk
            while b"\n" in self.buffer:
                raw, self.buffer = self.buffer.split(b"\n", 1)
                line = raw.rstrip(b"\r").decode("utf-8", "replace")
                self.lines.append(line)
        return list(self.lines)

    def expect(self, needle: str, timeout: float = 2.0) -> str:
        deadline = time.time() + timeout
        while time.time() < deadline:
            for index, line in enumerate(self.lines):
                if needle in line:
                    return self.lines.pop(index)
            self.read_available(0.1)
        transcript = "\n".join(self.lines[-20:])
        raise AssertionError(f"{self.label}: expected {needle!r}; recent lines:\n{transcript}")

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def register(host: str, port: int, password: str, nick: str, realname: str) -> IrcPeer:
    peer = IrcPeer(host, port, nick)
    peer.send_line(f"PASS {password}")
    peer.send_line(f"NICK {nick}")
    peer.send_line(f"USER {nick} 0 * :{realname}")
    peer.expect(" 001 ")
    return peer


def main() -> int:
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} <host> <port> <password>", file=sys.stderr)
        return 2

    host = sys.argv[1]
    port = int(sys.argv[2])
    password = sys.argv[3]

    peers: List[IrcPeer] = []
    try:
        wrong = IrcPeer(host, port, "wrong-password")
        wrong.send_line("PASS wrong-password")
        wrong.expect(" 464 ")
        wrong.close()

        alice = register(host, port, password, "alice", "Alice Learner")
        peers.append(alice)

        alice.send_raw(b"PI")
        time.sleep(0.05)
        alice.send_raw(b"NG :half-frame\r\n")
        pong = alice.expect("PONG")
        if "half-frame" not in pong:
            raise AssertionError(f"alice: split PING response did not echo token: {pong}")

        dup = register(host, port, password, "dupe", "Dupe One")
        peers.append(dup)
        collision = IrcPeer(host, port, "collision")
        collision.send_line(f"PASS {password}")
        collision.send_line("NICK dupe")
        collision.expect(" 433 ")
        collision.close()

        alice.send_line("JOIN #edu")
        alice.expect(" JOIN #edu")
        alice.expect(" 353 ")
        alice.expect(" 366 ")
        alice.send_line("LIST #edu")
        alice.expect(" 322 ")
        alice.expect(" 323 ")
        alice.send_line("NAMES #edu")
        alice.expect(" 353 ")
        alice.expect(" 366 ")

        bob = register(host, port, password, "bob", "Bob Learner")
        carol = register(host, port, password, "carol", "Carol Learner")
        peers.extend([bob, carol])

        bob.send_line("JOIN #edu")
        bob.expect(" JOIN #edu")
        alice.expect(":bob!")

        alice.send_line("TOPIC #edu :Protocol lab")
        alice.expect(" TOPIC #edu :Protocol lab")

        alice.send_line("PRIVMSG #edu :hello channel")
        bob.expect(" PRIVMSG #edu :hello channel")

        alice.send_line("INVITE carol #edu")
        alice.expect(" 341 ")
        carol.expect(" INVITE carol #edu")
        carol.send_line("JOIN #edu")
        carol.expect(" JOIN #edu")

        alice.send_line("MODE #edu +i")
        alice.expect(" MODE #edu +i")
        alice.send_line("MODE #edu +o bob")
        bob.expect(" MODE #edu +o bob")

        bob.send_line("TOPIC #edu :Bob can set topics")
        bob.expect(" TOPIC #edu :Bob can set topics")

        bob.send_line("KICK #edu carol :practice complete")
        carol.expect(" KICK #edu carol :practice complete")

        bob.send_line("PART #edu :done")
        bob.expect(" PART #edu done")

        alice.send_line("MODE #edu")
        alice.expect(" 324 ")

        alice.send_line("PRIVMSG bob :direct hello")
        bob.expect(" PRIVMSG bob :direct hello")

        alice.send_line("QUIT :smoke complete")
        time.sleep(0.05)
        return 0
    finally:
        for peer in peers:
            peer.close()


if __name__ == "__main__":
    raise SystemExit(main())
