# Design Spec: AI-Native Threat Defense & Watermarking Suite v2

**Author:** AI Pair Programmer & Antigravity  
**Status:** Approved  
**Date:** 2026-10-10  
**Target Milestone:** v2.0 Enterprise Security & Forensics Hardening  

---

## 1. Executive Summary & Goals

在首期 AI 威胁防御与隐写水印套件（v1）成功上线并全量通过 CI/CD 质量门禁的基础上，本规格说明书针对企业实际生产与红蓝对抗中的典型痛点展开第二阶段深度增强（v2）：

1. **标点自适应多片冗余水印（Punctuation-Anchored Multi-Tile & Sliding-Window Decoding）**：
   - 解决单片水印易受片段剪裁、局部文本删改导致的溯源失效问题；
   - 沿用成熟的 18 字节（72 个零宽字符）紧凑安全帧，自适应锚定在句号、逗号、问号、叹号、换行等天然断句后多次周期性嵌入；
   - 提取端采用滑动窗口多片容错算法，只要攻击者复制任意一句包含完整单片的文本片段，即可 100% 还原泄漏者 Key ID 与时间戳。
2. **流式 Chunk 逐包分散平滑打标（Streaming Distributed Chunk-Level Watermarking）**：
   - 消除流式输出中首包强制攒包打标的隐患，实现首字延迟（TTFT）**零损耗（0 Overhead）**；
   - 状态机依附流式 SSE chunk 词法输出事件，在遇到断句标点时平滑附带零宽单片。
3. **Canary 蜜罐诱捕与自适应内存 IP 封禁（Honey-Tokens & In-Memory IP Ban Table）**：
   - 支持通过 Admin API 创建带有 `is_canary=true` 标记的诱饵凭据，部署于外部文档或测试环境；
   - 一旦攻击者使用 Canary Key 发起调用，系统立即触发 `AUDIT_SEV_CRITICAL` 告警，伪装返回 401，并自动将调用方源 IP 录入内置高并发内存黑名单（`ip_ban_table_t`）；
   - 在连接入口执行微秒级 O(1) 阻断，返回 HTTP 403 Forbidden，保护核心推理算力，支持可选的 Redis 集群广播同步。
4. **动态安全白名单与规则例外机制（Dynamic Rule Whitelist & Bypass Policy）**：
   - 解决业务合规、自动化测试或红队授权渗透时的误报拦截问题；
   - 支持通过 Admin API / Web Console 配置基于 `key_id`、`model`、`rule_tag` 的豁免规则；
   - 内存规则快速求值（<1μs），数据库持久化，平滑放行特权流量并记录免检审计日志。

---

## 2. Architecture & Module Design

```
                          [ Client Request ]
                                  │
                                  ▼
      ┌────────────────────────────────────────────────────────┐
      │ 1. IP 准入防御层 (src/policy/ip_ban_table.c)           │
      │    - 4096 分桶并发读写锁哈希表                          │ ──> 命中黑名单? ──> 立即 403 Forbidden
      │    - 零拷贝微秒级 O(1) 判定 / TTL 自动淘汰             │
      └────────────────────────────────────────────────────────┘
                                  │ 未封禁放行
                                  ▼
      ┌────────────────────────────────────────────────────────┐
      │ 2. 身份认证与蜜罐识别 (src/policy/auth_key.c)          │
      │    - 校验 API Key 状态                                 │ ──> 命中 Canary Key? ──> 自动封禁 IP
      │    - 识别 is_canary 属性                               │     & 记录 CRITICAL 审计 & 返回伪装 401
      └────────────────────────────────────────────────────────┘
                                  │ 正常密钥
                                  ▼
      ┌────────────────────────────────────────────────────────┐
      │ 3. 进站过滤与白名单豁免 (src/policy/filter_chain.c)     │
      │    - 动态白名单匹配器 (threat_whitelist.c)             │ ──> 命中白名单? ──> 放行 bypass
      │    - 纯 C 启发式越狱检测 (jailbreak_detector.c)        │ ──> 未命中但违规 ──> 拦截 400
      └────────────────────────────────────────────────────────┘
                                  │ 通过安全策略
                                  ▼
                        [ Upstream Model LLM ]
                                  │
                                  ▼
      ┌────────────────────────────────────────────────────────┐
      │ 4. 出站响应与流式分散打标 (src/policy/watermark_engine) │
      │    - 非流式：标点断句多片自适应锚定注入 (Multi-Tile)    │
      │    - 流式：SSE Chunk 逐包状态机平滑混入                │
      │    - 防篡改连续审计哈希链记录 (audit_hash_chain.c)     │
      └────────────────────────────────────────────────────────┘
                                  │
                                  ▼
                         [ Client Response ]
```

