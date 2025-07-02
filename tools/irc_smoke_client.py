#!/usr/bin/env python3
"""Small IRC smoke client for irc-relay-server."""

import re
import socket
import sys
import time
from typing import List, Tuple


class IrcPeer:
    def __init__(self, host: str, port: int, label: str, auto_pong: bool = True):
        self.label = label
        self.auto_pong = auto_pong
        self.sock = socket.create_connection((host, port), timeout=3.0)
        self.sock.settimeout(0.2)
        self.buffer = b""
        self.lines: List[str] = []
        self.endings: List[bytes] = []
        self.closed = False

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
            except OSError:
                self.closed = True
                break
            if not chunk:
                self.closed = True
                break
            self.buffer += chunk
            while b"\n" in self.buffer:
                raw, self.buffer = self.buffer.split(b"\n", 1)
                if raw.endswith(b"\r"):
                    line_bytes = raw[:-1]
                    ending = b"\r\n"
                else:
                    line_bytes = raw
                    ending = b"\n"
                line = line_bytes.decode("utf-8", "replace")
                self.lines.append(line)
                self.endings.append(ending)
                self._auto_reply_to_ping(line)
        return list(self.lines)

    def expect(self, needle: str, timeout: float = 2.0) -> str:
        deadline = time.time() + timeout
        while time.time() < deadline:
            for index, line in enumerate(self.lines):
                if needle in line:
                    return self._pop_line(index)[0]
            self.read_available(0.1)
        transcript = "\n".join(self.lines[-20:])
        raise AssertionError(f"{self.label}: expected {needle!r}; recent lines:\n{transcript}")

    def expect_exact(self, expected: str, timeout: float = 2.0) -> str:
        deadline = time.time() + timeout
        while time.time() < deadline:
            for index, line in enumerate(self.lines):
                if line == expected:
                    matched, ending = self._pop_line(index)
                    self._require_crlf(matched, ending)
                    return matched
            self.read_available(0.1)
        transcript = "\n".join(self.lines[-20:])
        raise AssertionError(
            f"{self.label}: expected exact frame {expected!r}; recent lines:\n{transcript}"
        )

    def expect_next_exact(self, expected: str, timeout: float = 2.0) -> str:
        deadline = time.time() + timeout
        while time.time() < deadline and not self.lines:
            self.read_available(0.1)
        if not self.lines:
            raise AssertionError(f"{self.label}: expected next frame {expected!r}; no frame received")
        actual, ending = self._pop_line(0)
        self._require_crlf(actual, ending)
        if actual != expected:
            transcript = "\n".join(self.lines[-20:])
            raise AssertionError(
                f"{self.label}: expected next frame {expected!r}, got {actual!r}; "
                f"remaining lines:\n{transcript}"
            )
        return actual

    def expect_regex(self, pattern: str, timeout: float = 2.0) -> str:
        deadline = time.time() + timeout
        while time.time() < deadline:
            for index, line in enumerate(self.lines):
                if re.fullmatch(pattern, line):
                    matched, ending = self._pop_line(index)
                    self._require_crlf(matched, ending)
                    return matched
            self.read_available(0.1)
        transcript = "\n".join(self.lines[-20:])
        raise AssertionError(
            f"{self.label}: expected frame matching {pattern!r}; recent lines:\n{transcript}"
        )

    def wait_closed(self, timeout: float = 3.0) -> None:
        deadline = time.time() + timeout
        while time.time() < deadline and not self.closed:
            self.read_available(0.1)
        if not self.closed:
            raise AssertionError(f"{self.label}: connection did not close within {timeout}s")

    def hostmask(self, nick: str, user: str) -> str:
        address = self.sock.getsockname()
        host = str(address[0])
        port = int(address[1])
        peer = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
        return f"{nick}!{user}@{peer}"

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass
        self.closed = True

    def _pop_line(self, index: int) -> Tuple[str, bytes]:
        line = self.lines.pop(index)
        ending = self.endings.pop(index)
        return line, ending

    def _require_crlf(self, line: str, ending: bytes) -> None:
        if ending != b"\r\n":
            raise AssertionError(
                f"{self.label}: frame {line!r} ended with {ending!r}, expected b'\\r\\n'"
            )

    def _auto_reply_to_ping(self, line: str) -> None:
        if not self.auto_pong:
            return
        token = None
        if " PING " in line:
            token = line.split(" PING ", 1)[1]
        elif line.startswith("PING "):
            token = line.split(" ", 1)[1]
        if token is None:
            return
        token = token.lstrip(":")
        self.send_line(f"PONG :{token}")


def register(host: str, port: int, password: str, nick: str, realname: str) -> IrcPeer:
    peer = IrcPeer(host, port, nick)
    peer.send_line(f"PASS {password}")
    peer.send_line(f"NICK {nick}")
    peer.send_line(f"USER {nick} 0 * :{realname}")
    peer.expect_next_exact(
        f":irc.relay.local 001 {nick} :Welcome to irc-relay-server, {nick}"
    )
    peer.expect_next_exact(f":irc.relay.local 002 {nick} :Your host is irc.relay.local")
    peer.expect_next_exact(
        f":irc.relay.local 003 {nick} :This server is running a C++17 event backend"
    )
    return peer


