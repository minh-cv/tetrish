#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "usage: $0 TETRISD TETRISLOGD PROBE OPENSSL" >&2
    exit 2
fi

tetrisd=$1
tetrislogd=$2
probe=$3
openssl_bin=$4
test_root=$(mktemp -d)
server_pid=
logd_pid=

cleanup() {
    if [[ -n ${server_pid} ]]; then
        kill -TERM "${server_pid}" 2>/dev/null || true
        wait "${server_pid}" 2>/dev/null || true
    fi
    if [[ -n ${logd_pid} ]]; then
        kill -TERM "${logd_pid}" 2>/dev/null || true
        wait "${logd_pid}" 2>/dev/null || true
    fi
    rm -rf "${test_root}"
}
trap cleanup EXIT

"${openssl_bin}" req -x509 -newkey rsa:2048 -nodes \
    -keyout "${test_root}/ca.key" -out "${test_root}/ca.crt" \
    -days 1 -subj /CN=tetrish-system-test-ca \
    -addext basicConstraints=critical,CA:TRUE >/dev/null 2>&1
"${openssl_bin}" req -newkey rsa:2048 -nodes \
    -keyout "${test_root}/server.key" -out "${test_root}/server.csr" \
    -subj /CN=localhost >/dev/null 2>&1
"${openssl_bin}" x509 -req -in "${test_root}/server.csr" \
    -CA "${test_root}/ca.crt" -CAkey "${test_root}/ca.key" \
    -CAcreateserial -out "${test_root}/server.crt" -days 1 >/dev/null 2>&1

port=$((24000 + $$ % 16000))
for attempt in $(seq 0 99); do
    candidate=$((port + attempt))
    if ! (exec 9<>"/dev/tcp/127.0.0.1/${candidate}") 2>/dev/null; then
        port=${candidate}
        break
    fi
done

cat >"${test_root}/.tetrishrc" <<EOF
listen_port=${port}
address=127.0.0.1
cert_path=server.crt
key_path=server.key
ca_path=ca.crt
log_ipc=tetrislogd.sock
log_path=tetrish.log
max_fds=256
max_events=64
client_capacity=8
state_push_interval_ms=50
logger_reconnect_seconds=1
logd_max_clients=8
EOF

# Start the game server first: it must remain available while logging is down.
PROJECT_DIR="${test_root}" TETRISH_FOREGROUND=1 \
    "${tetrisd}" >"${test_root}/server.stderr" 2>&1 &
server_pid=$!
server_ready=false
for _ in $(seq 1 100); do
    if (exec 9<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then
        exec 9>&-
        server_ready=true
        break
    fi
    kill -0 "${server_pid}" 2>/dev/null || {
        cat "${test_root}/server.stderr" >&2
        exit 1
    }
    sleep 0.02
done
if [[ ${server_ready} != true ]]; then
    echo "tetrisd did not listen on ${port}" >&2
    exit 1
fi

# Bring logging up afterwards and give tetrisd one configured retry interval.
PROJECT_DIR="${test_root}" TETRISH_FOREGROUND=1 \
    "${tetrislogd}" >"${test_root}/logd.stderr" 2>&1 &
logd_pid=$!
for _ in $(seq 1 100); do
    [[ -S ${test_root}/tetrislogd.sock ]] && break
    kill -0 "${logd_pid}" 2>/dev/null || {
        cat "${test_root}/logd.stderr" >&2
        exit 1
    }
    sleep 0.02
done
[[ -S ${test_root}/tetrislogd.sock ]]
sleep 1.2

"${probe}" 127.0.0.1 "${port}" "${test_root}/ca.crt"

kill -TERM "${server_pid}"
wait "${server_pid}"
server_pid=
kill -TERM "${logd_pid}"
wait "${logd_pid}"
logd_pid=

grep -q 'renamed to reference-player' "${test_root}/tetrish.log"
