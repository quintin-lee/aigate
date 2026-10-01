# 数据脱敏与安全风控升级 (PII Masking & Advanced Guardrails) 设计规范

- **状态**: 草案 (Draft / Pending Approval)
- **创建日期**: 2026-10-01
- **责任模块**: `src/policy/guardrails.h`, `src/policy/guardrails.c`, `src/core/pipeline_chat.c`, `src/server/admin_api.c`, `web/admin.html`
- **目标**: 构建支持双向无感去标识化、高精度校验和算法（Luhn/MOD 11-2）、多级策略（可逆还原/掩码/擦除/阻断）及在线演练沙箱的企业级数据脱敏与安全风控体系。

---

## 1. 背景与目标 (Context & Objectives)

在企业接入公网商业大模型（OpenAI、Anthropic、Google 等）的过程中，提示词中常无意包含用户隐私或企业机密（如手机号、身份证号、银行卡号、API 密钥等）。若直接发送至上游，将面临严重的合规法律风险（如《个人信息保护法》、GDPR、PCI-DSS）。

传统单向打码存在两大严重缺陷：
1. **误报率高**：普通 16/18 位订单号或长数值极易被误判为银行卡或身份证号；
2. **体验割裂**：模型回复直接输出 `[PHONE]` 等占位符，终端业务系统（如智能客服、工单跟进）无法直接使用回复。

本项目旨在为 aigate 引入：
1. **高精度校验算法**：结合正则与数学校验和（银行卡 Luhn 模 10、居民身份证 ISO 7064:1983.MOD 11-2 校验和），将误报率降至极低；
2. **双向无感去标识化与流式还原流水线**：入站阶段去标识化为 Token 占位符发往公网大模型，出站阶段（支持 SSE 流式跨包滑动窗口）实时逆向还原为真实数据，公网零泄露的同时实现用户无感体验；
3. **多级动作策略**：支持按实体独立配置「双向还原」、「部分掩码 (如 138****1234)」、「标签擦除 ([PHONE])」与「直接阻断 (Block)」；
4. **可视化控制台与沙箱演练**：在 `web/admin.html` 中提供直观规则配置面板与实时脱敏效果验证沙箱。

---

## 2. 总体架构与数据流 (Architecture & Data Flow)

```
[ 客户端 Client ]
       │
       ▼ (1) 原始输入: "发短信给 13812345678 查银行卡 6222021234567890"
[ aigate 入站去标识化 (Inbound PII Engine) ]
       │   • 正则识别 + Luhn/MOD 11-2 校验
       │   • 策略匹配: 手机号->双向还原, 银行卡->部分掩码
       │   • 会话映射表: [PHONE_1] -> 13812345678
       ▼ (2) 脱敏提示词: "发短信给 [PHONE_1] 查银行卡 622202******7890"
[ 公网大模型 (Upstream LLM: OpenAI / Claude) ]
       │   • 零隐私泄漏，公网合规出境
       ▼ (3) 大模型回复: "已向 [PHONE_1] 发送账单" (SSE 流式或非流式)
[ aigate 出站实时还原 (Outbound De-anonymizer) ]
       │   • 滑动窗口缓存，解决 [PHONE_1] 跨 SSE 包切割问题
       │   • 逆向查表还原: [PHONE_1] -> 13812345678
       ▼ (4) 最终响应: "已向 13812345678 发送账单"
[ 客户端 Client 接收自然连贯回答 ]
```

---

## 3. 详细设计 (Detailed Specifications)

### 3.1 敏感实体高精度检测与校验规则

| 实体标识 | 实体名称 | 检测特征与精准校验算法 | 典型示例 |
|---|---|---|---|
| `phone` | 手机号码 | 国内 11 位号段 (`1[3-9]\d{9}`) + 国际 E.164 (`\+[1-9]\d{1,14}`) | `13812345678`, `+8613812345678` |
| `id_card` | 居民身份证 | 18 位大陆身份证号，**内置 ISO 7064:1983.MOD 11-2 加权和余数校验码校验** | `110101199003072378` (有效通过，随机数被过滤) |
| `bank_card` | 银行卡/信用卡 | 13~19 位纯数字，**内置 Luhn 模 10 校验和算法** | `6222021234567890` (有效卡通过，订单号被过滤) |
| `email` | 电子邮箱 | RFC 5322 邮箱模式 | `support@company.com` |
| `api_key` | API 密钥与凭据 | OpenAI (`sk-...`)、Anthropic (`ant-...`)、AWS (`AKIA...`)、JWT (`eyJ...`)、私钥报头 | `sk-proj-abc...`, `eyJhbGci...` |
| `ip_address` | 网络 IP 地址 | IPv4 Dotted-quad (`\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b`) | `192.168.1.1` |

### 3.2 四大处理动作策略 (Action Strategies)

