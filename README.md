# MiniVectorDB（2026-competition-round2）

单机、单进程、单客户端的轻量向量数据库。Linux x86_64 容器前台运行，通过 **JSONL stdin/stdout** 与判题器通信，数据写入 `/data`，日志写 stderr，不启动网络服务。

协议以实现 `STDIO_PROTOCOL.md` 为准；公开自测题包在 `mysqltest/`，驱动在 `protocol_check/`。

## 1. 当前状态

- C++20，单二进制；启动后先输出 `{"type":"ready"}`，随后逐行处理 `execute` / `shutdown` 请求。
- 官方公开用例：**handshake PASS；20/20 用例 PASS**（本地 `--command` 与 Docker 镜像各跑一遍）。
- 已实现：`CREATE/DROP TABLE`、`INSERT/UPDATE/DELETE`、过滤/排序/LIMIT、`L2_DISTANCE` 精确与过滤 TopK、`BEGIN/COMMIT/ROLLBACK`、自动提交、WAL 崩溃恢复、`CREATE VECTOR INDEX` 元数据、`SHOW/DESCRIBE`。
- 结果按 JSON 原类型返回：BIGINT 为 JSON 整数、VARCHAR 为字符串、向量为数值数组、距离为浮点数；错误返回非空 `error.code`。

本地 JSONL 基准（WSL，10k×128，非平台 cgroup 口径）：

| 项 | 结果 |
|---|---|
| 批量写入 100×100 条 | 约 **1.1 s** |
| 1,000 次 TopK，K=10（一半过滤） | 堆 **1.615 s / 13.3 MiB**；全排序 **5.697 s / 20.1 MiB** |
| 100 次 TopK，K=1000 | 堆 **0.378 s / 13.3 MiB**；全排序 **0.643 s / 20.0 MiB** |

