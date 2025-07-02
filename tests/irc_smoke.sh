#!/usr/bin/env bash
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${IRC_SMOKE_PORT:-$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)}"
PASSWORD="${IRC_SMOKE_PASSWORD:-relay-secret}"
LOG_FILE="$(mktemp "${TMPDIR:-/tmp}/irc_smoke_server.XXXXXX")"
SERVER_PID=""

cleanup() {
	if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
		kill "${SERVER_PID}" 2>/dev/null || true
		wait "${SERVER_PID}" 2>/dev/null || true
	fi
	rm -f "${LOG_FILE}"
}
trap cleanup EXIT

make -C "${ROOT}" >/dev/null
"${ROOT}/irc-relay-server" "${PORT}" "${PASSWORD}" \
	--idle-timeout=2 \
	--ping-timeout=2 \
	--registration-timeout=5 \
	--rate-limit=24:3 \
	--max-pending-bytes=1048576 \
	>"${LOG_FILE}" 2>&1 &
SERVER_PID="$!"

python3 - <<PY
import socket
import time

host = "127.0.0.1"
port = int("${PORT}")
deadline = time.time() + 5
while time.time() < deadline:
    try:
        with socket.create_connection((host, port), timeout=0.2):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.05)
raise SystemExit("server did not accept connections")
PY

python3 "${ROOT}/tools/irc_smoke_client.py" 127.0.0.1 "${PORT}" "${PASSWORD}"
python3 "${ROOT}/tests/irc_contract.py" \
	"${ROOT}/irc-relay-server" 127.0.0.1 "${PORT}" "${PASSWORD}" \
	"${SERVER_PID}" "${LOG_FILE}"
wait "${SERVER_PID}"
SERVER_PID=""
echo "IRC smoke passed on port ${PORT}"