def metrics_pattern(nick: str) -> str:
    return (
        rf":irc\.relay\.local NOTICE {re.escape(nick)} :"
        r"connections=\d+ accepted=\d+ closed=\d+ rooms=\d+ commands=\d+ "
        r"messages=\d+ queue_drops=\d+ rate_limited=\d+"
    )


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
        wrong.expect_next_exact(":irc.relay.local 464 * :Password incorrect")
        wrong.close()

        alice = register(host, port, password, "alice", "Alice Learner")
        peers.append(alice)

        alice.send_raw(b"PI")
        time.sleep(0.05)
        alice.send_raw(b"NG :half-frame\r\n")
        alice.expect_next_exact(":irc.relay.local PONG irc.relay.local half-frame")

        dup = register(host, port, password, "dupe", "Dupe One")
        peers.append(dup)
        collision = IrcPeer(host, port, "collision")
        collision.send_line(f"PASS {password}")
        collision.send_line("NICK dupe")
        collision.expect_next_exact(":irc.relay.local 433 * dupe :Nickname is already in use")
        collision.close()

        alice_prefix = alice.hostmask("alice", "alice")
        alice.send_line("JOIN #edu")
        alice.expect_next_exact(f":{alice_prefix} JOIN #edu")
        alice.expect_next_exact(":irc.relay.local 331 alice #edu :No topic is set")
        alice.expect_next_exact(":irc.relay.local 353 alice = #edu @alice")
        alice.expect_next_exact(":irc.relay.local 366 alice #edu :End of /NAMES list")
        alice.send_line("LIST #edu")
        alice.expect_next_exact(":irc.relay.local 321 alice Channel Users Name")
        alice.expect_next_exact(":irc.relay.local 322 alice #edu 1 :open room")
        alice.expect_next_exact(":irc.relay.local 323 alice :End of /LIST")
        alice.send_line("NAMES #edu")
        alice.expect_next_exact(":irc.relay.local 353 alice = #edu @alice")
        alice.expect_next_exact(":irc.relay.local 366 alice #edu :End of /NAMES list")

        bob = register(host, port, password, "bob", "Bob Learner")
        carol = register(host, port, password, "carol", "Carol Learner")
        peers.extend([bob, carol])

        bob_prefix = bob.hostmask("bob", "bob")
        bob.send_line("JOIN #edu")
        bob.expect_exact(f":{bob_prefix} JOIN #edu")
        alice.expect_exact(f":{bob_prefix} JOIN #edu")

        alice.send_line("TOPIC #edu :Protocol lab")
        alice.expect_exact(f":{alice_prefix} TOPIC #edu :Protocol lab")

        alice.send_line("PRIVMSG #edu :hello channel")
        bob.expect_exact(f":{alice_prefix} PRIVMSG #edu :hello channel")

        alice.send_line("INVITE carol #edu")
        alice.expect_exact(":irc.relay.local 341 alice carol #edu")
        carol.expect_exact(f":{alice_prefix} INVITE carol #edu")
        carol.send_line("JOIN #edu")
        carol.expect_exact(f":{carol.hostmask('carol', 'carol')} JOIN #edu")

        alice.send_line("MODE #edu +i")
        alice.expect_exact(f":{alice_prefix} MODE #edu +i")
        alice.send_line("MODE #edu +o bob")
        bob.expect_exact(f":{alice_prefix} MODE #edu +o bob")

        bob.send_line("TOPIC #edu :Bob can set topics")
        bob.expect_exact(f":{bob_prefix} TOPIC #edu :Bob can set topics")

        bob.send_line("KICK #edu carol :practice complete")
        carol.expect_exact(f":{bob_prefix} KICK #edu carol :practice complete")

        bob.send_line("PART #edu :done")
        bob.expect_exact(f":{bob_prefix} PART #edu done")

        alice.send_line("MODE #edu")
        alice.expect_exact(":irc.relay.local 324 alice #edu +it")

        alice.send_line("PRIVMSG bob :direct hello")
        bob.expect_exact(f":{alice_prefix} PRIVMSG bob :direct hello")

        alice.send_line("METRICS")
        alice.expect_regex(metrics_pattern("alice"))

        bots: List[IrcPeer] = []
        for index in range(6):
            bot = register(host, port, password, f"bot{index}", f"Bot {index}")
            bot.send_line("JOIN #load")
            bot.expect_exact(f":{bot.hostmask(f'bot{index}', f'bot{index}')} JOIN #load")
            bots.append(bot)
        peers.extend(bots)
        bots[0].send_line("PRIVMSG #load :load hello")
        bots[1].expect_exact(
            f":{bots[0].hostmask('bot0', 'bot0')} PRIVMSG #load :load hello"
        )

        flood = register(host, port, password, "flood", "Flood Tester")
        for index in range(25):
            flood.send_line(f"PING :burst-{index}")
        flood.expect_exact(":irc.relay.local 439 flood :Command rate limit exceeded")
        flood.close()

        alice.send_line("QUIT :smoke complete")
        time.sleep(0.05)

        idle = register(host, port, password, "idle", "Idle Tester")
        peers.append(idle)
        idle.expect_regex(r":irc\.relay\.local PING heartbeat-\d+-\d+", timeout=5.0)
        idle.send_line("METRICS")
        idle.expect_regex(metrics_pattern("idle"))

        time.sleep(0.05)
        return 0
    finally:
        for peer in peers:
            peer.close()


if __name__ == "__main__":
    raise SystemExit(main())