## 2. 构建与运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/minivectordb --data /data
```

Docker：

```bash
docker build -t minivectordb:latest .
docker run --rm -i -v minivectordb-data:/data --cpus=2 --memory=2g minivectordb:latest
```

不需要 `EXPOSE`、端口映射或 `-t`；判题器通过 `docker run -i` 的 stdin/stdout 通信。

### 启动参数

| 参数 | 环境变量 | 默认 | 说明 |
|---|---|---|---|
| `--data DIR` | `MVDB_DATA` | `/data` | 数据目录 |
| `--cache-mb N` | `MVDB_CACHE_MB` | `64` | 行缓存上限；超出后退化为按需读盘 |
| `--no-heap` | `MVDB_NO_HEAP` | off | 实验开关：关闭堆 TopK，走全量收集+排序 |

`search_mode` 由每个 `execute` 请求携带；当前 `ann` 走精确扫描（Recall = 1.0）。

## 3. JSONL 协议（摘要）

完整定义见 `STDIO_PROTOCOL.md`。

启动（恢复完成后、等待请求前）：

```json
{"type":"ready"}
```

执行请求：

```json
{"id":1,"op":"execute","sql":"SELECT id FROM docs ORDER BY id;","search_mode":"exact"}
```

查询响应：

```json
{"id":1,"ok":true,"columns":["id"],"rows":[[1],[2]]}
```

修改响应：

```json
{"id":2,"ok":true,"affected_rows":1}
```

错误响应：

```json
{"id":3,"ok":false,"error":{"code":"duplicate_key","message":"duplicate primary key"}}
```

关闭：

```json
{"id":4,"op":"shutdown"}
```

返回 `{"id":4,"ok":true}`，回滚未提交事务，flush 后退出码 0；之后 stdout 不再输出任何内容。

约束：每个报文一行、UTF-8、输出后立即 flush；`id` 原样回传且保持 JSON 类型；`ok` 必须是布尔；stdout 只允许协议报文；不输出空行、提示符、NaN/Infinity 或重复键。

## 4. SQL 子集

```sql
CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16), embedding VECTOR(2));
INSERT INTO docs VALUES (1,'甲','一','[0,0]'), (2,'甲','二','[1,0]');
SELECT id, content FROM docs WHERE id >= 1 AND category = '甲' ORDER BY id LIMIT 10;
SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs ORDER BY distance LIMIT 10;
UPDATE docs SET content = '修改', embedding = '[3,4]' WHERE id = 1;
DELETE FROM docs WHERE id = 2;
CREATE VECTOR INDEX docs_vec ON docs(embedding);
BEGIN; INSERT INTO docs VALUES (3,'乙','三','[0,2]'); ROLLBACK;
ALTER TABLE docs ADD extra BIGINT;  -- 约定错误：unsupported_sql
```

约定：

- 主键不可重复；不支持主键更新、NULL、JOIN、聚合、子查询、ALTER TABLE、外键、嵌套事务。
- VARCHAR 长度按 Unicode 码点数；比较按 Unicode 码点顺序（UTF-8 字节序等价）。
- 向量只接受有限 float32；拒绝 NaN/Infinity/维度不符/转换溢出；距离按存储后的 float32 计算，`L2_DISTANCE` 返回欧氏距离。
- 过滤后的 TopK 先过滤再取 K；不足 K 返回全部；距离/普通 ORDER BY 并列按主键升序。

## 5. 设计与恢复

### 记录布局

每个表一个追加式文件 `/data/t_<id>.dat`：

```
[u32 total_len][u32 crc32][u64 txn_id][i64 pk][u8 flags][3 bytes reserved][values...]
```

`flags` bit0 = 删除墓碑。列编码：BIGINT 8 字节；VARCHAR 为 `u32 字节长度 + 字节`；VECTOR 为 `dim × float32`。CRC 覆盖记录头之后的内容。

### 内存结构

- `pk -> 最新可见版本` 哈希映射；版本元数据含文件偏移、长度、txn/commit id、墓碑标记。
- 活跃事务的 pending 版本单独存放，读取时覆盖已提交版本，实现读己写。
- 小表构建行缓存（默认 64 MiB 上限）；新建表初始化为空缓存并在提交时增量维护；超限则清空并退化为按需 `pread`，避免全量常驻内存。

### 事务与 WAL

- 每条记录携带 `txn_id`；WAL（`/data/wal.log`）记录 `BEGIN` / `COMMIT(txn, commit_id)` / `ABORT`，28 字节定长 + CRC32。
- 非显式事务的 DML 自动提交：BEGIN → 执行 → 数据 `fdatasync` → COMMIT → WAL `fsync` → 返回成功。
- `ROLLBACK` 丢弃 pending；数据文件中未提交记录永远不会被恢复。
- 提交顺序：先 `fdatasync` 数据文件，再写 COMMIT 并 `fsync` WAL。

### 恢复

1. 读 `catalog.meta`（tmp + fsync + rename 原子写）。
2. 顺序读 WAL，校验长度/CRC，得到已提交/已中止事务与最大 txn/commit id；尾部非法记录截断。
3. 顺序扫描表文件，只把“已提交且未中止”的记录加入内存映射；按 `(commit_id, offset)` 选最新版本。
4. 构建行缓存；完成后才输出 `{"type":"ready"}`。恢复只读表文件（仅截断撕裂尾部），重复恢复幂等。

### 向量检索

- 精确扫描；先执行过滤，再在结果集合上计算 L2，并用大小为 `K+offset` 的最大堆维护 TopK。
- 距离用 AVX2 + FMA 双精度累加（运行时分发；无 AVX2 回退标量 double）。
- `CREATE VECTOR INDEX` 记录元数据；`ann` 模式返回精确结果，保证 Recall = 1.0。

## 6. 错误码（语义名）

| 场景 | code |
|---|---|
| 主键重复 | `duplicate_key` |
| 不支持的语句（如 ALTER TABLE） | `unsupported_sql` |
| 向量维度不符 | `dimension_mismatch` |
| 非法向量（NaN/Inf/溢出） | `invalid_vector` |
| VARCHAR 超长 | `value_too_long` |
| 表已存在 / 不存在 | `table_exists` / `table_not_found` |
| 列不存在 | `column_not_found` |
| 整数溢出 | `integer_out_of_range` |
| 语法错误 | `syntax_error` |

公开题包的 `config.json` 暂未冻结错误码映射；正式映射以统一题包为准，判题不依赖 `message` 措辞。

## 7. 已实现功能与已知限制

已实现：JSONL 协议、上述 SQL 子集、精确/过滤 TopK、事务、自动提交、WAL 恢复、崩溃恢复、UTF-8 长度与码点比较、向量合法性校验、目录持久化、`SHOW TABLES`/`DESCRIBE`、`SELECT` 无 FROM 的常量查询。

已知限制：

1. **正式 29 个用例与隐藏数据不可见**；公开 20 个样例通过不代表功能准入通过。
2. **ANN**：未实现物理近似索引，`ann` 走精确扫描；正确性和召回满足，但大数据集查询耗时不如专用索引。
3. **DDL 与事务**：显式事务内执行 DDL 返回 `unsupported_sql`；不支持事务性 DDL。
4. **存储增长**：更新/删除追加新版本，未实现 compaction/GC。
5. **PK 映射**常驻内存；数千万行以上需要进一步压缩或磁盘索引。
6. **并发**：单进程、单客户端、单显式事务。
7. **本地性能**是 WSL + JSONL 驱动的测量，不是平台 cgroup 口径。

## 8. 本地测试

```bash
# 官方协议握手（本地进程）
python3 protocol_check/run.py \
  --command '["build/minivectordb","--data","{data_dir}"]' \
  --handshake-only --output "protocol-results/handshake-$(date +%Y%m%d-%H%M%S)"

# 官方 20 个公开用例（本地进程）
python3 protocol_check/run.py \
  --command '["build/minivectordb","--data","{data_dir}"]' \
  --output "protocol-results/public-$(date +%Y%m%d-%H%M%S)"

# 官方 20 个公开用例（Docker 镜像）
python3 protocol_check/run.py --image minivectordb:latest \
  --output "protocol-results/public-image-$(date +%Y%m%d-%H%M%S)"

# 本地 JSONL 性能 / 崩溃恢复
python3 tests/bench.py
python3 tests/bench.py --no-heap
python3 tests/crash_test.py
```