---

## 3. Data Structures & Core Algorithms

### 3.1 标点自适应多片冗余水印（Multi-Tile Watermark）

#### (1) 紧凑单片（Tile Frame）
单片为 18 字节（144 比特）二进制结构：
- `Magic (16b)`: `0x574D` ('WM')
- `Key ID (64b)`: 调用方客户端 ID
- `Timestamp (32b)`: Unix 时间戳秒
- `Short Trace (16b)`: 链路短标识
- `CRC-16 (16b)`: CCITT 多项式 `0x1021` 校验码

由 4 种零宽字符 `U+200B`（00）、`U+200C`（01）、`U+200D`（10）、`U+FEFF`（11）编码为 **72 个 UTF-8 零宽字符**（字节数 216 字节，视觉宽度 0）。

#### (2) 标点锚定多片注入算法
1. 定义中英文断句标点集合：`。`、`！`、`？`、`；`、`\n`、`.`、`!`、`?`、`;`。
2. 扫描文本正文，统计自上次打标以来的字符间距 `chars_since_last_tile`：
   - 设定最小打标间距 `MIN_TILE_INTERVAL_CHARS = 80`；
   - 当遇到断句标点且 `chars_since_last_tile >= MIN_TILE_INTERVAL_CHARS` 时，紧随标点后追加 72 字符零宽单片；
   - 若全文遍历结束且注入片数 `tiles_injected == 0`（如极短回复或无标点），在文本末尾保底注入 1 个单片。

