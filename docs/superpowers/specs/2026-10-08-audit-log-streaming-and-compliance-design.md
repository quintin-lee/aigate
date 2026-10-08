# 审计日志与合规投递设计规格说明书 (Audit Log Streaming & Compliance Design)

- **作者**: aigate 架构组
- **日期**: 2026-10-08
- **状态**: Approved / Spec Ready
- **关联规范**: [docs/superpowers/specs/2026-09-26-guardrails-and-budget-limits-design.md](2026-09-26-guardrails-and-budget-limits-design.md), [docs/CONFIGURATION.md](../../CONFIGURATION.md)

---

## 1. 业务背景与设计目标

### 1.1 现状与痛点
当前 `aigate` 网关已具备基于 PostgreSQL 的用量与计费计量流水（`usage_requests` 表，包含 Token 数、延迟、状态码等元数据），但缺乏企业级安全与合规审计能力：
1. **内容取证缺失**：当触发护栏拦截（Guardrails）、敏感信息阻断（PII Leak）、恶意注入攻击或模型越权时，网关未记录违规时刻的原始 Prompt 上下文与命中规则证据，事后无法审计追溯。
2. **缺乏流式归档导出**：审计信息局限于 PG 关系型数据库，未提供云原生标准日志导出接口（如 NDJSON 文件流），难以直接对接企业数据湖、ClickHouse、Vector、Filebeat 等分析工具。
3. **实时安全告警缺失**：当发生严重安全拦截、并发配额击穿或 5xx 故障时，外部安全运维中心无法实时感知。

### 1.2 核心目标
构建**独立双通道异步审计管线**：
1. **主通路零阻塞 (< 1µs)**：请求主链路只负责极速填充轻量事件对象推入环形队列，绝不在主线程执行任何磁盘或网络系统调用。
2. **动态安全分级留存**：正常放行流量（`INFO`）仅记录纯元数据，零隐私泄漏且极低 I/O；拦截违规流量（`VIOLATION`/`ERROR`）完整保留截断的原始 Prompt 现场证据。
3. **通道 A（文件异步落盘）**：后台 Worker 定时批量写入结构化单行 JSON（NDJSON），支持自动大小切分与轮转。
4. **通道 B（安全告警 Webhook）**：针对违规拦截与严重异常事件，后台 HTTP Worker 异步将结构化告警或聊天软件卡片推送到目标 Webhook，具备有限重试与防雪崩机制。

---

## 2. 总体架构与数据流

```
[ 客户端请求 Client Request ]
             │
             ▼
[ aigate 业务流水线 (Pipeline) ]
  ├── 鉴权检查 (Auth Check) ───────────┐ 违规/异常拦截分支
  ├── 护栏检测 (Guardrails) ───────────┤ (如: PII/注入阻断)
  ├── 路由与并发控制 (Model Router) ───┤ (如: 429 并发耗尽)
  └── 上游代理响应 (Proxying) ──────────┘ 正常 200 结束 / 5xx
             │
             ▼
  ┌────────────────────────────────────────────────────────┐
  │ 纳秒级内存推入：audit_logger_record()                    │
  │ • INFO 级（正常）：仅拷贝元数据，prompt_snapshot 为 NULL │
  │ • VIOLATION/ERROR 级：受控拷贝截断 Prompt 现场 (≤ 4KB)   │
  └──────────────────────────┬─────────────────────────────┘
                             │
            ┌────────────────┴────────────────┐
            ▼ (无锁/轻量自旋环形缓冲区)           ▼ (安全告警专用缓冲队列)
   [ 审计环形队列 Audit Ring ]            [ 告警队列 Alert Queue ]
            │                                 │
            ▼                                 ▼
   [ 异步落盘线程 File Worker ]           [ 异步推送线程 Webhook Worker ]
            │                                 │
            ▼                                 ▼
   追加写入 NDJSON 轮转文件               HTTP POST 异步推送至企业端
   (例如: /var/log/aigate/audit.ndjson)   (标准 JSON / 飞书 / 钉钉 / 企微)
```

---

## 3. 数据模型与安全分级规范

### 3.1 安全等级枚举 (`audit_severity_t`)

