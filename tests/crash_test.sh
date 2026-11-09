#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/minivectordb}"
TMP="$(mktemp -d)"
PID=""

cleanup() {
  if [[ -n "$PID" ]] && kill -0 "$PID" 2>/dev/null; then
    kill -9 "$PID" 2>/dev/null || true
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

if [[ ! -x "$BIN" ]]; then
  echo "binary not found: $BIN (build first or set BIN=...)" >&2
  exit 1
fi

# Phase 1: auto-commit row 1, start a transaction and insert row 2, then kill.
mkfifo "$TMP/f1"
"$BIN" --data "$TMP/db" < "$TMP/f1" > "$TMP/out1" 2>"$TMP/err1" &
PID=$!
exec 3>"$TMP/f1"
printf "CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(10), vec VECTOR(2));\n" >&3
printf "INSERT INTO t VALUES (1,'a','[0,0]');\n" >&3
printf "BEGIN;\n" >&3
printf "INSERT INTO t VALUES (2,'b','[1,0]');\n" >&3
sleep 1
kill -9 "$PID" 2>/dev/null || true
exec 3>&-
wait "$PID" 2>/dev/null || true
PID=""

"$BIN" --data "$TMP/db" > "$TMP/check1" <<'SQL'
SELECT * FROM t ORDER BY id;
SQL

if ! grep -Fq $'1\ta' "$TMP/check1"; then
  echo "FAIL: committed row 1 missing after crash" >&2
  cat "$TMP/check1" >&2
  exit 1
fi
if grep -Fq $'2\tb' "$TMP/check1"; then
  echo "FAIL: uncommitted row 2 survived crash" >&2
  cat "$TMP/check1" >&2
  exit 1
fi

# Phase 2: commit row 3, kill immediately after COMMIT, then reopen.
mkfifo "$TMP/f2"
"$BIN" --data "$TMP/db" < "$TMP/f2" > "$TMP/out2" 2>"$TMP/err2" &
PID=$!
exec 3>"$TMP/f2"
printf "BEGIN;\n" >&3
printf "INSERT INTO t VALUES (3,'c','[2,0]');\n" >&3
printf "COMMIT;\n" >&3
sleep 1
kill -9 "$PID" 2>/dev/null || true
exec 3>&-
wait "$PID" 2>/dev/null || true
PID=""

"$BIN" --data "$TMP/db" > "$TMP/check2" <<'SQL'
SELECT * FROM t ORDER BY id;
SQL

if ! grep -Fq $'3\tc' "$TMP/check2"; then
  echo "FAIL: committed row 3 missing after crash" >&2
  cat "$TMP/check2" >&2
  exit 1
fi

echo "crash: OK"
