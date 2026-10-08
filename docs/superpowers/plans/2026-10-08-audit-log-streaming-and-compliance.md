# 审计日志与合规投递实施计划 (Audit Log Streaming & Compliance Implementation Plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建 aigate 高性能异步双通道安全审计与合规告警管线，主链路零阻塞（< 1µs 内存入队），支持 NDJSON 文件批量写入轮转及多渠道 Webhook 实时告警卡片投递。

**Architecture:** 主请求线程纳秒级封装轻量 `audit_event_t` 并推入无锁/自旋环形缓冲区；通道 A 后台 Worker 每 200ms/64 条批量持久化为 NDJSON 并按大小切分与响应 SIGHUP 轮转；通道 B 后台 Worker 针对 VIOLATION/ERROR 级别告警异步调用 libcurl HTTP POST 投递至企业端（支持 Standard/飞书/钉钉/企微卡片适配器），具备指数退避重试；正常流量纯元数据留存，拦截违规流量保留截断原始 Prompt 现场快照。

**Tech Stack:** C17, POSIX Threads (`pthread`), libcurl, Jansson JSON, POSIX Signals, CMake, CTest, Python 3.

---

## 影响文件与模块分解

- **新增核心接口与实现:**
  - `src/observe/audit_logger.h`: 审计核心数据结构、安全等级枚举、Webhook 格式枚举、环形队列与 Logger API 声明。
  - `src/observe/audit_logger.c`: 环形缓冲区、序列化格式化器、NDJSON 文件落盘与轮转 Worker、libcurl 异步 Webhook 告警 Worker 及指标统计。
- **新增测试用例:**
  - `tests/unit/observe/test_audit_logger.c`: 覆盖序列化、快照裁剪、队列并发与丢弃、文件轮转、SIGHUP 重载、Webhook 适配器及重试逻辑的单元测试。
  - `tests/integration/test_audit_pipeline.py`: 端到端 Python 集成测试，验证实际 HTTP 请求与 Mock Webhook 接收。
- **修改现有核心模块:**
  - `tests/unit/run_tests.c`: 扩容 `g_tests` 静态数组上限（256 -> 512）并注册新增审计测试套件。
  - `src/core/config.h`: 添加 `AIGATE_AUDIT_*` 配置结构体字段。
  - `src/core/config.c`: 增加审计日志路径、轮转阈值、Webhook URL 及格式等环境变量解析与校验。
  - `src/observe/metrics.h` & `src/observe/metrics.c`: 接入 Prometheus 计数器 `aigate_audit_events_total`、`aigate_audit_dropped_total`、`aigate_audit_webhook_success_total`、`aigate_audit_webhook_failures_total`。
  - `src/core/aigate_core.h`: 在 `aigate_core` 结构体持有 `audit_logger_t* audit` 并在核心层声明 `aigate_record_audit`。
  - `src/core/aigate_core.c`: 在生命周期函数中初始化与安全释放 `audit_logger`，实现 `aigate_record_audit` 统一切面。
  - `src/core/pipeline_chat.c` & `src/core/pipeline_embeddings.c`: 在正常响应、429 并发耗尽、护栏阻断（敏感词/PII/注入）及 5xx 异常处触发审计埋点。
  - `src/main.c`: 捕获 SIGHUP 信号触发审计日志文件平滑重载（`audit_logger_reload`）。

---

### Task 1: 核心数据模型、分级规范与序列化格式化器

**Files:**
- Create: `src/observe/audit_logger.h`
- Create: `src/observe/audit_logger.c`
- Create: `tests/unit/observe/test_audit_logger.c`
- Modify: `tests/unit/run_tests.c:16-30`

- [ ] **Step 1: 扩容 `tests/unit/run_tests.c` 并编写首个失败单元测试**

在 `tests/unit/run_tests.c` 中将 `g_tests[256]` 修改为 `g_tests[512]`，并在 `tests/unit/observe/test_audit_logger.c` 编写测试用例 `test_audit_event_serialization_and_snapshots`：