针对每个实体类型，管理员可在控制台配置以下动作：
1. **`anonymize_restore` (双向去标识化与还原，推荐默认)**：
   * 入站：生成占位符 `[<ENTITY>_<INDEX>]`（如 `[PHONE_1]`），记录到当前会话映射表；
   * 出站：大模型回复中如果包含该占位符，由网关逆向无感还原为真实明文。
2. **`mask_partial` (部分掩码)**：
   * 保留首尾关键识别位，中间打码：
     * 手机号：`138****5678`（保留前 3 后 4）
     * 身份证：`110101********2378`（保留前 6 后 4）
     * 银行卡：`622202******7890`（保留前 6 后 4）
     * 邮箱：`z***n@example.com`
3. **`redact_tag` (实体标签擦除)**：
   * 单向完全替换为标签（如 `[PHONE]`），不保留映射表，出站不还原。
4. **`block` (直接拦截阻断)**：
   * 一旦在输入中检测到，立刻拒绝请求，返回 HTTP 400：
     `{"error": {"type": "guardrails_violation", "code": "sensitive_data_blocked", "entity": "api_key"}}`。

### 3.3 出站 SSE 流式跨包还原状态机 (Streaming Sliding Window)

为解决大模型在流式输出（Server-Sent Events）时，可能把占位符 `[PHONE_1]` 拆分到两个不同 chunk 的问题（例如 chunk 1 返回 `"[PH"`，chunk 2 返回 `"ONE_1]"`）：
1. 在流式响应过滤器中维护 32 字节的微缓冲环形区；
2. 当遇到 `[` 且未闭合 `]` 时，暂存待输出字符；
3. 一旦收到 `]`，检测完整 Token 是否在当前会话的 `pii_session_map` 中：
   * 若命中：将对应明文输出，并清空缓冲区；
   * 若未命中或长度超过 32 字节：将缓冲区内容原样冲刷输出；
4. 保证流式输出端到端延迟增加在 1ms 以内，且绝对不会破坏 SSE 协议格式。

### 3.4 会话映射结构设计 (C17 Lifecycle)

在 `chat_req_t` / `guardrails.h` 中定义会话映射结构：
```c
#define PII_MAX_SESSION_ENTRIES 64

typedef struct {
    char placeholder[32];   /* e.g. "[PHONE_1]" */
    char original[128];     /* e.g. "13812345678" */
} pii_entry_t;

typedef struct {
    pii_entry_t entries[PII_MAX_SESSION_ENTRIES];
    int         count;
} pii_session_map_t;
```
生命周期与单次请求严格绑定，请求处理完成后伴随 `chat_req_t` 销毁即刻内存归零 (`memset(&map, 0, sizeof map)`)，防止任何内存驻留。

### 3.5 管理接口规范 (Admin API)

1. `GET /admin/v1/guardrails/pii`：
   * 返回当前系统中各敏感实体的启用状态与动作策略 JSON。
2. `PUT /admin/v1/guardrails/pii`：
   * 更新脱敏配置规则，原子重载入内存，立即全局热生效。
3. `POST /admin/v1/guardrails/pii/test`：
   * 实时脱敏沙箱演练接口：
     * 请求体：`{"text": "测试输入文本..."}`
     * 响应体：`{"anonymized": "...", "detected_entities": [...], "restored_preview": "..."}`

### 3.6 Web 控制台前端改造 (`web/admin.html`)

在「🛡️ 安全风控」标签页中增设「🛡️ 数据脱敏与隐私合规 (PII Rules)」专属面板：
1. **实体策略矩阵表格**：每种实体对应一行，包含启用开关、实体图标、实体名称、防误报说明、动作选择下拉框（双向还原 / 部分掩码 / 标签擦除 / 拦截阻断）；
2. **实时脱敏沙箱 (PII Simulator)**：
   * 左侧：测试文本输入框与预设测试样例（包含带各种敏感信息的示例）；
   * 右侧：实时对比展示「发往大模型（已脱敏）」与「客户端接收（已还原）」视图，点击「立即演练」即时验证。

---

## 4. 验证与验收标准 (Acceptance Criteria)

- [ ] 正则与算法验证：
  - 手机号检测国内与国际格式；
  - 18 位身份证需通过 MOD 11-2 校验和验证，普通 18 位随机数不触发；
  - 银行卡需通过 Luhn 校验和验证，普通长订单号不触发；
  - API Key、JWT、私钥报头能稳定识别。
- [ ] 策略动作验证：
  - `anonymize_restore`：入站正确替换为 `[ENTITY_N]`，出站（含非流式与流式 SSE）100% 还原；
  - `mask_partial`：正确展示为带 `****` 的部分打码串；
  - `block`：请求被拦截并返回 400，审计日志记录拦截事件。
- [ ] 控制台功能验证：
  - 在 `web/admin.html`「安全风控」页中可自由配置策略并保存；
  - 实时脱敏沙箱演练可正确展示脱敏与还原效果。
- [ ] 编译与构建标准：
  - CMake 构建成功，全量单元测试与集成测试通过（100%）；
  - `Doxygen: 0 warnings`（严格保持零 Doxygen 告警）。
