#!/usr/bin/env python3
"""Local JSONL benchmark: 10k x 128, 1000 top-10 queries."""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("BIN", os.path.join(ROOT, "build", "minivectordb"))
TIME_BIN = "/usr/bin/time" if os.path.exists("/usr/bin/time") else None
DIM = 128
N = 10000
BATCH = 100
QUERIES = 1000
K = 10


def vec_for(seed):
    return ",".join(f"{((seed * 131 + i * 17) % 2000) / 100.0:.2f}" for i in range(DIM))


class Client:
    def __init__(self, data_dir, no_heap=False, measure_rss=False):
        cmd = [BIN, "--data", data_dir]
        if no_heap:
            cmd.append("--no-heap")
        if measure_rss and TIME_BIN:
            cmd = [TIME_BIN, "-v"] + cmd
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1,
        )
        ready = self.proc.stdout.readline()
        if not ready:
            raise RuntimeError("no ready line")
        if json.loads(ready) != {"type": "ready"}:
            raise RuntimeError(f"bad ready: {ready!r}")
        self.next_id = 1

    def request(self, sql=None, mode="exact", op="execute"):
        ident = self.next_id
        self.next_id += 1
        req = {"id": ident, "op": op}
        if sql is not None:
            req.update(sql=sql, search_mode=mode)
        self.proc.stdin.write(json.dumps(req, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("no response")
        resp = json.loads(line)
        if resp.get("id") != ident or resp.get("ok") is not True:
            raise RuntimeError(f"bad response: {resp}")
        return resp

    def shutdown(self):
        self.request(op="shutdown")
        self.proc.stdin.close()
        rc = self.proc.wait(timeout=60)
        err = self.proc.stderr.read()
        return rc, err


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-heap", action="store_true")
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--queries", type=int, default=1000)
    args = ap.parse_args()
    K = args.k
    QUERIES = args.queries
    if not os.path.isfile(BIN) or not os.access(BIN, os.X_OK):
        print(f"binary not found: {BIN}", file=sys.stderr)
        return 1

    tmp = tempfile.mkdtemp(prefix="mvdb-jsonl-bench-")
    try:
        db = os.path.join(tmp, "db")
        c = Client(db, no_heap=args.no_heap, measure_rss=True)
        c.request(sql="CREATE TABLE bench (id BIGINT PRIMARY KEY, grp BIGINT, vec VECTOR(128));")

        t0 = time.perf_counter()
        for b in range(N // BATCH):
            rows = []
            for j in range(BATCH):
                rid = b * BATCH + j + 1
                rows.append(f"({rid},{rid % 100},'[{vec_for(rid)}]')")
            c.request(sql="INSERT INTO bench VALUES " + ",".join(rows) + ";")
        write_s = time.perf_counter() - t0

        t0 = time.perf_counter()
        for q in range(QUERIES):
            v = vec_for(q * 7 + 3)
            filtered = (q % 2 == 1)
            if filtered:
                sql = (f"SELECT id, L2_DISTANCE(vec, '[{v}]') AS d FROM bench "
                       f"WHERE grp = {q % 100} ORDER BY d LIMIT {K};")
            else:
                sql = (f"SELECT id, L2_DISTANCE(vec, '[{v}]') AS d FROM bench "
                       f"ORDER BY d LIMIT {K};")
            resp = c.request(sql=sql)
            expected = min(K, 100) if filtered else min(K, N)
            if len(resp.get("rows", [])) != expected:
                raise RuntimeError(f"query returned {len(resp.get('rows', []))} rows")
        query_s = time.perf_counter() - t0

        rc, err = c.shutdown()
        rss_mb = None
        for line in err.splitlines():
            if "Maximum resident set size" in line:
                try:
                    rss_mb = int(line.split(":")[-1].strip()) / 1024.0
                except ValueError:
                    pass
        print(f"mode: {'no-heap' if args.no_heap else 'heap'}")
        print(f"write: {N} rows, {N // BATCH} batches -> {write_s:.3f} s")
        print(f"query: {QUERIES} top-{K} -> {query_s:.3f} s")
        if rss_mb is not None:
            print(f"peak RSS: {rss_mb:.1f} MiB")
        return 0 if rc == 0 else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