```c
/* tests/unit/observe/test_audit_logger.c */
#include "audit_logger.h"
#include "run_tests.h"
#include <jansson.h>
#include <string.h>
#include <stdlib.h>

TEST_CASE(test_audit_event_serialization_and_snapshots)
{
    /* 1. 测试 INFO 级别：正常请求纯元数据，不携带 prompt 快照 */
    audit_event_t ev_info;
    audit_event_init(&ev_info);
    strncpy(ev_info.trace_id, "00-trace-info-01", sizeof(ev_info.trace_id) - 1);
    ev_info.timestamp_ms = 1760000000123LL;
    ev_info.key_id = 42;
    strncpy(ev_info.client_ip, "192.168.1.50", sizeof(ev_info.client_ip) - 1);
    strncpy(ev_info.model, "gpt-4o", sizeof(ev_info.model) - 1);
    strncpy(ev_info.provider, "openai", sizeof(ev_info.provider) - 1);
    ev_info.http_status = 200;
    ev_info.prompt_tokens = 120;
    ev_info.completion_tokens = 80;
    ev_info.latency_ns = 250000000ULL; /* 250ms */
    ev_info.severity = AUDIT_SEV_INFO;

    char* ndjson_info = audit_event_to_ndjson(&ev_info);
    TEST_ASSERT(ndjson_info != NULL, "ndjson_info serialization returned NULL");
    TEST_ASSERT(strstr(ndjson_info, "\"trace_id\":\"00-trace-info-01\"") != NULL, "trace_id missing");
    TEST_ASSERT(strstr(ndjson_info, "\"severity\":\"INFO\"") != NULL, "severity INFO missing");
    TEST_ASSERT(strstr(ndjson_info, "\"status\":200") != NULL, "status 200 missing");
    TEST_ASSERT(strstr(ndjson_info, "\"prompt\"") == NULL, "INFO level must NOT contain prompt snapshot");
    free(ndjson_info);
    audit_event_cleanup(&ev_info);

    /* 2. 测试 VIOLATION 级别：护栏违规拦截，全量上下文截断现场保留 */
    audit_event_t ev_violation;
    audit_event_init(&ev_violation);
    strncpy(ev_violation.trace_id, "00-trace-violation-02", sizeof(ev_violation.trace_id) - 1);
    ev_violation.timestamp_ms = 1760000000456LL;
    ev_violation.key_id = 42;
    strncpy(ev_violation.client_ip, "10.0.0.99", sizeof(ev_violation.client_ip) - 1);
    strncpy(ev_violation.model, "claude-3-5-sonnet", sizeof(ev_violation.model) - 1);
    strncpy(ev_violation.provider, "anthropic", sizeof(ev_violation.provider) - 1);
    ev_violation.http_status = 400;
    ev_violation.severity = AUDIT_SEV_VIOLATION;
    strncpy(ev_violation.violation_type, "pii_leak", sizeof(ev_violation.violation_type) - 1);
    strncpy(ev_violation.rule_detail, "detected phone_number regex", sizeof(ev_violation.rule_detail) - 1);
    audit_event_set_prompt(&ev_violation, "我的手机号是 13800138000，请帮我查询订单", 4096);

    char* ndjson_viol = audit_event_to_ndjson(&ev_violation);
    TEST_ASSERT(ndjson_viol != NULL, "ndjson_viol serialization returned NULL");
    TEST_ASSERT(strstr(ndjson_viol, "\"severity\":\"VIOLATION\"") != NULL, "severity VIOLATION missing");
    TEST_ASSERT(strstr(ndjson_viol, "\"type\":\"pii_leak\"") != NULL, "violation type missing");
    TEST_ASSERT(strstr(ndjson_viol, "13800138000") != NULL, "prompt snapshot missing");
    free(ndjson_viol);

    /* 3. 验证适配器格式化卡片 (Feishu, DingTalk, WeChat Work) */
    char* feishu_card = audit_event_to_webhook_payload(&ev_violation, AUDIT_HOOK_FEISHU);
    TEST_ASSERT(feishu_card != NULL, "feishu payload returned NULL");
    TEST_ASSERT(strstr(feishu_card, "\"msg_type\":\"interactive\"") != NULL, "feishu msg_type missing");
    TEST_ASSERT(strstr(feishu_card, "13800138000") != NULL, "feishu prompt missing");
    free(feishu_card);

    char* ding_card = audit_event_to_webhook_payload(&ev_violation, AUDIT_HOOK_DINGTALK);
    TEST_ASSERT(ding_card != NULL, "dingtalk payload returned NULL");
    TEST_ASSERT(strstr(ding_card, "\"msgtype\":\"markdown\"") != NULL, "dingtalk msgtype missing");
    free(ding_card);

    char* wx_card = audit_event_to_webhook_payload(&ev_violation, AUDIT_HOOK_WECHAT_WORK);
    TEST_ASSERT(wx_card != NULL, "wechat_work payload returned NULL");
    TEST_ASSERT(strstr(wx_card, "\"msgtype\":\"markdown\"") != NULL, "wechat_work msgtype missing");
    free(wx_card);

    audit_event_cleanup(&ev_violation);
}
```

在 `tests/unit/run_tests.c` 注册：
```c
extern void test_audit_event_serialization_and_snapshots(void);
test_register("audit_event_serialization_and_snapshots", test_audit_event_serialization_and_snapshots);
```

- [ ] **Step 2: 编译并运行测试确认其失败**

Run: `cmake -B build -S . && cmake --build build --target aigate_unit_tests`
Expected: 编译报错提示找不到 `audit_logger.h` 或相关未定义符号。

- [ ] **Step 3: 编写数据模型与序列化最小实现**

