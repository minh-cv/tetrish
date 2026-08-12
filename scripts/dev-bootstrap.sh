#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "${script_dir}/.." && pwd)
auth_dir="${project_dir}/var/dev-auth"
config_path="${project_dir}/.tetrishrc"

mkdir -p "${auth_dir}" "${project_dir}/var"

if [[ ! -f ${auth_dir}/ca.crt || ! -f ${auth_dir}/ca.key ]]; then
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "${auth_dir}/ca.key" \
        -out "${auth_dir}/ca.crt" \
        -days 30 \
        -subj /CN=tetrish-development-ca \
        -addext basicConstraints=critical,CA:TRUE
fi

if [[ ! -f ${auth_dir}/server.crt || ! -f ${auth_dir}/server.key ]]; then
    openssl req -newkey rsa:2048 -nodes \
        -keyout "${auth_dir}/server.key" \
        -out "${auth_dir}/server.csr" \
        -subj /CN=localhost
    openssl x509 -req \
        -in "${auth_dir}/server.csr" \
        -CA "${auth_dir}/ca.crt" \
        -CAkey "${auth_dir}/ca.key" \
        -CAcreateserial \
        -out "${auth_dir}/server.crt" \
        -days 30
fi

chmod 600 "${auth_dir}/ca.key" "${auth_dir}/server.key"

if [[ ! -f ${config_path} ]]; then
    cp "${project_dir}/.tetrishrc.example" "${config_path}"
    echo "created ${config_path}"
else
    echo "leaving existing ${config_path} unchanged"
fi

echo "development credentials are ready under ${auth_dir}"
