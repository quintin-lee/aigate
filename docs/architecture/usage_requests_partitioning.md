# aigate `usage_requests` 分区方案架构设计

- **模块**: 数据面用量审计与存储架构 (Audit Store Architecture)
- **版本对应**: Schema v6 / v7 进阶 (建议作为 Migration v8 候选)
- **目标**: 支持日均千万级请求（月度数亿级审计记录）下的高吞吐写入、毫秒级范围查询与零开销数据生命周期清理（Retention）。

---

## 1. 背景与现状瓶颈

### 1.1 现状数据模型
在 Schema v6 中，aigate 引入了每请求审计明细表 `usage_requests`：
```sql
CREATE TABLE IF NOT EXISTS usage_requests (
  key_id               BIGINT NOT NULL,
  model_name           TEXT NOT NULL,
  provider             TEXT NOT NULL DEFAULT '',
  http_status          INT NOT NULL,
  prompt_tokens        BIGINT NOT NULL DEFAULT 0,
  completion_tokens    BIGINT NOT NULL DEFAULT 0,
  cached_prompt_tokens BIGINT NOT NULL DEFAULT 0,
  latency_ns           BIGINT NOT NULL DEFAULT 0,
  ts                   BIGINT NOT NULL -- UTC 秒级时间戳
);
CREATE INDEX IF NOT EXISTS ix_usage_requests_key_ts ON usage_requests (key_id, ts);
CREATE INDEX IF NOT EXISTS ix_usage_requests_ts ON usage_requests (ts);
```

### 1.2 大规模场景下的物理瓶颈
当业务规模达到日均 500 万 ~ 2000 万请求时，单表每月增长 1.5 亿 ~ 6 亿行（存储占用约 30GB ~ 120GB/月）：
1. **索引膨胀与缓存失效 (B-Tree Working Set)**：
   `ix_usage_requests_key_ts` 和 `ix_usage_requests_ts` 的 B-Tree 体积将迅速超过 PostgreSQL 的 `shared_buffers` 乃至宿主机 RAM。随机 key_id 写入将导致剧烈的索引页分裂与频繁的磁盘随机 I/O，严重拖慢网关异步 drain 批量写入速度。
2. **过期数据清理灾难 (WAL Amplification & Bloat)**：
   若采用传统的 `DELETE FROM usage_requests WHERE ts < now() - interval '90 days'`，将产生海量 WAL 日志、严重的表膨胀（Dead Tuples）以及长事务锁竞争，触发重度 autovacuum 侵占系统资源。
3. **范围查询与聚合扫描成本高**：
   管理后台在查询最近 7 天或 30 天的明细或成本分摊时，需要扫描庞大的单表及全量索引，缺乏物理边界隔离。

---

## 2. 方案选型与分区键设计

### 2.1 选型：PostgreSQL 原生声明式 RANGE 分区
对比方案：
- **方案 A（应用层分表）**：在 C 代码根据时间戳动态格式化表名 `usage_requests_202609`。缺点是应用层侵入深，跨表聚合（如跨月成本分摊报表）极其繁琐。
- **方案 B（TimescaleDB Hypertable）**：时序数据库插件，功能完善但引入重度第三方扩展依赖，不符合 aigate 追求轻量自治、单二进制分发的原则。
- **方案 C（PostgreSQL 原生声明式 RANGE 分区 - 推荐采用）**：
  PostgreSQL 11+ 原生支持完善的声明式分区（Declarative Partitioning）、自动索引继承、分区剪枝（Partition Pruning）与并行聚合，对业务代码透明。

### 2.2 分区键与粒度设计
- **分区键**：`ts` (`BIGINT`)。
  - 虽然 PostgreSQL 原生对 `TIMESTAMPTZ` 支持友好，但 aigate 核心内存态与传输协议统一采用 UTC Unix Epoch 秒 (`long ts`)，避免时区解析开销与浮点偏差。
  - 因此分区定义直接采用 `RANGE (ts)`，区间边界采用整月或整周的 Epoch 秒整数。
- **分区粒度**：
  - **默认推荐：按月分区 (Monthly)**。单月 5000 万 ~ 1.5 亿行，分区物理文件约 10GB ~ 30GB，适中便于维护。
  - **高吞吐场景（>1000 万 req/day）：按周分区 (Weekly)**。

---

## 3. Schema v8 DDL 定义

### 3.1 声明式父表
```sql
-- 父表定义（PARTITION BY RANGE）
CREATE TABLE usage_requests_p (
  key_id               BIGINT NOT NULL,
  model_name           TEXT NOT NULL,
  provider             TEXT NOT NULL DEFAULT '',
  http_status          INT NOT NULL,
  prompt_tokens        BIGINT NOT NULL DEFAULT 0,
  completion_tokens    BIGINT NOT NULL DEFAULT 0,
  cached_prompt_tokens BIGINT NOT NULL DEFAULT 0,
  latency_ns           BIGINT NOT NULL DEFAULT 0,
  ts                   BIGINT NOT NULL
) PARTITION BY RANGE (ts);

-- 父表索引定义（PG 11+ 自动向所有子分区级联创建）
CREATE INDEX ix_usage_requests_p_key_ts ON usage_requests_p (key_id, ts);
CREATE INDEX ix_usage_requests_p_ts ON usage_requests_p (ts);
```

