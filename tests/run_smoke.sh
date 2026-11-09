#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/minivectordb}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [[ ! -x "$BIN" ]]; then
  echo "binary not found: $BIN (build first or set BIN=...)" >&2
  exit 1
fi

"$BIN" --data "$TMP/db" < "$ROOT/tests/smoke.sql" > "$TMP/out.txt"

expect() {
  if ! grep -Fq "$1" "$TMP/out.txt"; then
    echo "FAIL: missing [$1]" >&2
    echo "----- output -----" >&2
    cat "$TMP/out.txt" >&2
    exit 1
  fi
}

expect "Query OK, 3 rows affected"
expect $'1\ta\t[0,0]'
expect $'2\tb\t[1,0]'
expect $'1\t0.000000'
expect $'2\t1.000000'
expect "ERROR 1062"
expect "ERROR 1406"
expect $'5\te\t[0,0]'

echo "smoke: OK"