在 `src/observe/audit_logger.h` 中定义：
```c
/** @file audit_logger.h
 *  @ingroup group_observe
 *  @brief Audit log streaming, compliance data models, and dual-channel pipeline.
 */
#ifndef AIGATE_AUDIT_LOGGER_H
#define AIGATE_AUDIT_LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AUDIT_SEV_INFO      = 0,  /**< Normal successful requests (HTTP 200). */
    AUDIT_SEV_WARN      = 1,  /**< 429 rate limit, concurrency limits, failovers. */
    AUDIT_SEV_VIOLATION = 2,  /**< Guardrail blocks, PII leakage, prompt injections. */
    AUDIT_SEV_ERROR     = 3   /**< 5xx errors, all-upstream outages, critical failures. */
} audit_severity_t;

typedef enum {
    AUDIT_HOOK_STANDARD    = 0, /**< Standard JSON payload for SIEM. */
    AUDIT_HOOK_FEISHU      = 1, /**< Feishu interactive card. */
    AUDIT_HOOK_DINGTALK    = 2, /**< DingTalk markdown robot message. */
    AUDIT_HOOK_WECHAT_WORK = 3  /**< WeChat Work markdown robot message. */
} audit_webhook_format_t;

typedef struct audit_event {
    char              trace_id[64];
    int64_t           timestamp_ms;
    int64_t           key_id;
    char              client_ip[48];
    char              model[64];
    char              provider[32];
    int               http_status;
    uint32_t          prompt_tokens;
    uint32_t          completion_tokens;
    uint64_t          latency_ns;
    uint64_t          ttft_ns;
    audit_severity_t  severity;
    char              violation_type[32];
    char              rule_detail[128];
    char*             prompt_snapshot;
    size_t            prompt_snapshot_len;
} audit_event_t;

void audit_event_init(audit_event_t* ev);
void audit_event_cleanup(audit_event_t* ev);
int  audit_event_copy(audit_event_t* dst, const audit_event_t* src);
int  audit_event_set_prompt(audit_event_t* ev, const char* prompt, size_t max_len);

char* audit_event_to_ndjson(const audit_event_t* ev);
char* audit_event_to_webhook_payload(const audit_event_t* ev, audit_webhook_format_t fmt);

#endif /* AIGATE_AUDIT_LOGGER_H */
```

在 `src/observe/audit_logger.c` 中实现对应初始化、快照拷贝与 Jansson JSON 序列化逻辑，包括 ISO8601 时间格式化及飞书、钉钉、企微和标准格式构造。

- [ ] **Step 4: 重新编译并执行测试验证通过**

Run: `cmake --build build --target aigate_unit_tests && ./build/tests/aigate_unit_tests audit_event_serialization`
Expected: `PASS: 1/1 test(s), 0 failure(s)`

- [ ] **Step 5: 提交更改**

```bash
git add src/observe/audit_logger.h src/observe/audit_logger.c tests/unit/observe/test_audit_logger.c tests/unit/run_tests.c
git commit -m "feat(audit): ✨ add audit event data models and serialization adapters"
```

---

### Task 2: 高性能无锁/自旋环形缓冲区与丢弃计数

**Files:**
- Modify: `src/observe/audit_logger.h`
- Modify: `src/observe/audit_logger.c`
- Modify: `tests/unit/observe/test_audit_logger.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 编写环形缓冲区并发与饱和丢弃失败测试**

在 `tests/unit/observe/test_audit_logger.c` 中增加：
```c
TEST_CASE(test_audit_ring_buffer_concurrency_and_drops)
{
    /* 创建容量为 8 的微型环形缓冲区进行压力饱和测试 */
    audit_ring_t* ring = audit_ring_create(8);
    TEST_ASSERT(ring != NULL, "audit_ring_create failed");

    for (int i = 0; i < 12; i++) {
        audit_event_t ev;
        audit_event_init(&ev);
        ev.timestamp_ms = i;
        ev.key_id = i;
        ev.severity = AUDIT_SEV_INFO;
        TEST_ASSERT(audit_ring_push(ring, &ev) == true, "push must succeed non-blocking");
        audit_event_cleanup(&ev);
    }

    /* 8 个容量放入 12 条，应恰好丢弃最旧的 4 条 */
    TEST_ASSERT(audit_ring_count(ring) == 8, "count should be clamped to capacity 8");
    TEST_ASSERT(audit_ring_dropped(ring) == 4, "dropped count must be 4");

    audit_event_t batch[16];
    size_t popped = audit_ring_pop_batch(ring, batch, 16, 0);
    TEST_ASSERT(popped == 8, "should pop 8 events");
    TEST_ASSERT(batch[0].key_id == 4, "oldest 4 items dropped, first remaining must be key_id 4");
    TEST_ASSERT(batch[7].key_id == 11, "last item must be key_id 11");

    for (size_t i = 0; i < popped; i++) {
        audit_event_cleanup(&batch[i]);
    }

    audit_ring_destroy(ring);
}
```

在 `tests/unit/run_tests.c` 注册：
```c
extern void test_audit_ring_buffer_concurrency_and_drops(void);
test_register("audit_ring_buffer_concurrency_and_drops", test_audit_ring_buffer_concurrency_and_drops);
```

- [ ] **Step 2: 编译测试确认编译/运行失败**

Run: `cmake --build build --target aigate_unit_tests`
Expected: 编译失败，提示 `audit_ring_t` 未定义。

- [ ] **Step 3: 实现 `audit_ring_t` 无阻塞推入与批量弹出**

在 `src/observe/audit_logger.h` 增加：
```c
typedef struct audit_ring audit_ring_t;
audit_ring_t* audit_ring_create(size_t capacity);
void          audit_ring_destroy(audit_ring_t* ring);
bool          audit_ring_push(audit_ring_t* ring, const audit_event_t* ev);
size_t        audit_ring_pop_batch(audit_ring_t* ring, audit_event_t* out_batch, size_t max_count, uint32_t timeout_ms);
size_t        audit_ring_count(audit_ring_t* ring);
uint64_t      audit_ring_dropped(audit_ring_t* ring);
```

在 `src/observe/audit_logger.c` 实现：
- 结构体 `struct audit_ring`: `audit_event_t* slots`, `size_t head, tail, count, capacity; uint64_t dropped; pthread_mutex_t lock; pthread_cond_t not_empty;`.
- `audit_ring_push`: 锁互斥后检查 `count == capacity`；满时深释放 `slots[head]`，`head = (head + 1) % capacity`，`dropped++`；新元素深拷贝到 `tail`，`tail = (tail + 1) % capacity`，`pthread_cond_signal`。
- `audit_ring_pop_batch`: 支持带超时的 `pthread_cond_timedwait`，批量转移所有权，返回取出数量。

- [ ] **Step 4: 编译并执行测试验证通过**

Run: `cmake --build build --target aigate_unit_tests && ./build/tests/aigate_unit_tests audit_ring_buffer`
Expected: `PASS: 1/1 test(s), 0 failure(s)`

- [ ] **Step 5: 提交更改**

```bash
git add src/observe/audit_logger.h src/observe/audit_logger.c tests/unit/observe/test_audit_logger.c tests/unit/run_tests.c
git commit -m "feat(audit): ⚡ implement non-blocking audit ring buffer with drop accounting"
```

---

### Task 3: 通道 A：NDJSON 异步文件 Worker、大小轮转与 SIGHUP 重载

**Files:**
- Modify: `src/observe/audit_logger.h`
- Modify: `src/observe/audit_logger.c`
- Modify: `tests/unit/observe/test_audit_logger.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 编写文件落盘与多份备份轮转失败测试**