#### (3) 滑动窗口多片解码器（Sliding-Window Decoder）
解码器 [`watermark_decode`](file:///home/quintin/Data/source/c_cpp/aigate/src/policy/watermark_engine.c) 升级为滑动窗口搜索：
1. 从待查输入提取零宽字符流（若无零宽字符直接返回 `found = false`）；
2. 以 72 字符为窗口逐字符滑动搜索：
   - 若窗口前 8 字符解码出 `0x574D`，则提取后续 64 字符并计算 CRC-16；
   - CRC-16 匹配成功即确认为 1 个有效单片，记录 payload 并跳跃 72 字符继续搜索；
   - 统计全部有效单片数 `tiles_detected`。只要提取到至少 1 个有效单片，即宣告溯源成功（`found = true`）。

---

### 3.2 流式 Chunk 逐包平滑注入状态机

针对 SSE 流式管道（`pipeline_chat.c` / `transport_civetweb.c`）：

```c
typedef struct stream_watermark_state {
    watermark_payload_t payload;
    char                tile_zw_buf[256]; /* 预先编码生成的 72 字符零宽字节流 */
    size_t              tile_zw_len;
    size_t              chars_since_last_tile;
    int                 tiles_emitted;
    bool                enabled;
} stream_watermark_state_t;
```

**处理逻辑**：
- 在流式连接建立并判定目标 Key 启用水印后，初始化 `stream_watermark_state_t` 并预编码 `tile_zw_buf`；
- 上游返回每个 chunk 时，解析 `choices[0].delta.content` 文本：
  - 若文本包含断句标点且 `chars_since_last_tile >= 80`：在标点后拼装插入 `tile_zw_buf`，重置计数；
  - 否则累加 `chars_since_last_tile`；
- 若流式结束（`[DONE]`）且 `tiles_emitted == 0`，向最后一个 delta chunk 追加单片后发送。

---

### 3.3 高并发内置 IP 黑名单引擎（`ip_ban_table_t`）

在 [`src/policy/ip_ban_table.h`](file:///home/quintin/Data/source/c_cpp/aigate/src/policy/ip_ban_table.h) 中定义：

```c
typedef struct ip_ban_entry {
    char                 ip[48];
    int64_t              expires_at_sec;
    char                 reason[64];
    uint32_t             hit_count;
    struct ip_ban_entry* next;
} ip_ban_entry_t;

typedef struct ip_ban_table {
    ip_ban_entry_t*  buckets[4096];
    pthread_rwlock_t rwlock;
    size_t           count;
} ip_ban_table_t;
```

**关键 API**：
- `ip_ban_table_t* ip_ban_table_create(void);`
- `void ip_ban_table_destroy(ip_ban_table_t* tbl);`
- `bool ip_ban_table_is_banned(ip_ban_table_t* tbl, const char* ip, char* out_reason, size_t cap);`
- `int ip_ban_table_ban(ip_ban_table_t* tbl, const char* ip, int64_t ttl_sec, const char* reason);`
- `int ip_ban_table_unban(ip_ban_table_t* tbl, const char* ip);`
- `size_t ip_ban_table_list(ip_ban_table_t* tbl, ip_ban_info_t* out_arr, size_t max_count);`

**接入点**：Civetweb `begin_request_handler` 第一行，命中使用 `mg_send_http_error(conn, 403, "Access Forbidden: IP temporarily blocked")` 立即中断。

---

### 3.4 动态安全白名单匹配器（`threat_whitelist_t`）

在 [`src/policy/threat_whitelist.h`](file:///home/quintin/Data/source/c_cpp/aigate/src/policy/threat_whitelist.h) 中定义：

```c
typedef struct threat_whitelist_rule {
    int64_t  rule_id;
    char     name[64];
    uint64_t match_key_id;     /* 0: wildcard */
    char     match_model[64];  /* "": wildcard */
    char     bypass_rule_tag[64]; /* "*": all */
    char     reason[128];
    int64_t  expires_at_sec;   /* 0: no expiry */
    bool     enabled;
} threat_whitelist_rule_t;

typedef struct threat_whitelist {
    threat_whitelist_rule_t rules[256];
    size_t                  rule_count;
    pthread_rwlock_t        rwlock;
} threat_whitelist_t;
```

**求值逻辑**：
```c
bool threat_whitelist_is_bypassed(const threat_whitelist_t* tw,
                                 uint64_t                  key_id,
                                 const char*               model,
                                 const char*               rule_tag);
```
在 [`filter_chain.c`](file:///home/quintin/Data/source/c_cpp/aigate/src/policy/filter_chain.c) 中：当 `jailbreak_detector_scan` 检出违规时，先调用 `threat_whitelist_is_bypassed`；若返回 `true`，则记一条 INFO 审计并放行流量。

---

## 4. Database Schema Migration v18

在 `schema/schema.sql` 与 `src/store/schema_sql.h` 中追加：

```sql
-- Migration v18: Canary honey-tokens and dynamic threat defense whitelists
ALTER TABLE api_keys
  ADD COLUMN IF NOT EXISTS is_canary BOOLEAN NOT NULL DEFAULT false;

CREATE TABLE IF NOT EXISTS threat_rule_whitelists (
  rule_id         BIGSERIAL PRIMARY KEY,
  name            VARCHAR(64) NOT NULL,
  match_key_id    BIGINT NOT NULL DEFAULT 0,
  match_model     VARCHAR(64) NOT NULL DEFAULT '',
  bypass_rule_tag VARCHAR(64) NOT NULL DEFAULT '*',
  reason          TEXT NOT NULL DEFAULT '',
  enabled         BOOLEAN NOT NULL DEFAULT true,
  expires_at      TIMESTAMPTZ,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_threat_whitelists_lookup 
  ON threat_rule_whitelists (match_key_id, match_model, enabled);

CREATE TABLE IF NOT EXISTS ip_bans_persistent (
  ip              VARCHAR(48) PRIMARY KEY,
  reason          VARCHAR(64) NOT NULL,
  expires_at      TIMESTAMPTZ NOT NULL,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

INSERT INTO schema_migrations(version) VALUES (18) ON CONFLICT (version) DO NOTHING;
```

---

## 5. Admin REST API Specification

| 路径 | 方法 | 权限 | 说明 |
| :--- | :--- | :--- | :--- |
| `/admin/v1/security/ip-bans` | `GET` | Admin Token | 查询当前活跃封禁 IP 列表与统计 |
| `/admin/v1/security/ip-bans` | `POST` | Admin Token | 手动添加封禁 IP：`{"ip": "1.2.3.4", "ttl_sec": 3600, "reason": "manual"}` |
| `/admin/v1/security/ip-bans/{ip}` | `DELETE` | Admin Token | 手动解封指定 IP |
| `/admin/v1/threats/whitelist` | `GET` | Admin Token | 获取全部安全规则豁免白名单 |
| `/admin/v1/threats/whitelist` | `POST` | Admin Token | 创建白名单规则：`{"name": "...", "match_key_id": 1, ...}` |
| `/admin/v1/threats/whitelist/{id}`| `DELETE` | Admin Token | 删除/禁用指定白名单规则 |
| `/admin/v1/keys` | `POST` | Admin Token | 支持新建 Canary 蜜罐 Key（`"is_canary": true`） |

---

## 6. Web Console UI & Visual Forensics

在 [`web/admin.html`](file:///home/quintin/Data/source/c_cpp/aigate/web/admin.html) 安全面板中：
1. **Canary 蜜罐卡片**：展示当前存活 Canary 诱饵密钥数量，提供一键生成诱饵 Key 模态框，列表标注醒目的金色蜜罐图标；
2. **IP 黑名单看板**：展示被封禁 IP 实时表格、剩余 TTL 倒计时、拦截计数及“解封”按钮；
3. **白名单配置中心**：表单化配置白名单规则并支持生效倒计时；
4. **增强水印取证工作台**：
   - 提取结果展示 `Tiles Detected: N`、`Integrity: 100% Valid`；
   - 原文预览区高亮标记出隐藏单片的标点锚定位置。

---

## 7. Metrics & Observability

- `aigate_ip_bans_active` (Gauge): 当前内存中处于封禁状态的 IP 总数。
- `aigate_ip_bans_total{reason}` (Counter): 累计执行封禁次数（按 `canary`、`manual` 分类）。
- `aigate_ip_ban_blocked_requests_total` (Counter): 累计在请求入口 403 阻断的恶意请求数。
- `aigate_threat_whitelist_bypassed_total{rule_tag}` (Counter): 命中白名单放行的请求数。
- `aigate_watermark_multi_tiles_injected_total` (Counter): 累计注入的零宽单片总数。

---

## 8. Testing & Quality Gates

1. **C 单元测试套件 (`tests/unit/`)**：
   - `unit/policy/test_watermark_multi_tile.c`: 覆盖多片标点自适应打标、任意单句片段提取及滑窗容错。
   - `unit/policy/test_ip_ban_table.c`: 覆盖并发读写锁、分桶哈希碰撞、TTL 懒释放及 API 列表序列化。
   - `unit/policy/test_threat_whitelist.c`: 覆盖多维度通配符匹配及边界条件。
   - `unit/server/test_admin_security_api.c`: 覆盖新增 IP 封禁与白名单 REST 端点。
2. **Python E2E 集成测试 (`tests/integration/test_ai_threat_defense_and_watermarking.py`)**：
   - `test_watermark_arbitrary_excerpt_clipping_e2e`: 生成长文，随机截取中间一句话送入解码 API，断言准确识别 Key ID。
   - `test_stream_watermark_sse_injection_e2e`: 验证流式 chunk 中平滑混入零宽字符且拼接后可完整溯源。
   - `test_canary_honey_token_and_ip_ban_e2e`: 验证调用 Canary Key 立即触发源 IP 403 封禁，并在解封后恢复。
   - `test_threat_whitelist_bypass_e2e`: 验证在白名单保护下的越狱测试流量正常通过并交付 200 OK。
3. **质量标准**：
   - 全量代码符合 `-Wall -Wextra -Werror`；
   - 格式完全符合 Ubuntu 24.04 `clang-format 18` 标准；
   - 287+ 单元测试与全部 Python 集成测试 100% 通过；
   - GitHub Actions CI 三矩阵任务全部绿灯。