```c
typedef enum {
    AUDIT_SEV_INFO      = 0,  /* 正常成功响应 (HTTP 200) */
    AUDIT_SEV_WARN      = 1,  /* 429 限流 / 并发耗尽 / 上游 Failover */
    AUDIT_SEV_VIOLATION = 2,  /* 护栏阻断 / PII 敏感泄露 / 恶意注入 / 403 越权 */
    AUDIT_SEV_ERROR     = 3   /* 网关 5xx / 上游全部宕机 / 严重系统故障 */
} audit_severity_t;
```

### 3.2 审计事件数据结构 (`audit_event_t`)

```c
typedef struct audit_event {
    /* 1. 全链路元数据 */
    char              trace_id[64];       /* 全链路 Trace ID (W3C traceparent 或 UUID) */
    int64_t           timestamp_ms;       /* 毫秒时间戳 */
    int64_t           key_id;             /* 鉴权 Key ID */
    char              client_ip[48];      /* 客户端真实 IP (经过信任反代解析) */
    char              model[64];          /* 请求模型名 */
    char              provider[32];       /* 实际生效的 Provider */
    int               http_status;        /* HTTP 响应状态码 */
    uint32_t          prompt_tokens;      /* 输入 Token 计数 */
    uint32_t          completion_tokens;  /* 输出 Token 计数 */
    uint64_t          latency_ns;         /* 整体端到端耗时 (纳秒) */
    uint64_t          ttft_ns;            /* 首字延迟 (流式请求专用，纳秒) */

    /* 2. 审计分级与违规信息 */
    audit_severity_t  severity;           /* INFO / WARN / VIOLATION / ERROR */
    char              violation_type[32]; /* 规则标签: "guardrail_block", "pii_leak", "concurrency" 等 */
    char              rule_detail[128];   /* 命中规则明细 (如: "pattern: regex_ssn") */

    /* 3. 动态现场上下文快照 */
    char*             prompt_snapshot;    /* 截断快照 (仅 VIOLATION/ERROR 分配拷贝，INFO 为 NULL) */
    size_t            prompt_snapshot_len;
} audit_event_t;
```

---

## 4. 详细组件与投递引擎设计

### 4.1 核心环形缓冲区 (`audit_ring_t`)
- **容量定义**：固定容量（默认 `4096` 个槽位）。
- **线程安全与低延迟**：
  - 采用无锁原子头尾指针或基于轻量 `pthread_spinlock_t` 的读写隔离设计。
  - 生产者（主请求线程）：仅执行槽位分配与浅拷贝，若队列满则丢弃最旧数据并自增 `aigate_audit_dropped_total` 指标。
  - 消费者（Worker 线程）：批量提取就绪事件槽位（最多单次提取 128 条）。

### 4.2 通道 A：NDJSON 文件异步落盘与轮转 (`audit_file_worker`)
- **写入格式**：单行标准 NDJSON，字段全量序列化：
  ```json
  {"ts":"2026-10-08T19:45:00.123Z","trace_id":"00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01","severity":"VIOLATION","key_id":12,"client_ip":"192.168.1.100","model":"gpt-4o","provider":"openai","status":403,"prompt_tokens":0,"completion_tokens":0,"latency_us":1250,"violation":{"type":"pii_leak","detail":"phone_number_detected"},"prompt":"联系电话是 13800138000"}
  ```
- **攒批调度**：
  - 每 `200ms` 超时或队列积压达到 `64` 条时唤醒写入。
  - 使用用户态缓冲流 `fwrite`，并在每批末尾执行 `fflush`。
- **文件轮转策略**：
  - 单文件达到 `AIGATE_AUDIT_MAX_SIZE_MB` 时，关闭句柄，将当前文件滚动重命名为 `.1`，历史依次推移至 `.N`，超出最大保留数的文件直接删除。
  - 捕获 `SIGHUP` 信号以平滑支持外部 Linux `logrotate`。

### 4.3 通道 B：实时安全告警 Webhook 引擎 (`audit_webhook_worker`)
- **告警过滤**：仅投递 `severity >= AUDIT_SEV_VIOLATION` 的事件。
- **适配器模板 (Adapter)**：
  1. `standard`：通用结构化 JSON，供 SIEM / 自动化安全运维平台消费。
  2. `feishu`：飞书富文本卡片（`interactive` 消息格式，展示红标告警头、违规模型、IP、命中原因、违规 Prompt 引用）。
  3. `dingtalk`：钉钉 `markdown` 消息格式。
  4. `wechat_work`：企业微信 `markdown` 机器人消息格式。