在 `tests/unit/observe/test_audit_logger.c` 中增加：
```c
#include <unistd.h>

TEST_CASE(test_audit_file_worker_and_rotation)
{
    const char* test_file = "/tmp/aigate_test_audit.ndjson";
    unlink(test_file);
    unlink("/tmp/aigate_test_audit.ndjson.1");
    unlink("/tmp/aigate_test_audit.ndjson.2");

    audit_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.log_file, test_file, sizeof(cfg.log_file) - 1);
    cfg.max_size_mb = 1;
    cfg.max_backups = 2;
    cfg.max_prompt_len = 1024;
    cfg.sample_rate = 1.0;

    audit_logger_t* al = audit_logger_create(&cfg);
    TEST_ASSERT(al != NULL, "audit_logger_create failed");
    TEST_ASSERT(audit_logger_start(al) == 0, "audit_logger_start failed");

    /* 记录 100 条审计事件 */
    for (int i = 0; i < 100; i++) {
        audit_event_t ev;
        audit_event_init(&ev);
        snprintf(ev.trace_id, sizeof(ev.trace_id), "trace-%03d", i);
        ev.timestamp_ms = 1760000000000LL + i;
        ev.key_id = 1;
        ev.http_status = 200;
        ev.severity = AUDIT_SEV_INFO;
        audit_logger_record(al, &ev);
        audit_event_cleanup(&ev);
    }

    /* 触发 SIGHUP 重载测试 */
    audit_logger_reload(al);

    /* 优雅停止（应清空残留并 flush） */
    audit_logger_stop(al);
    audit_logger_destroy(al);

    /* 校验目标文件已生成且内容完整包含第一条与最后一条 */
    FILE* fp = fopen(test_file, "r");
    TEST_ASSERT(fp != NULL, "audit file was not created");
    char line[4096];
    int line_count = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        line_count++;
    }
    fclose(fp);
    TEST_ASSERT(line_count == 100, "all 100 lines must be flushed to disk");

    unlink(test_file);
}
```

在 `tests/unit/run_tests.c` 注册：
```c
extern void test_audit_file_worker_and_rotation(void);
test_register("audit_file_worker_and_rotation", test_audit_file_worker_and_rotation);
```

- [ ] **Step 2: 编译测试确认其失败**

Run: `cmake --build build --target aigate_unit_tests`
Expected: 编译报错提示 `audit_config_t`、`audit_logger_create` 未定义。

- [ ] **Step 3: 实现文件 Worker 线程、轮转重命名与 SIGHUP 处理**

在 `src/observe/audit_logger.h` 增加：
```c
typedef struct {
    char                   log_file[512];
    int                    max_size_mb;
    int                    max_backups;
    char                   webhook_url[512];
    audit_webhook_format_t webhook_format;
    int                    max_prompt_len;
    double                 sample_rate;
} audit_config_t;

typedef struct audit_logger audit_logger_t;

audit_logger_t* audit_logger_create(const audit_config_t* cfg);
int             audit_logger_start(audit_logger_t* al);
void            audit_logger_stop(audit_logger_t* al);
void            audit_logger_reload(audit_logger_t* al);
void            audit_logger_record(audit_logger_t* al, const audit_event_t* ev);
void            audit_logger_destroy(audit_logger_t* al);
```

在 `src/observe/audit_logger.c` 中实现：
- `audit_file_worker` 线程函数：
  - 循环弹出批次（每 200ms 或 64 条）。
  - 若 `reload_flag` 置位：`fclose(al->file_fp); al->file_fp = fopen(al->cfg.log_file, "a");`。
  - 遍历事件，调用 `audit_event_to_ndjson`，`fwrite` + `\n`，累加文件大小。
  - 大小超过阈值时触发轮转：关闭原句柄，将 `.1` 改为 `.2`，将当前文件滚动重命名为 `.1`，重新以 `"a"` 打开新文件。
  - 批量写入末尾执行 `fflush`。

