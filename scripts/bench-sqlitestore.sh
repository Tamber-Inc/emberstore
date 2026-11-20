#!/usr/bin/env bash
# Starts the sqlitestore demo from a build dir, runs its /bench a few times
# against a fresh data dir, prints each result, then stops the server.
#   scripts/bench-sqlitestore.sh <build-dir> [n] [rounds] [durability]
set -euo pipefail

build_dir=${1:?usage: bench-sqlitestore.sh <build-dir> [n] [rounds] [durability]}
document_count=${2:-2000}
rounds=${3:-3}
durability=${4:-atomic}
port=18081

data_dir=$(mktemp -d)
"$build_dir/Apps/Sqlitestore/sqlitestore" "$port" "$data_dir" > /dev/null &
server_pid=$!
trap 'kill $server_pid 2>/dev/null; rm -rf "$data_dir"' EXIT

until curl -s "localhost:$port/" > /dev/null; do sleep 0.1; done

for _ in $(seq "$rounds"); do
    curl -s -X POST "localhost:$port/bench?n=$document_count&durability=$durability"
    echo
done
