#!/usr/bin/env python3
"""Local crash test over the JSONL protocol (SIGKILL, same data directory)."""
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("BIN", os.path.join(ROOT, "build", "minivectordb"))


class Client:
    def __init__(self, data_dir):
        self.proc = subprocess.Popen(
            [BIN, "--data", data_dir], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", bufsize=1,
        )
        ready = self.proc.stdout.readline()
        if json.loads(ready) != {"type": "ready"}:
            raise RuntimeError(f"bad ready: {ready!r}")
        self.next_id = 1

    def request(self, sql=None, op="execute"):
        ident = self.next_id
        self.next_id += 1
        req = {"id": ident, "op": op}
        if sql is not None:
            req.update(sql=sql, search_mode="exact")
        self.proc.stdin.write(json.dumps(req, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        resp = json.loads(line)
        if resp.get("id") != ident or resp.get("ok") is not True:
            raise RuntimeError(f"bad response: {resp}")
        return resp

    def kill(self):
        self.proc.kill()
        self.proc.wait(timeout=10)

    def shutdown(self):
        self.request(op="shutdown")
        self.proc.stdin.close()
        rc = self.proc.wait(timeout=30)
        self.proc.stderr.read()
        return rc


def main():
    tmp = tempfile.mkdtemp(prefix="mvdb-jsonl-crash-")
    try:
        db = os.path.join(tmp, "db")

        c = Client(db)
        c.request(sql="CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));")
        c.request(sql="INSERT INTO docs VALUES (1, 'a');")
        c.request(sql="BEGIN;")
        c.request(sql="INSERT INTO docs VALUES (2, 'b');")
        c.kill()

        c = Client(db)
        r = c.request(sql="SELECT id FROM docs ORDER BY id;")
        if r.get("rows") != [[1]]:
            raise AssertionError(f"uncommitted row survived: {r}")
        c.shutdown()

        c = Client(db)
        c.request(sql="BEGIN;")
        c.request(sql="INSERT INTO docs VALUES (3, 'c');")
        c.request(sql="COMMIT;")
        c.kill()

        c = Client(db)
        r = c.request(sql="SELECT id FROM docs ORDER BY id;")
        if r.get("rows") != [[1], [3]]:
            raise AssertionError(f"committed row missing: {r}")
        c.shutdown()

        print("crash: OK")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