### 3.2 子分区定义示例（2026年第四季度）
分区边界采用左闭右开 `[FROM, TO)`：
```sql
-- 2026-10 (2026-10-01 00:00:00 UTC = 1790812800 ~ 2026-11-01 00:00:00 UTC = 1793491200)
CREATE TABLE usage_requests_y2026m10 PARTITION OF usage_requests_p
  FOR VALUES FROM (1790812800) TO (1793491200);

-- 2026-11 (2026-11-01 00:00:00 UTC = 1793491200 ~ 2026-12-01 00:00:00 UTC = 1796083200)
CREATE TABLE usage_requests_y2026m11 PARTITION OF usage_requests_p
  FOR VALUES FROM (1793491200) TO (1796083200);

-- 2026-12 (2026-12-01 00:00:00 UTC = 1796083200 ~ 2027-01-01 00:00:00 UTC = 1798761600)
CREATE TABLE usage_requests_y2026m12 PARTITION OF usage_requests_p
  FOR VALUES FROM (1796083200) TO (1798761600);

-- 兜底分区（Default Partition，防止因时钟偏差或异常超前/滞后数据导致 INSERT 报错）
CREATE TABLE usage_requests_default PARTITION OF usage_requests_p DEFAULT;
```

---

## 4. 零停机平滑迁移流程 (Zero-Downtime Migration)

生产环境下已有海量单表数据时，必须保证网关在线写入不中断、数据不丢失。

### 阶段一：创建分区结构与未来分区
在数据库中预先创建 `usage_requests_p` 父表以及当前月、下个月的子分区和 `default` 分区。

### 阶段二：原子原子重命名切换 (毫秒级锁)
利用 PostgreSQL 支持事务内 DDL 的特性完成秒级原子表切换：
```sql
BEGIN;
-- 1. 将原单表改名为 _old
ALTER TABLE usage_requests RENAME TO usage_requests_old;

-- 2. 将新建的分区父表重命名为正式表名
ALTER TABLE usage_requests_p RENAME TO usage_requests;

-- 3. 将旧表的索引重命名避免命名冲突
ALTER INDEX ix_usage_requests_key_ts RENAME TO ix_usage_requests_old_key_ts;
ALTER INDEX ix_usage_requests_ts RENAME TO ix_usage_requests_old_ts;
ALTER INDEX ix_usage_requests_p_key_ts RENAME TO ix_usage_requests_key_ts;
ALTER INDEX ix_usage_requests_p_ts RENAME TO ix_usage_requests_ts;
COMMIT;
```
> **影响评估**：该事务仅修改系统元数据（Catalog），耗时 < 5ms。切换瞬间后，aigate 网关新的批量写入立即路由进入新的分区表中，无任何请求中断。

### 阶段三：历史数据分批离线回填 (Backfill)
通过外置脚本或运维后台分批将 `usage_requests_old` 中的历史数据迁入新表：
```sql
-- 按天或按时间切片分批复制，避免大事务打满 WAL
DO $$
DECLARE
  v_start BIGINT := 1788220800; -- 起始时间戳
  v_step  BIGINT := 86400;      -- 每次回填 1 天
  v_end   BIGINT := 1790812800; -- 切换时间戳
  v_curr  BIGINT := v_start;
BEGIN
  WHILE v_curr < v_end LOOP
    INSERT INTO usage_requests (
      key_id, model_name, provider, http_status,
      prompt_tokens, completion_tokens, cached_prompt_tokens,
      latency_ns, ts
    )
    SELECT
      key_id, model_name, provider, http_status,
      prompt_tokens, completion_tokens, cached_prompt_tokens,
      latency_ns, ts
    FROM usage_requests_old
    WHERE ts >= v_curr AND ts < v_curr + v_step;

    COMMIT; -- 提交当前批次
    PERFORM pg_sleep(0.5); -- 控频，防磁盘 I/O 尖峰
    v_curr := v_curr + v_step;
  END LOOP;
END $$;
```

### 阶段四：验证与清理旧表
回填完成后，核对旧表与新分区表中数据行数及校验和：
```sql
SELECT count(*) FROM usage_requests_old;
DROP TABLE usage_requests_old; -- 瞬时释放磁盘空间
```

---

## 5. 分区生命周期自动维护 (Automation & Retention)