- **弹性与容灾控制**：
  - 告警队列深度硬上限为 `1024`。
  - 单次 HTTP 客户端调用硬超时限制：连接超时 2s，总传输超时 5s。
  - 失败重试：最多重试 3 次，采用指数退避（1s → 2s → 4s）；连续不可达则静默丢弃，绝对不阻塞业务通路。

---

## 5. 配置参数定义

全部配置项遵循网关现有的环境变量规范：

| 环境变量 | 类型 | 默认值 | 作用说明 |
| :--- | :--- | :--- | :--- |
| `AIGATE_AUDIT_LOG_FILE` | 字符串 | `""` | 审计落盘文件绝对/相对路径（为空时禁用落盘） |
| `AIGATE_AUDIT_MAX_SIZE_MB` | 整数 | `100` | 单个审计文件上限大小 (MB) |
| `AIGATE_AUDIT_MAX_BACKUPS` | 整数 | `5` | 轮转历史文件最大保留份数 |
| `AIGATE_AUDIT_WEBHOOK_URL` | 字符串 | `""` | 安全告警接收端 HTTP/HTTPS URL（为空时禁用推送） |
| `AIGATE_AUDIT_WEBHOOK_FORMAT` | 字符串 | `"standard"` | Webhook 格式：`standard`, `feishu`, `dingtalk`, `wechat_work` |
| `AIGATE_AUDIT_MAX_PROMPT_LEN`| 整数 | `4096` | 违规现场保留的 Prompt 最大截断字符数 |
| `AIGATE_AUDIT_SAMPLE_RATE` | 浮点数 | `1.0` | 正常流量（INFO）采样率 (0.0~1.0)；**违规流量恒为 100%** |

---

## 6. 系统韧性与优雅停机

1. **磁盘满保护**：当 `fwrite` 返回错误或磁盘写满时，Worker 记录限频 Warning 日志并进入 5s 保护休眠，禁止无限报错刷屏，杜绝主进程崩溃。
2. **优雅下线排空**：网关在捕获 `SIGTERM` / `SIGINT` 时，在优雅停机窗口内（`AIGATE_DRAIN_TIMEOUT_S=15`）唤醒 Worker，清空队列中积压的存量审计事件并安全关闭文件句柄，保障下线不丢失最后一批审计日志。
3. **可观测性指标集成**：
   - `aigate_audit_events_total{severity="..."}`：各级别审计事件计数 Counter
   - `aigate_audit_dropped_total`：由于队列满丢弃的审计事件计数
   - `aigate_audit_webhook_success_total`：Webhook 成功发送计数
   - `aigate_audit_webhook_failures_total`：Webhook 失败（经重试仍放弃）计数

---

## 7. 测试与验证策略

### 7.1 单元测试 (`tests/unit/observe/test_audit_logger.c`)
- **分级与快照**：断言 INFO 请求中 `prompt_snapshot == NULL`；断言 VIOLATION 请求正确抓取截断的原始 Prompt 并正确带上规则标签。
- **并发与无锁队列**：启动 32 个并发模拟线程快速推入事件，断言数据不丢失且零内存踩踏。
- **文件轮转测试**：写入模拟大数据集，断言正确触发 `.1`, `.2` 滚动重命名，且总文件数不超过最大备份限制。
- **Webhook 序列化**：针对各模板驱动（Standard, Feishu, DingTalk, WeChat Work）分别校验生成的 JSON 报文格式。

### 7.2 集成测试与压测 (`tests/integration/test_audit_pipeline.py`)
- **端到端合规验证**：本地启动 Mock Webhook 接收服务，触发正常请求与触发护栏违规拦截，验证审计文件正确写入 NDJSON，且 Mock Webhook 准确捕获违规警报。
- **性能开销基准断言**：在开启全量审计日志时运行 `wrk`/`k6` 压测，端到端延迟增量不得高于 **50 微秒**，QPS 损耗不超过 **2%**。