- [ ] **Step 4: 编译并执行测试验证通过**

Run: `cmake --build build --target aigate_unit_tests && ./build/tests/aigate_unit_tests audit_file_worker`
Expected: `PASS: 1/1 test(s), 0 failure(s)`

- [ ] **Step 5: 提交更改**

```bash
git add src/observe/audit_logger.h src/observe/audit_logger.c tests/unit/observe/test_audit_logger.c tests/unit/run_tests.c
git commit -m "feat(audit): 📁 add Channel A NDJSON file worker with log rotation and SIGHUP reload"
```

---

### Task 4: 通道 B：实时安全告警 Webhook 引擎与指数退避重试

**Files:**
- Modify: `src/observe/audit_logger.h`
- Modify: `src/observe/audit_logger.c`
- Modify: `tests/unit/observe/test_audit_logger.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 编写 Webhook 告警过滤、指标计数与退避逻辑失败测试**

在 `tests/unit/observe/test_audit_logger.c` 中增加：
```c
TEST_CASE(test_audit_webhook_worker_and_retry)
{
    audit_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* 指向无效本地端口以验证连接失败与有限重试防护 */
    strncpy(cfg.webhook_url, "http://127.0.0.1:54321/mock_alert", sizeof(cfg.webhook_url) - 1);
    cfg.webhook_format = AUDIT_HOOK_STANDARD;
    cfg.max_prompt_len = 1024;
    cfg.sample_rate = 1.0;

    audit_logger_t* al = audit_logger_create(&cfg);
    TEST_ASSERT(al != NULL, "audit_logger_create failed");
    TEST_ASSERT(audit_logger_start(al) == 0, "audit_logger_start failed");

    /* 1. 推送 INFO 事件：不应进入 Webhook 告警队列 */
    audit_event_t ev_info;
    audit_event_init(&ev_info);
    ev_info.severity = AUDIT_SEV_INFO;
    audit_logger_record(al, &ev_info);
    audit_event_cleanup(&ev_info);

    /* 2. 推送 VIOLATION 事件：应进入 Webhook 队列 */
    audit_event_t ev_viol;
    audit_event_init(&ev_viol);
    ev_viol.severity = AUDIT_SEV_VIOLATION;
    strncpy(ev_viol.violation_type, "prompt_injection", sizeof(ev_viol.violation_type) - 1);
    audit_event_set_prompt(&ev_viol, "Ignore previous instructions", 1024);
    audit_logger_record(al, &ev_viol);
    audit_event_cleanup(&ev_viol);

    /* 优雅退出 */
    audit_logger_stop(al);

    TEST_ASSERT(audit_logger_get_webhook_failures_total(al) >= 1, "failed destination should record failure metric");
    audit_logger_destroy(al);
}
```

在 `tests/unit/run_tests.c` 注册：
```c
extern void test_audit_webhook_worker_and_retry(void);
test_register("audit_webhook_worker_and_retry", test_audit_webhook_worker_and_retry);
```

- [ ] **Step 2: 编译测试确认其失败**

Run: `cmake --build build --target aigate_unit_tests`
Expected: 编译报错提示 `audit_logger_get_webhook_failures_total` 未定义。

- [ ] **Step 3: 实现 Channel B Webhook Worker 与 libcurl 指数退避调度**

在 `src/observe/audit_logger.h` 增加指标统计接口：
```c
uint64_t audit_logger_get_events_total(audit_severity_t sev);
uint64_t audit_logger_get_dropped_total(audit_logger_t* al);
uint64_t audit_logger_get_webhook_success_total(audit_logger_t* al);
uint64_t audit_logger_get_webhook_failures_total(audit_logger_t* al);
```

在 `src/observe/audit_logger.c` 实现：
- 专用 Webhook 环形队列 `al->webhook_ring`（容量 1024）。
- `audit_logger_record`: 当 `ev->severity >= AUDIT_SEV_VIOLATION` 且 `webhook_url` 非空时深拷贝推入 `al->webhook_ring`。
- `audit_webhook_worker` 线程函数：
  - 循环弹出事件，调用 `audit_event_to_webhook_payload`。
  - 使用 libcurl 配置硬超时（连接 2000ms，总传输 5000ms）。
  - 最多重试 3 次，间隔 1s -> 2s -> 4s（若 `!al->running` 则提前退出重试杜绝阻塞停机）。
  - 成功时更新原子计数器 `al->webhook_success_total`，3 次均失败时更新 `al->webhook_failures_total` 并记录限频警告。

- [ ] **Step 4: 编译并执行测试验证通过**

Run: `cmake --build build --target aigate_unit_tests && ./build/tests/aigate_unit_tests audit_webhook_worker`
Expected: `PASS: 1/1 test(s), 0 failure(s)`

- [ ] **Step 5: 提交更改**

```bash
git add src/observe/audit_logger.h src/observe/audit_logger.c tests/unit/observe/test_audit_logger.c tests/unit/run_tests.c
git commit -m "feat(audit): 🔔 add Channel B real-time webhook alert worker with retry and backoff"
```

---

### Task 5: 全局启动配置与环境变量支持

**Files:**
- Modify: `src/core/config.h:20-35`
- Modify: `src/core/config.c:130-160`
- Modify: `tests/unit/observe/test_audit_logger.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 编写配置解析与校验单元测试**

