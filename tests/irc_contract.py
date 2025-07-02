#!/usr/bin/env python3
"""Characterize the public CLI, IRC wire, and shutdown contracts."""

import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
from typing import Dict, List, Optional


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from irc_smoke_client import IrcPeer, metrics_pattern, register  # noqa: E402


SERVER_NAME = "irc.relay.local"


def fail(message: str) -> None:
    raise AssertionError(message)


def check_cli(
    manifest: Dict[str, object],
    binary: str,
    label: str,
    arguments: List[str],
    expected_stderr: Optional[str] = None,
    stderr_prefix: Optional[str] = None,
) -> None:
    completed = subprocess.run(
        [binary] + arguments,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 1:
        fail(f"CLI {label}: expected exit 1, got {completed.returncode}")
    if completed.stdout != "":
        fail(f"CLI {label}: expected empty stdout, got {completed.stdout!r}")
    if expected_stderr is not None and completed.stderr != expected_stderr:
        fail(
            f"CLI {label}: stderr mismatch\n"
            f"expected: {expected_stderr!r}\nactual:   {completed.stderr!r}"
        )
    if stderr_prefix is not None:
        allowed_stderr = (
            stderr_prefix + "\n",
            stderr_prefix + ": Invalid argument\n",
        )
        if completed.stderr not in allowed_stderr:
            fail(
                f"CLI {label}: stderr was outside the same-platform contract; "
                f"expected one of {allowed_stderr!r}, got {completed.stderr!r}"
            )

    cli_manifest = manifest["cli"]
    if not isinstance(cli_manifest, dict):
        fail("internal manifest error: cli entry is not a mapping")
    cli_manifest[label] = {
        "arguments": arguments,
        "exit": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def check_cli_contract(manifest: Dict[str, object], binary: str) -> None:
    usage = (
        f"Usage: {binary} <port> <password> "
        "[--idle-timeout=N] [--ping-timeout=N] [--registration-timeout=N] "
        "[--rate-limit=COUNT:SECONDS] [--max-pending-bytes=N] "
        "[--max-connections=N]\n"
    )
    check_cli(manifest, binary, "usage", [], expected_stderr=usage)
    check_cli(
        manifest,
        binary,
        "invalid_port",
        ["0", "contract-secret"],
        expected_stderr="irc-relay-server: port must be an integer from 1 to 65535\n",
    )
    check_cli(
        manifest,
        binary,
        "zero_timeout",
        ["6667", "contract-secret", "--idle-timeout=0"],
        expected_stderr="irc-relay-server: idle timeout must be a positive integer\n",
    )
    check_cli(
        manifest,
        binary,
        "rate_limit_shape",
        ["6667", "contract-secret", "--rate-limit=24"],
        expected_stderr="irc-relay-server: rate limit must use COUNT:SECONDS\n",
    )
    check_cli(
        manifest,
        binary,
        "unknown_option",
        ["6667", "contract-secret", "--unknown=1"],
        expected_stderr="irc-relay-server: unknown option: --unknown=1\n",
    )
    check_cli(
        manifest,
        binary,
        "platform_errno_suffix",
        ["6667", "contract-secret", "--max-pending-bytes=abc"],
        stderr_prefix="irc-relay-server: max pending bytes must be an unsigned integer",
    )


def record_exact(
    manifest: Dict[str, object],
    peer: IrcPeer,
    label: str,
    expected: str,
    timeout: float = 2.0,
    next_frame: bool = False,
) -> str:
    if next_frame:
        actual = peer.expect_next_exact(expected, timeout)
    else:
        actual = peer.expect_exact(expected, timeout)
    checks = manifest["wire_checks"]
    if not isinstance(checks, list):
        fail("internal manifest error: wire_checks entry is not a list")
    normalized = re.sub(r"(@(?:\[[^]]+\]|[^ :]+):)\d+", r"\1<port>", expected)
    checks.append({"label": label, "expected": normalized})
    return actual


def record_regex(
    manifest: Dict[str, object],
    peer: IrcPeer,
    label: str,
    pattern: str,
    timeout: float = 2.0,
) -> str:
    actual = peer.expect_regex(pattern, timeout)
    checks = manifest["wire_checks"]
    if not isinstance(checks, list):
        fail("internal manifest error: wire_checks entry is not a list")
    checks.append({"label": label, "pattern": pattern})
    return actual


def register_contract_peer(
    manifest: Dict[str, object],
    host: str,
    port: int,
    password: str,
    nick: str,
) -> IrcPeer:
    peer = register(host, port, password, nick, f"{nick} Contract")
    checks = manifest["wire_checks"]
    if not isinstance(checks, list):
        fail("internal manifest error: wire_checks entry is not a list")
    checks.extend(
        [
            {"label": f"{nick}_001", "expected": f":{SERVER_NAME} 001 {nick} :Welcome to irc-relay-server, {nick}"},
            {"label": f"{nick}_002", "expected": f":{SERVER_NAME} 002 {nick} :Your host is {SERVER_NAME}"},
            {"label": f"{nick}_003", "expected": f":{SERVER_NAME} 003 {nick} :This server is running a C++17 event backend"},
        ]
    )
    return peer


def close_peers(peers: List[IrcPeer]) -> None:
    for peer in peers:
        peer.close()


def check_wire_contract(
    manifest: Dict[str, object], host: str, port: int, password: str
) -> IrcPeer:
    peers: List[IrcPeer] = []
    try:
        pre_registration = IrcPeer(host, port, "contract-pre-registration")
        peers.append(pre_registration)
        pre_registration.send_line("PRIVMSG nobody :blocked")
        record_exact(
            manifest,
            pre_registration,
            "pre_registration_451",
            f":{SERVER_NAME} 451 * :You have not registered",
            next_frame=True,
        )
        pre_registration.send_line("NICK 1contract")
        record_exact(
            manifest,
            pre_registration,
            "invalid_nickname_432",
            f":{SERVER_NAME} 432 * 1contract :Erroneous nickname",
            next_frame=True,
        )
        pre_registration.close()

        wrong_password = IrcPeer(host, port, "contract-wrong-password")
        peers.append(wrong_password)
        wrong_password.send_line("PASS wrong-password")
        record_exact(
            manifest,
            wrong_password,
            "wrong_password_464",
            f":{SERVER_NAME} 464 * :Password incorrect",
            next_frame=True,
        )
        wrong_password.close()

        taken = register_contract_peer(manifest, host, port, password, "cttaken")
        peers.append(taken)
        collision = IrcPeer(host, port, "contract-collision")
        peers.append(collision)
        collision.send_line(f"PASS {password}")
        collision.send_line("NICK CTTAKEN")
        record_exact(
            manifest,
            collision,
            "nickname_collision_433",
            f":{SERVER_NAME} 433 * CTTAKEN :Nickname is already in use",
            next_frame=True,
        )
        collision.close()
        taken.close()

        alpha = register_contract_peer(manifest, host, port, password, "ctalpha")
        beta = register_contract_peer(manifest, host, port, password, "ctbeta")
        gamma = register_contract_peer(manifest, host, port, password, "ctgamma")
        peers.extend([alpha, beta, gamma])
        alpha_prefix = alpha.hostmask("ctalpha", "ctalpha")
        beta_prefix = beta.hostmask("ctbeta", "ctbeta")
        gamma_prefix = gamma.hostmask("ctgamma", "ctgamma")

        alpha.send_raw(b"PI")
        time.sleep(0.05)
        alpha.send_raw(b"NG :contract-token\r\n")
        record_exact(
            manifest,
            alpha,
            "split_ping_pong",
            f":{SERVER_NAME} PONG {SERVER_NAME} contract-token",
            next_frame=True,
        )

        alpha.send_line("JOIN #contract")
        record_exact(manifest, alpha, "join", f":{alpha_prefix} JOIN #contract", next_frame=True)
        record_exact(
            manifest,
            alpha,
            "join_no_topic_331",
            f":{SERVER_NAME} 331 ctalpha #contract :No topic is set",
            next_frame=True,
        )
        record_exact(
            manifest,
            alpha,
            "join_names_353",
            f":{SERVER_NAME} 353 ctalpha = #contract @ctalpha",
            next_frame=True,
        )
        record_exact(
            manifest,
            alpha,
            "join_names_end_366",
            f":{SERVER_NAME} 366 ctalpha #contract :End of /NAMES list",
            next_frame=True,
        )

        alpha.send_line("LIST #contract")
        record_exact(
            manifest,
            alpha,
            "list_start_321",
            f":{SERVER_NAME} 321 ctalpha Channel Users Name",
            next_frame=True,
        )
        record_exact(
            manifest,
            alpha,
            "list_item_322",
            f":{SERVER_NAME} 322 ctalpha #contract 1 :open room",
            next_frame=True,
        )
        record_exact(
            manifest,
            alpha,
            "list_end_323",
            f":{SERVER_NAME} 323 ctalpha :End of /LIST",
            next_frame=True,
        )
        alpha.send_line("NAMES #contract")
        record_exact(
            manifest,
            alpha,
            "names_353",
            f":{SERVER_NAME} 353 ctalpha = #contract @ctalpha",
            next_frame=True,
        )
        record_exact(
            manifest,
            alpha,
            "names_end_366",
            f":{SERVER_NAME} 366 ctalpha #contract :End of /NAMES list",
            next_frame=True,
        )

        beta.send_line("JOIN #contract")
        record_exact(manifest, beta, "beta_join", f":{beta_prefix} JOIN #contract")
        record_exact(manifest, alpha, "beta_join_broadcast", f":{beta_prefix} JOIN #contract")

        alpha.send_line("TOPIC #contract :Contract topic")
        record_exact(
            manifest,
            alpha,
            "topic_broadcast",
            f":{alpha_prefix} TOPIC #contract :Contract topic",
        )
        alpha.send_line("PRIVMSG #contract :channel contract")
        record_exact(
            manifest,
            beta,
            "channel_privmsg",
            f":{alpha_prefix} PRIVMSG #contract :channel contract",
        )

        alpha.send_line("INVITE ctgamma #contract")
        record_exact(
            manifest,
            alpha,
            "invite_numeric_341",
            f":{SERVER_NAME} 341 ctalpha ctgamma #contract",
        )
        record_exact(
            manifest,
            gamma,
            "invite_delivery",
            f":{alpha_prefix} INVITE ctgamma #contract",
        )
        gamma.send_line("JOIN #contract")
        record_exact(manifest, gamma, "gamma_join", f":{gamma_prefix} JOIN #contract")

        alpha.send_line("MODE #contract +i")
        record_exact(manifest, alpha, "mode_invite_only", f":{alpha_prefix} MODE #contract +i")
        alpha.send_line("MODE #contract +o ctbeta")
        record_exact(
            manifest,
            beta,
            "mode_operator",
            f":{alpha_prefix} MODE #contract +o ctbeta",
        )
        beta.send_line("KICK #contract ctgamma :contract complete")
        record_exact(
            manifest,
            gamma,
            "kick",
            f":{beta_prefix} KICK #contract ctgamma :contract complete",
        )

        alpha.send_line("MODE #contract")
        record_exact(
            manifest,
            alpha,
            "mode_query_324",
            f":{SERVER_NAME} 324 ctalpha #contract +it",
        )
        alpha.send_line("PRIVMSG ctbeta :direct contract")
        record_exact(
            manifest,
            beta,
            "direct_privmsg",
            f":{alpha_prefix} PRIVMSG ctbeta :direct contract",
        )
        alpha.send_line("METRICS")
        record_regex(manifest, alpha, "metrics_key_order", metrics_pattern("ctalpha"))

        flood = register_contract_peer(manifest, host, port, password, "ctflood")
        peers.append(flood)
        for index in range(25):
            flood.send_line(f"PING :contract-burst-{index}")
        record_exact(
            manifest,
            flood,
            "rate_limit_439",
            f":{SERVER_NAME} 439 ctflood :Command rate limit exceeded",
        )
        flood.close()

        alpha.close()
        beta.close()
        gamma.close()
        time.sleep(0.2)

        heartbeat = register_contract_peer(manifest, host, port, password, "ctheartbeat")
        peers.append(heartbeat)
        record_regex(
            manifest,
            heartbeat,
            "heartbeat_ping",
            rf":{re.escape(SERVER_NAME)} PING heartbeat-\d+-\d+",
            timeout=5.0,
        )
        heartbeat.send_line("METRICS")
        record_regex(
            manifest,
            heartbeat,
            "heartbeat_pong_then_metrics",
            metrics_pattern("ctheartbeat"),
        )
        heartbeat.close()
        time.sleep(0.2)

        shutdown_peer = register_contract_peer(
            manifest, host, port, password, "ctshutdown"
        )
        peers.append(shutdown_peer)
        return shutdown_peer
    except Exception:
        close_peers(peers)
        raise


def validate_shutdown_log(
    log_path: Path, port: int, timeout: float = 3.0
) -> List[str]:
    deadline = time.time() + timeout
    last_text = ""
    while time.time() < deadline:
        try:
            last_text = log_path.read_text(errors="replace")
        except OSError:
            time.sleep(0.02)
            continue
        lines = last_text.splitlines()

        try:
            started_index = lines.index(f"event=server_started port={port}")
            registered_index = next(
                index
                for index, line in enumerate(lines)
                if re.fullmatch(r"event=client_registered fd=\d+ nick=ctshutdown", line)
            )
            registered = lines[registered_index]
            fd_match = re.search(r"fd=(\d+)", registered)
            if fd_match is None:
                raise ValueError("shutdown registration log has no fd")
            fd = fd_match.group(1)
            connected_index = max(
                index
                for index, line in enumerate(lines[:registered_index])
                if line.startswith(f"event=client_connected fd={fd} peer=")
            )
            metrics_index = next(
                index
                for index, line in enumerate(lines[registered_index + 1 :], registered_index + 1)
                if re.fullmatch(
                    r"event=server_metrics accepted=\d+ closed=\d+ lines=\d+ "
                    r"queue_drops=\d+ commands=\d+ messages=\d+ rooms=\d+ "
                    r"rooms_created=\d+ rate_limited=\d+ idle_timeouts=\d+ "
                    r"heartbeats=\d+",
                    line,
                )
            )
            disconnected_index = next(
                index
                for index, line in enumerate(lines[metrics_index + 1 :], metrics_index + 1)
                if line == f"event=client_disconnected fd={fd} reason=Server_shutting_down"
            )
        except (StopIteration, ValueError):
            time.sleep(0.02)
            continue

        indices = [
            started_index,
            connected_index,
            registered_index,
            metrics_index,
            disconnected_index,
        ]
        if indices != sorted(indices) or len(set(indices)) != len(indices):
            fail(f"shutdown log events are out of order: {indices}")
        return [
            "server_started",
            "client_connected",
            "client_registered",
            "server_metrics",
            "client_disconnected:Server_shutting_down",
        ]

    tail = "\n".join(last_text.splitlines()[-30:])
    fail(f"shutdown log contract was not observed within {timeout}s; log tail:\n{tail}")
    return []


def write_manifest(manifest: Dict[str, object]) -> None:
    target = os.environ.get("IRC_CONTRACT_MANIFEST")
    if not target:
        return
    Path(target).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")


def main() -> int:
    if len(sys.argv) != 7:
        print(
            f"usage: {sys.argv[0]} <binary> <host> <port> <password> <server-pid> <log-file>",
            file=sys.stderr,
        )
        return 2

    binary = sys.argv[1]
    host = sys.argv[2]
    port = int(sys.argv[3])
    password = sys.argv[4]
    server_pid = int(sys.argv[5])
    log_path = Path(sys.argv[6])
    manifest: Dict[str, object] = {
        "cli": {},
        "wire_checks": [],
        "log_order": [],
    }

    check_cli_contract(manifest, binary)
    shutdown_peer = check_wire_contract(manifest, host, port, password)

    os.kill(server_pid, signal.SIGTERM)
    record_exact(
        manifest,
        shutdown_peer,
        "shutdown_error",
        "ERROR :Server shutting down",
        timeout=3.0,
    )
    shutdown_peer.wait_closed(3.0)
    manifest["log_order"] = validate_shutdown_log(log_path, port)
    shutdown_peer.close()
    write_manifest(manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
