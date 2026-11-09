# MiniVectorDB（2026-competition-round2）

单机、单进程、单客户端的轻量向量数据库。Linux x86_64 容器前台运行：stdin 收 SQL，stdout 回结果，stderr 写日志，`/data` 持久化。

## 1. 实现状态

已实现：

- `CREATE TABLE` / `DROP TABLE`：BIGINT 主键、VARCHAR(n)、VECTOR(n)，每表一个 BIGINT 主键、最多一个向量列。
- `INSERT`（多行、可选列清单）、`SELECT`（投影、`*`、`WHERE` 的 `= < <= > >= AND`、`ORDER BY`、`LIMIT/OFFSET`）、`UPDATE`、`DELETE`。
- `L2_DISTANCE` 欧氏距离、精确 TopK、带普通列过滤的 TopK；`CREATE VECTOR INDEX` 记录元数据。
- `BEGIN` / `COMMIT` / `ROLLBACK`、自动提交、事务内读自己的修改、语句失败不留下部分修改。
- WAL + 追加式行版本；COMMIT 前 `fdatasync` 数据文件并 `fsync` WAL；进程被 `kill -9` 后按 WAL 恢复，未提交事务不生效。
- `/data` 持久化；重启后表定义、数据、已提交事务一致；恢复幂等，尾部撕裂记录会被截断。

本地验证（WSL Ubuntu，g++ 15.2，AVX2）：

| 项 | 结果 |
|---|---|
| 基础 CRUD / 投影 / 过滤 / 排序 / LIMIT | 通过 |
| 过滤后 TopK（先过滤再取 K） | 通过 |
| 距离并列按主键升序、普通 ORDER BY 并列按主键升序 | 通过 |
| VARCHAR 按 Unicode 字符数、按码点比较 | 通过（中文 1 字符可入 VARCHAR(1)，2 字符报 1406） |
| 向量维度/NaN/Inf/溢出拒绝 | 通过（1366） |
| 事务内读己写、ROLLBACK、COMMIT | 通过 |
| 语句失败后事务可继续提交 | 通过 |
| `kill -9`：未提交消失、已提交保留 | 通过 |
| 数据文件/WAL 尾部损坏 | 通过（截断后正常启动） |
| 10,000 条 × 128 维批量写入（100 条/批） | 约 **0.36–0.59 s** |
| 1,000 次 TopK（K=10，一半带等值过滤） | 约 **0.98 s**（含 1.2 万行输出） |

## 2. 构建与运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/minivectordb --data /data
```

Docker（多阶段构建，运行镜像只带二进制）：

```bash
docker build -t minivectordb:latest .
docker run --rm -i -v minivectordb-data:/data --cpus=2 --memory=2g minivectordb:latest
```

不需要 `EXPOSE`/端口；使用 `-i`，不要 `-t`/`-d`。

### 启动参数与环境变量

| 参数 | 环境变量 | 默认 | 说明 |
|---|---|---|---|
| `--data DIR` | `MVDB_DATA` | `/data` | 数据目录 |
| `--ready TEXT` | `MVDB_READY` | `READY` | 启动后打印的就绪行 |
| `--mode exact\|ann` | `MVDB_MODE` | `exact` | 向量检索模式；当前 ann 走精确扫描 |
| `--format tsv\|mysql` | `MVDB_FORMAT` | `tsv` | 结果集格式 |
| `--cache-mb N` | `MVDB_CACHE_MB` | `64` | 行缓存上限；超过则退化为按需读盘 |
| `--no-heap` | `MVDB_NO_HEAP` | off | 实验开关：关闭堆 TopK，改为收集全部匹配后排序（A/B 用） |

## 3. 协议

题面未附测试驱动/题包，以下为本实现采用的协议，集中在一层，便于按平台驱动调整：

- 启动：恢复完成后向 stdout 打印一行 `READY`（可配置）。
- 输入：从 stdin 读取，按顶层 `;` 切分；字符串、反引号标识符、`--`/`#`/`/* */` 注释中的 `;` 不切分。
- 查询结果（默认 `tsv`）：首行列名，随后数据行，制表符分隔；字符串中的 `\t`、`\n`、`\r`、`\\`、`\0` 转义。
- DML/DDL：`Query OK, N row(s) affected`。
- 错误：`ERROR <code> (<sqlstate>): <message>`。
- 退出：`\q`、`EXIT`、`QUIT`、stdin EOF、SIGTERM/SIGINT；正常退出会回滚未提交事务。
- stdout 由独立写线程 + 队列输出，每条语句后 flush；避免驱动先写完输入再读输出时管道互锁。

## 4. SQL 子集

```sql
CREATE TABLE t (
  id BIGINT PRIMARY KEY,
  name VARCHAR(20),
  vec VECTOR(3)
);
INSERT INTO t VALUES (1, 'a', '[1,2,3]'), (2, 'b', '[4,5,6]');
INSERT INTO t (id, name, vec) VALUES (3, 'c', VECTOR('[7,8,9]'));
SELECT * FROM t WHERE id >= 1 AND name < 'z' ORDER BY id DESC LIMIT 2;
SELECT id, L2_DISTANCE(vec, '[1,2,3]') AS d FROM t ORDER BY d LIMIT 1;
UPDATE t SET name = 'z', vec = '[9,9,9]' WHERE id = 1;
DELETE FROM t WHERE id = 2;
CREATE VECTOR INDEX idx ON t(vec);
BEGIN; INSERT INTO t VALUES (4, 'd', '[0,0,0]'); ROLLBACK;
```