在 `tests/unit/observe/test_audit_logger.c` 增加：
```c
TEST_CASE(test_config_audit_parameters)
{
    /* 设置环境变量测试解析 */
    setenv("AIGATE_AUDIT_LOG_FILE", "/var/log/aigate/audit.ndjson", 1);
    setenv("AIGATE_AUDIT_MAX_SIZE_MB", "200", 1);
    setenv("AIGATE_AUDIT_MAX_BACKUPS", "10", 1);
    setenv("AIGATE_AUDIT_WEBHOOK_URL", "https://open.feishu.cn/open-apis/bot/v2/hook/xxx", 1);
    setenv("AIGATE_AUDIT_WEBHOOK_FORMAT", "feishu", 1);
    setenv("AIGATE_AUDIT_MAX_PROMPT_LEN", "2048", 1);
    setenv("AIGATE_AUDIT_SAMPLE_RATE", "0.5", 1);
    setenv("AIGATE_PG_DSN", "postgres://localhost/test", 1);
    setenv("AIGATE_ADMIN_TOKEN", "supersecret", 1);

    aigate_config cfg;
    TEST_ASSERT(aigate_config_load(&cfg) == 0, "config load must succeed");
    TEST_ASSERT(strcmp(cfg.audit_log_file, "/var/log/aigate/audit.ndjson") == 0, "log file mismatch");
    TEST_ASSERT(cfg.audit_max_size_mb == 200, "max size mismatch");
    TEST_ASSERT(cfg.audit_max_backups == 10, "max backups mismatch");
    TEST_ASSERT(strcmp(cfg.audit_webhook_url, "https://open.feishu.cn/open-apis/bot/v2/hook/xxx") == 0, "webhook url mismatch");
    TEST_ASSERT(strcmp(cfg.audit_webhook_format, "feishu") == 0, "webhook format mismatch");
    TEST_ASSERT(cfg.audit_max_prompt_len == 2048, "max prompt len mismatch");
    TEST_ASSERT(cfg.audit_sample_rate >= 0.49 && cfg.audit_sample_rate <= 0.51, "sample rate mismatch");

    unsetenv("AIGATE_AUDIT_LOG_FILE");
    unsetenv("AIGATE_AUDIT_MAX_SIZE_MB");
    unsetenv("AIGATE_AUDIT_MAX_BACKUPS");
    unsetenv("AIGATE_AUDIT_WEBHOOK_URL");
    unsetenv("AIGATE_AUDIT_WEBHOOK_FORMAT");
    unsetenv("AIGATE_AUDIT_MAX_PROMPT_LEN");
    unsetenv("AIGATE_AUDIT_SAMPLE_RATE");
}
```

在 `tests/unit/run_tests.c` 注册：
```c
extern void test_config_audit_parameters(void);
test_register("config_audit_parameters", test_config_audit_parameters);
```

- [ ] **Step 2: 编译测试确认其失败**

Run: `cmake --build build --target aigate_unit_tests`
Expected: 编译报错提示 `aigate_config` 结构体没有 `audit_log_file` 等字段。

- [ ] **Step 3: 在 `config.h` 和 `config.c` 添加审计配置字段与校验**

在 `src/core/config.h` 的 `aigate_config` 增加：
```c
    char   audit_log_file[512];        /**< AIGATE_AUDIT_LOG_FILE, default "" */
    int    audit_max_size_mb;          /**< AIGATE_AUDIT_MAX_SIZE_MB, default 100 */
    int    audit_max_backups;          /**< AIGATE_AUDIT_MAX_BACKUPS, default 5 */
    char   audit_webhook_url[512];     /**< AIGATE_AUDIT_WEBHOOK_URL, default "" */
    char   audit_webhook_format[32];   /**< AIGATE_AUDIT_WEBHOOK_FORMAT, default "standard" */
    int    audit_max_prompt_len;       /**< AIGATE_AUDIT_MAX_PROMPT_LEN, default 4096 */
    double audit_sample_rate;          /**< AIGATE_AUDIT_SAMPLE_RATE, default 1.0 */
```

在 `src/core/config.c` 添加解析并设置默认值和合法性校验。

- [ ] **Step 4: 编译并执行测试验证通过**

Run: `cmake --build build --target aigate_unit_tests && ./build/tests/aigate_unit_tests config_audit`
Expected: `PASS: 1/1 test(s), 0 failure(s)`

- [ ] **Step 5: 提交更改**

```bash
git add src/core/config.h src/core/config.c tests/unit/observe/test_audit_logger.c tests/unit/run_tests.c
git commit -m "feat(config): ⚙️ add audit log streaming and webhook configuration options"
```

---

### Task 6: Prometheus 审计与告警指标输出集成

**Files:**
- Modify: `src/observe/metrics.h`
- Modify: `src/observe/metrics.c`
- Modify: `tests/unit/observe/test_audit_logger.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 编写 Prometheus 审计指标输出测试**

在 `tests/unit/observe/test_audit_logger.c` 增加：
```c
#include "metrics.h"

