#!/usr/bin/env python3
"""Local 10k x 128 write/query benchmark for MiniVectorDB."""
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("BIN", os.path.join(ROOT, "build", "minivectordb"))

DIM = 128
N = 10000
BATCH = 100
QUERIES = 1000
K = 10


def vec_for(seed):
    return ",".join(f"{((seed * 131 + i * 17) % 2000) / 100.0:.2f}" for i in range(DIM))


def main():
    if not os.path.isfile(BIN) or not os.access(BIN, os.X_OK):
        print(f"binary not found: {BIN} (build first or set BIN=...)", file=sys.stderr)
        return 1

    tmp = tempfile.mkdtemp(prefix="mvdb-bench-")
    try:
        db = os.path.join(tmp, "db")
        write_sql = os.path.join(tmp, "write.sql")
        query_sql = os.path.join(tmp, "query.sql")

        with open(write_sql, "w", encoding="utf-8") as f:
            f.write("CREATE TABLE bench (id BIGINT PRIMARY KEY, grp BIGINT, vec VECTOR(128));\n")
            for b in range(N // BATCH):
                rows = []
                for j in range(BATCH):
                    rid = b * BATCH + j + 1
                    rows.append(f"({rid},{rid % 100},'[{vec_for(rid)}]')")
                f.write("INSERT INTO bench VALUES " + ",".join(rows) + ";\n")

        with open(query_sql, "w", encoding="utf-8") as f:
            for q in range(QUERIES):
                v = vec_for(q * 7 + 3)
                if q % 2:
                    f.write(
                        f"SELECT id, L2_DISTANCE(vec, '[{v}]') AS d FROM bench "
                        f"WHERE grp = {q % 100} ORDER BY d LIMIT {K};\n"
                    )
                else:
                    f.write(
                        f"SELECT id, L2_DISTANCE(vec, '[{v}]') AS d FROM bench "
                        f"ORDER BY d LIMIT {K};\n"
                    )

        t0 = time.perf_counter()
        with open(write_sql, "rb") as fin:
            subprocess.run(
                [BIN, "--data", db],
                stdin=fin,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=True,
            )
        write_s = time.perf_counter() - t0

        p = subprocess.Popen(
            [BIN, "--data", db],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        ready = p.stdout.readline()
        with open(query_sql, "rb") as f:
            qbytes = f.read()
        t0 = time.perf_counter()
        out, _ = p.communicate(input=qbytes)
        query_s = time.perf_counter() - t0
        if p.returncode != 0:
            print(f"query process failed: {p.returncode}", file=sys.stderr)
            return 1

        print(f"ready: {ready.decode(errors='replace').strip()}")
        print(f"write: {N} rows x {DIM} dims, {N // BATCH} batches -> {write_s:.3f} s")
        print(
            f"query: {QUERIES} top-{K} queries -> {query_s:.3f} s, "
            f"{out.count(chr(10).encode())} output lines"
        )
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