约定：

- 主键不可重复；不支持主键更新、NULL、JOIN、聚合、子查询、ALTER TABLE、外键、嵌套事务。
- VARCHAR 长度按 Unicode 码点数；字符串比较按 Unicode 码点顺序（UTF-8 字节序等价）。
- 向量只接受有限 float32；拒绝 NaN/Infinity/维度不符/转换溢出。
- 距离按存储后的 float32 计算，输出使用 `%.6f`。
- 过滤后的 TopK 先过滤再取 K；不足 K 返回全部；距离/普通 ORDER BY 并列按主键升序。
- 未指定 ORDER BY 时本实现按主键升序返回（题面按集合比较）。

## 5. 设计与恢复

### 记录布局

每个表一个追加式文件 `/data/t_<id>.dat`，每条记录：

```
[u32 total_len][u32 crc32][u64 txn_id][i64 pk][u8 flags][3 bytes reserved][values...]
```

`flags` bit0 = 删除墓碑。列编码：BIGINT 8 字节；VARCHAR 为 `u32 字节长度 + 字节`；VECTOR 为 `dim × float32`。CRC 覆盖记录头之后的内容。

### 内存结构

- `pk -> 最新可见版本` 的哈希映射；版本元数据含文件偏移、长度、txn/commit id、墓碑标记。
- 活跃事务的 pending 版本单独存放，读取时覆盖已提交版本，实现读己写。
- 小表构建行缓存（默认 64 MiB 上限）；超过上限则不缓存，扫描按需 `pread`，避免全量载入内存。

### 事务与 WAL

- 每条记录携带 `txn_id`；WAL（`/data/wal.log`）记录 `BEGIN` / `COMMIT(txn, commit_id)` / `ABORT`，28 字节定长 + CRC32。
- 非显式事务的 DML 走自动提交：BEGIN → 执行 → 数据 `fdatasync` → COMMIT → WAL `fsync` → 返回成功。
- `ROLLBACK` 丢弃 pending；数据文件中的旧记录因没有 COMMIT 而永远不会被恢复。
- 提交顺序：先 `fdatasync` 数据文件，再写 COMMIT 并 `fsync` WAL；崩溃模型是进程被强杀，因此该顺序保证“已确认 COMMIT 不丢、未提交不生效”。

### 恢复

1. 读 `catalog.meta`（原子写：tmp + fsync + rename）。
2. 顺序读 WAL，校验长度/CRC，得到已提交、已中止事务集合与最大 txn/commit id；尾部非法记录截断。
3. 顺序扫描每个表文件，只把“txn 已提交且未中止”的记录加入内存映射；按 `(commit_id, offset)` 选最新版本。
4. 构建行缓存；完成后再打印 `READY`。恢复只读数据文件（仅截断撕裂尾部），重复恢复幂等。

### 向量检索

- 精确扫描；过滤条件先求值，再在结果集合上计算 L2 并维护大小为 `K+offset` 的最大堆，最后排序输出。
- 距离使用 AVX2 + FMA 双精度累加（运行时分发；无 AVX2 时回退标量 double）。
- `CREATE VECTOR INDEX` 只记录元数据；`ann` 模式返回精确结果（Recall = 1.0），因此不依赖物理索引也能满足正确性与召回要求。性能基准可选 exact。

## 6. 错误码

| 场景 | 码 |
|---|---|
| 语法错误/不支持的语句 | 1064 |
| 表已存在 / 不存在 | 1050 / 1146 |
| 列不存在 / 重复列 | 1054 / 1060 |
| 主键重复 | 1062 |
| 列数不匹配 / 列重复指定 | 1136 / 1110 |
| 缺主键值 | 1364 |
| VARCHAR 超长 | 1406 |
| 整数溢出 / 向量非法/维度不符 | 1264 / 1366 |
| 嵌套 BEGIN | 1568 |

## 7. 已实现功能与已知限制

已实现：上述 SQL 子集、精确/过滤 TopK、事务、自动提交、WAL 恢复、崩溃恢复、UTF-8 长度与码点比较、向量合法性校验、目录持久化、`SHOW TABLES`/`DESCRIBE`、`SELECT` 无 FROM 的常量查询。

已知限制：

1. **协议格式**：题包/驱动未提供，`READY`、TSV、`Query OK`、错误文案均为本实现约定；已做成参数/环境变量可切换，若平台驱动不同，只需改输出层。
2. **ANN**：未实现物理近似索引，`ann` 走精确扫描；功能正确、召回 1.0，但大数据集查询耗时不如专用索引。
3. **DDL 与事务**：显式事务内执行 DDL 返回 1105；不支持事务性 DDL。
4. **存储增长**：更新/删除追加新版本，未实现 compaction/GC；长期大量更新会使文件增长。
5. **PK 映射**：常驻内存；超大规模（数千万行以上）时内存占用需要进一步压缩或改为磁盘索引。
6. **并发**：单进程、单客户端、单显式事务，未实现多客户端并发控制。
7. **输出顺序**：无 ORDER BY 时按主键升序输出（题面按集合比较，顺序不应作为判据）。

## 8. 本地测试

```bash
# 功能烟测（会创建临时数据目录）
bash tests/run_smoke.sh

# kill -9 崩溃恢复
bash tests/crash_test.sh

# 10k × 128 维写入/查询基准
python3 tests/bench.py
```