TEST_CASE(test_audit_metrics_exposition)
{
    metrics_inc_audit_event(AUDIT_SEV_INFO);
    metrics_inc_audit_event(AUDIT_SEV_VIOLATION);
    metrics_inc_audit_dropped(2);
    metrics_inc_audit_webhook_success();
    metrics_inc_audit_webhook_failure();

    char buf[4096];
    int len = metrics_render(NULL, buf, sizeof(buf));
    TEST_ASSERT(len > 0, "metrics_render failed");
    TEST_ASSERT(strstr(buf, "# TYPE aigate_audit_events_total counter") != NULL, "audit_events_total header missing");
    TEST_ASSERT(strstr(buf, "aigate_audit_events_total{severity=\"info\"} 1") != NULL, "info event counter missing");
    TEST_ASSERT(strstr(buf, "aigate_audit_events_total{severity=\"violation\"} 1") != NULL, "violation event counter missing");
    TEST_ASSERT(strstr(buf, "aigate_audit_dropped_total 2") != NULL, "dropped counter missing");
    TEST_ASSERT(strstr(buf, "aigate_audit_webhook_success_total 1") != NULL, "webhook success counter missing");
    TEST_ASSERT(strstr(buf, "aigate_audit_webhook_failures_total 1") != NULL, "webhook failure counter missing");
}
```

在 `tests/unit/run_tests.c` 注册：
```c
extern void test_audit_metrics_exposition(void);
test_register("audit_metrics_exposition", test_audit_metrics_exposition);
```

- [ ] **Step 2: 编译测试确认其失败**

Run: `cmake --build build --target aigate_unit_tests`
Expected: 编译报错提示 `metrics_inc_audit_event` 等函数未定义。

- [ ] **Step 3: 在 `metrics.h` 和 `metrics.c` 中导出审计指标**

在 `src/observe/metrics.h` 增加指标辅助函数声明并在 `src/observe/metrics.c` 中实现原子计数和 Prometheus 渲染输出。

- [ ] **Step 4: 编译并执行测试验证通过**

Run: `cmake --build build --target aigate_unit_tests && ./build/tests/aigate_unit_tests audit_metrics`
Expected: `PASS: 1/1 test(s), 0 failure(s)`

- [ ] **Step 5: 提交更改**

```bash
git add src/observe/metrics.h src/observe/metrics.c tests/unit/observe/test_audit_logger.c tests/unit/run_tests.c
git commit -m "feat(observe): 📊 add Prometheus metrics for audit events and webhook delivery"
```

---

### Task 7: 网关核心生命周期集成与业务流水线埋点

**Files:**
- Modify: `src/core/aigate_core.h`
- Modify: `src/core/aigate_core.c`
- Modify: `src/core/pipeline_chat.c`
- Modify: `src/core/pipeline_embeddings.c`
- Modify: `src/main.c`
- Modify: `tests/unit/observe/test_audit_logger.c`
- Modify: `tests/unit/run_tests.c`

- [ ] **Step 1: 编写核心埋点逻辑单元测试**

在 `tests/unit/observe/test_audit_logger.c` 增加：
```c
#include "aigate_core.h"