### 5.1 方案 A：原生 SQL 存储过程 + 定时触发 (推荐，零外部依赖)
在数据库中定义自动化运维函数：
```sql
CREATE OR REPLACE PROCEDURE aigate_maintain_partitions(
  retention_days INT DEFAULT 90,
  precreate_months INT DEFAULT 2
)
LANGUAGE plpgsql AS $$
DECLARE
  v_now_ts      BIGINT := EXTRACT(EPOCH FROM now())::BIGINT;
  v_cutoff_ts   BIGINT := v_now_ts - (retention_days * 86400);
  v_month_start TIMESTAMPTZ;
  v_part_start  BIGINT;
  v_part_end    BIGINT;
  v_part_name   TEXT;
  v_drop_name   TEXT;
  r RECORD;
BEGIN
  -- 1. 预先创建未来几个月的分区
  FOR i IN 0..precreate_months LOOP
    v_month_start := date_trunc('month', now() + (i || ' month')::interval);
    v_part_start  := EXTRACT(EPOCH FROM v_month_start)::BIGINT;
    v_part_end    := EXTRACT(EPOCH FROM v_month_start + interval '1 month')::BIGINT;
    v_part_name   := 'usage_requests_y' || to_char(v_month_start, 'YYYY') || 'm' || to_char(v_month_start, 'MM');

    IF NOT EXISTS (SELECT 1 FROM pg_class WHERE relname = v_part_name) THEN
      EXECUTE format(
        'CREATE TABLE IF NOT EXISTS %I PARTITION OF usage_requests FOR VALUES FROM (%s) TO (%s)',
        v_part_name, v_part_start, v_part_end
      );
      RAISE NOTICE 'Created partition %', v_part_name;
    END IF;
  END LOOP;

  -- 2. 检查并剥离/清理过期分区
  FOR r IN
    SELECT c.relname,
           pg_get_expr(c.relpartbound, c.oid) AS bound_expr
    FROM pg_inherits i
    JOIN pg_class c ON c.oid = i.inhrelid
    JOIN pg_class p ON p.oid = i.inhparent
    WHERE p.relname = 'usage_requests' AND c.relname <> 'usage_requests_default'
  LOOP
    -- 提取分区上界，若小于保留截止时间则执行卸载或删除
    -- 生产建议：先 DETACH 再 DROP，安全可控
    -- 示例逻辑：DROP TABLE r.relname;
  END LOOP;
END;
$$;
```
可在 Linux crontab 或 `pg_cron` 中每天执行一次：
```bash
0 2 * * * psql $PG_DSN -c "CALL aigate_maintain_partitions(90, 2);"
```

### 5.2 方案 B：pg_partman 插件
若部署环境允许安装官方插件，推荐集成 `pg_partman`：
```sql
CREATE EXTENSION IF NOT EXISTS pg_partman;
SELECT partman.create_parent(
  p_parent_table := 'public.usage_requests',
  p_control := 'ts',
  p_type := 'native',
  p_interval := '2592000', -- 30 天秒数
  p_premake := 2
);
```

---

## 6. 查询性能优化与分区剪枝 (Partition Pruning)

### 6.1 分区剪枝机制
PostgreSQL 内置的分区剪枝（`SET enable_partition_pruning = on`，默认启用）会在执行规划或执行期间评估 WHERE 条件中的 `ts`：

- **单日/近期范围查询**：
  ```sql
  SELECT * FROM usage_requests
  WHERE ts >= 1791000000 AND ts < 1791086400;
  ```
  执行计划直接过滤掉非目标月份的全部物理分区，只扫描对应的 `usage_requests_y2026m10`，**I/O 降低 90% 以上**。

- **部门成本分摊跨月聚合 (`query_cost`)**：
  ```sql
  SELECT COALESCE(g.id,0) gid, ur.model_name,
         SUM(prompt_tokens) p, SUM(completion_tokens) c,
         SUM(cached_prompt_tokens) cp, COUNT(*) rq
  FROM usage_requests ur
  JOIN api_keys k ON k.key_id=ur.key_id
  LEFT JOIN groups g ON g.id=k.group_id
  WHERE ur.ts >= $1 AND ur.ts < $2
  GROUP BY 1,2;
  ```
  PostgreSQL 可启动并行分区追加扫描（`Parallel Append`），多个工作进程独立并发扫描不同子分区的局部索引与堆数据，极大加速大跨度成本统计。

### 6.2 索引精简与 BRIN 索引评估
- 现有 B-Tree 索引 `(key_id, ts)` 是支持后台单 key 明细翻页的最佳索引。
- **BRIN 索引备选**：对于纯时间戳追加的 `ts` 列，BRIN 索引占用体积仅为 B-Tree 的 1%：
  ```sql
  CREATE INDEX ix_usage_requests_ts_brin ON usage_requests USING brin (ts);
  ```
  在大规模数据归档分区上，可将 `ix_usage_requests_ts` 替换为 BRIN 索引以大幅节省内存。

---

## 7. 实施路线建议

1. **短期（当前版本）**：
   - 保持 Schema v7 现状，现有 `usage_requests` 单表配合 `batch drain`（Phase 1 优化）可轻松承载上千万级数据。
2. **中期（规模扩大到数千万行后）**：
   - 按本文第 4 节执行零停机无缝切换至 Schema v8 分区方案。
   - 部署 `aigate_maintain_partitions` 每日定时巡检任务。
3. **长期（冷热分层与归档）**：
   - 超过 90 天的历史分区可直接执行 `ALTER TABLE usage_requests DETACH PARTITION`，导出至 S3 / Parquet / ClickHouse 深度分析仓，本地直接 DROP，完全消除数据库膨胀与清理负担。
