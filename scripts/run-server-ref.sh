#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "${script_dir}/.." && pwd)
build_dir=${TETRISH_BUILD_DIR:-"${project_dir}/build"}
logd_pid=
server_pid=

cleanup() {
    if [[ -n ${server_pid} ]]; then
        kill -TERM "${server_pid}" 2>/dev/null || true
        wait "${server_pid}" 2>/dev/null || true
    fi
    if [[ -n ${logd_pid} ]]; then
        kill -TERM "${logd_pid}" 2>/dev/null || true
        wait "${logd_pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

"${script_dir}/dev-bootstrap.sh"
cmake -S "${project_dir}" -B "${build_dir}" -G Ninja \
    -DTETRISH_DEV_MODE=ON -DBUILD_TESTING=ON
cmake --build "${build_dir}" --target tetrisu tetrisd tetrislogd

PROJECT_DIR="${project_dir}" TETRISH_FOREGROUND=1 \
    "${build_dir}/tetrislogd" >"${project_dir}/var/tetrislogd.stderr" 2>&1 &
logd_pid=$!
logd_ready=false
for _ in $(seq 1 100); do
    if [[ -S ${project_dir}/var/tetrislogd.sock ]]; then
        logd_ready=true
        break
    fi
    kill -0 "${logd_pid}" 2>/dev/null || {
        cat "${project_dir}/var/tetrislogd.stderr" >&2
        exit 1
    }
    sleep 0.02
done
if [[ ${logd_ready} != true ]]; then
    echo "tetrislogd did not become ready" >&2
    exit 1
fi

starting_log_lines=$(wc -l <"${project_dir}/var/tetrish.log" 2>/dev/null || echo 0)
PROJECT_DIR="${project_dir}" TETRISH_FOREGROUND=1 \
    "${build_dir}/tetrisd" >"${project_dir}/var/tetrisd.stderr" 2>&1 &
server_pid=$!

server_ready=false
for _ in $(seq 1 100); do
    if tail -n "+$((starting_log_lines + 1))" "${project_dir}/var/tetrish.log" 2>/dev/null |
        grep -q 'listening on'; then
        server_ready=true
        break
    fi
    kill -0 "${server_pid}" 2>/dev/null || {
        cat "${project_dir}/var/tetrisd.stderr" >&2
        exit 1
    }
    sleep 0.02
done
if [[ ${server_ready} != true ]]; then
    echo "tetrisd did not become ready" >&2
    exit 1
fi

PROJECT_DIR="${project_dir}" "${build_dir}/tetrisu"