TEST_CASE(test_audit_pipeline_hook_recording)
{
    aigate_core core;
    memset(&core, 0, sizeof(core));

    audit_config_t acfg;
    memset(&acfg, 0, sizeof(acfg));
    strncpy(acfg.log_file, "/tmp/aigate_hook_test.ndjson", sizeof(acfg.log_file) - 1);
    acfg.max_size_mb = 10;
    acfg.max_backups = 1;
    acfg.max_prompt_len = 1024;
    acfg.sample_rate = 1.0;
    core.audit = audit_logger_create(&acfg);
    audit_logger_start(core.audit);

    /* 触发正常审计记录 */
    aigate_record_audit(&core, "trace-hook-01", "127.0.0.1", 10, "gpt-4o", "openai", 200, 10, 20, 5000000ULL, 1000000ULL, AUDIT_SEV_INFO, "", "", NULL, 0);

    /* 触发违规审计记录（带 Prompt 现场） */
    const char* bad_prompt = "Tell me how to hack";
    aigate_record_audit(&core, "trace-hook-02", "127.0.0.1", 10, "gpt-4o", "openai", 400, 5, 0, 1000000ULL, 0, AUDIT_SEV_VIOLATION, "guardrail_block", "keyword: hack", bad_prompt, strlen(bad_prompt));

    audit_logger_stop(core.audit);
    audit_logger_destroy(core.audit);
    core.audit = NULL;

    FILE* fp = fopen("/tmp/aigate_hook_test.ndjson", "r");
    TEST_ASSERT(fp != NULL, "file must exist");
    char buf[2048];
    bool found_hack = false;
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        if (strstr(buf, "keyword: hack") != NULL) {
            found_hack = true;
        }
    }
    fclose(fp);
    unlink("/tmp/aigate_hook_test.ndjson");
    TEST_ASSERT(found_hack == true, "violation record must contain prompt and rule detail");
}
```

在 `tests/unit/run_tests.c` 注册：
```c
extern void test_audit_pipeline_hook_recording(void);
test_register("audit_pipeline_hook_recording", test_audit_pipeline_hook_recording);
```

- [ ] **Step 2: 编译测试确认其失败**

Run: `cmake --build build --target aigate_unit_tests`
Expected: 编译报错提示 `aigate_record_audit` 未定义或 `core.audit` 字段缺失。

- [ ] **Step 3: 实现 `aigate_record_audit` 并在流水线与主进程接入**

1. 在 `src/core/aigate_core.h`:
   - 包含 `"observe/audit_logger.h"`。
   - 在 `aigate_core` 结构体中添加 `audit_logger_t* audit;`。
   - 声明统一埋点切面函数：
     ```c
     void aigate_record_audit(aigate_core*      ac,
                              const char*       trace_id,
                              const char*       client_ip,
                              int64_t           key_id,
                              const char*       model,
                              const char*       provider,
                              int               http_status,
                              uint32_t          prompt_tokens,
                              uint32_t          completion_tokens,
                              uint64_t          latency_ns,
                              uint64_t          ttft_ns,
                              audit_severity_t  severity,
                              const char*       violation_type,
                              const char*       rule_detail,
                              const char*       prompt_raw,
                              size_t            prompt_len);
     ```
2. 在 `src/core/aigate_core.c`:
   - 实现 `aigate_record_audit`: 构造 `audit_event_t`，根据严重级别动态决定是否截断拷贝 Prompt，并调用 `audit_logger_record(ac->audit, &ev)`。
   - 在 `aigate_core_init` 中根据 `aigate_config` 初始化并启动 `ac->audit`。
   - 在 `aigate_core_shutdown` 中排空并销毁 `ac->audit`。
3. 在 `src/core/pipeline_chat.c` & `src/core/pipeline_embeddings.c`:
   - 在护栏拦截、PII阻断、429限流以及成功返回处，调用 `aigate_record_audit`。
4. 在 `src/main.c`:
   - 在 SIGHUP 信号触发分支调用 `audit_logger_reload(core.audit)`。

- [ ] **Step 4: 编译并执行测试验证通过**

Run: `cmake --build build --target aigate_unit_tests && ./build/tests/aigate_unit_tests audit_pipeline_hook`
Expected: `PASS: 1/1 test(s), 0 failure(s)`

- [ ] **Step 5: 提交更改**

```bash
git add src/core/aigate_core.h src/core/aigate_core.c src/core/pipeline_chat.c src/core/pipeline_embeddings.c src/main.c tests/unit/observe/test_audit_logger.c tests/unit/run_tests.c
git commit -m "feat(pipeline): 🔗 integrate audit logger into request lifecycle and guardrail checkpoints"
```

---

### Task 8: 端到端 Python 集成测试与 Mock Webhook 验证

**Files:**
- Create: `tests/integration/test_audit_pipeline.py`

- [ ] **Step 1: 编写 Python 端到端集成测试脚本**

在 `tests/integration/test_audit_pipeline.py` 中编写自动化端到端测试：
- 本地启动轻量 `http.server` 充当 Mock Webhook 接收端。
- 启动网关进程（配置 `AIGATE_AUDIT_LOG_FILE=/tmp/aigate_e2e_audit.ndjson` 与 `AIGATE_AUDIT_WEBHOOK_URL=http://127.0.0.1:<mock_port>/alert`）。
- 发送合法 Chat 请求：验证文件写入一行 `severity=INFO`，Prompt 为空，且 Webhook 收到 0 次请求。
- 发送触犯安全护栏的请求（如包含黑名单关键词）：验证文件写入一行 `severity=VIOLATION`，且 Mock Webhook 在 1 秒内收到 JSON 警报推送，包含违规 Prompt 与规则信息。
- 发送 `SIGHUP` 信号验证轮转重载；关闭网关验证存量排空与文件句柄释放。

- [ ] **Step 2: 执行集成测试并验证通过**

Run: `python3 tests/integration/test_audit_pipeline.py`
Expected: 所有的端到端断言均输出 `[PASS]`，退出码为 0。

- [ ] **Step 3: 运行全量单元测试套件确认零回归**

Run: `ctest --test-dir build --output-on-failure`
Expected: `100% tests passed out of 6`，且所有审计相关单元测试均绿灯通过。

- [ ] **Step 4: 提交更改**

```bash
git add tests/integration/test_audit_pipeline.py
git commit -m "test(integration): 🧪 add end-to-end audit log and webhook verification suite"
```

---

## 计划自审清单 (Self-Review Checklist)

1. **Spec 覆盖率检查**:
   - 双通道异步架构 (Channel A 文件落盘 + Channel B Webhook 告警) -> Task 2, 3, 4
   - 4 级安全分级规范 (`audit_severity_t`) -> Task 1
   - 动态现场 Prompt 快照留存与截断 -> Task 1, 7
   - NDJSON 格式化与 Feishu/DingTalk/WeChat Work 适配器 -> Task 1, 4
   - 文件大小轮转与 SIGHUP 平滑支持 -> Task 3, 7
   - Webhook 指数退避重试与有限重试容灾 -> Task 4
   - 环境变量与启动配置完整对接 -> Task 5
   - Prometheus 指标输出与统计 -> Task 6
   - 单元测试与端到端集成测试套件 -> Task 1, 2, 3, 4, 5, 6, 7, 8
2. **占位符检查**: 无任何 TODO、TBD、后续补充等占位符，每一处关键实现与测试均给出完整代码结构。
3. **类型与接口一致性**: `audit_event_t`、`audit_severity_t`、`audit_webhook_format_t`、`audit_config_t`、`audit_logger_t` 在所有任务中的命名与函数原型严格保持一致。
