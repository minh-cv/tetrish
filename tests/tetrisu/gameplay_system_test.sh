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
    -days 1 -subj /CN=tetrish-gameplay-test-ca \
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

mkdir -p "${test_root}/runtime"
cat >"${test_root}/.tetrishrc" <<EOF
listen_port=${port}
log_path=runtime/tetrisd.log
log_ipc=runtime/tetrislogd.sock
tetrisd_control_ipc=runtime/tetrisd-control.sock
cert_path=server.crt
key_path=server.key
ca_path=ca.crt
tetrisd_address=127.0.0.1
tetrisd_max_events=64
tetrisd_max_fds=256
tetrisd_max_player_fd=240
tetrisd_max_rooms=32
tetrisd_room_tick_hz=60
tetrisd_logger_reconnect_seconds=1
tetrisd_logger_capacity=256
tetrisd_client_capacity=16
tetrislogd_max_clients=8
EOF

PROJECT_DIR="${test_root}" "${tetrislogd}" \
    >"${test_root}/logd.stderr" 2>&1 &
logd_pid=$!
for _ in $(seq 1 100); do
    [[ -S ${test_root}/runtime/tetrislogd.sock ]] && break
    kill -0 "${logd_pid}" 2>/dev/null || {
        cat "${test_root}/logd.stderr" >&2
        exit 1
    }
    sleep 0.02
done
[[ -S ${test_root}/runtime/tetrislogd.sock ]]

PROJECT_DIR="${test_root}" "${tetrisd}" \
    >"${test_root}/server.stderr" 2>&1 &
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

"${probe}" 127.0.0.1 "${port}" "${test_root}/ca.crt"

kill -TERM "${server_pid}"
wait "${server_pid}"
server_pid=
kill -TERM "${logd_pid}"
wait "${logd_pid}"
logd_pid=

for method in CREATE START MOVE ROTATE DROP HOLD LEAVE JOIN; do
    grep -q "request ${method} " "${test_root}/runtime/tetrisd.log"
done
