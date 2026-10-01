# 敏感数据脱敏与安全风控升级 (PII Masking & Advanced Guardrails) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建支持双向无感去标识化与还原、高精度校验和算法（Luhn 模 10 / ISO 7064:1983.MOD 11-2）、多级策略（可逆还原/掩码/擦除/阻断）及在线演练沙箱的企业级数据脱敏与安全风控流水线。

**Architecture:** 入站阶段结合正则表达式与数学算法校验，精准识别 6 类敏感实体（手机号、身份证、银行卡、邮箱、API Key、IP），按实体策略执行双向占位符替换（`[PHONE_1]` 并记录会话映射表）、部分打码（`138****5678`）、标签擦除或 400 阻断；出站阶段（含非流式与流式 SSE 32 字节跨包滑动窗口）将大模型返回的占位符逆向还原为真实明文；通过 Admin REST API 与 Web 控制台提供可视化策略矩阵与实时脱敏效果验证沙箱。

**Tech Stack:** C17 (GCC/Clang), POSIX Extended Regex, Jansson JSON, CivetWeb, HTML5 / Tailwind CSS / Vanilla JS.

---

## File Structure

- **Modified:**
  - `src/policy/guardrails.h`: 声明 PII 实体枚举、动作策略、会话映射结构体 `pii_session_map_t`、Luhn 与 MOD 11-2 校验函数原型、脱敏与逆向还原函数原型、PII 配置获取与更新接口。
  - `src/policy/guardrails.c`: 实现 Luhn 模 10 校验、MOD 11-2 校验、部分打码算法、敏感实体正则与校验复合检测、双向占位符生成与逆向还原、全局/默认 PII 规则表。
  - `src/core/aigate_core_internal.h`: 在 `chat_req_t` 中挂载请求级生命周期的 `pii_session_map_t pii_map`；在 `stream_cache_acc_t` 中增加流式滑动窗口缓存字段。
  - `src/core/aigate_core.c`: 在 `chat_req_cleanup()` 中对 `pii_map` 进行安全清零；在 `stream_cache_acc_write()` 中实现 SSE 跨包占位符滑动窗口还原。
  - `src/policy/filter_chain.c`: 入站拦截阶段调用增强版 PII 检查并记录映射表；出站阶段调用逆向还原。
  - `src/server/admin_api.c`: 新增 `GET /admin/v1/guardrails/pii`、`PUT /admin/v1/guardrails/pii`、`POST /admin/v1/guardrails/pii/test` 路由与处理函数。
  - `web/admin.html`: 在「🛡️ 安全风控」选项卡中增加「数据脱敏与隐私合规 (PII Rules)」矩阵配置面板与「实时脱敏沙箱 (PII Simulator)」。
- **Tests:**
  - `tests/unit/policy/test_guardrails.c`: 覆盖校验和算法、各类实体识别与打码、双向去标识化与还原、SSE 跨包切割还原、Admin API 接口等单元测试。

---

### Task 1: Checksum Verification Algorithms (Luhn Mod 10 & Chinese ID Card MOD 11-2)

**Files:**
- Modify: `src/policy/guardrails.h`
- Modify: `src/policy/guardrails.c`
- Test: `tests/unit/policy/test_guardrails.c`

- [ ] **Step 1: Write the failing tests for Luhn and MOD 11-2 checksum algorithms**

Add test cases in `tests/unit/policy/test_guardrails.c`:
```c
TEST_CASE(test_pii_checksum_algorithms)
{
    /* 1. Luhn Mod 10 for Bank / Credit Card */
    /* Valid test cards */
    TEST_ASSERT(guardrails_validate_luhn("49927398716") == true, "valid luhn 1");
    TEST_ASSERT(guardrails_validate_luhn("6222021234567890") == true, "valid luhn 2 (UnionPay pattern)");
    TEST_ASSERT(guardrails_validate_luhn("4532015112830366") == true, "valid luhn 3 (Visa)");
    /* Invalid cards / random numbers / wrong lengths */
    TEST_ASSERT(guardrails_validate_luhn("49927398717") == false, "invalid luhn check digit");
    TEST_ASSERT(guardrails_validate_luhn("1234567890123456") == false, "sequential digits fail luhn");
    TEST_ASSERT(guardrails_validate_luhn("12345") == false, "too short card fails luhn");
    TEST_ASSERT(guardrails_validate_luhn("123456789012345678901") == false, "too long card fails luhn");
    TEST_ASSERT(guardrails_validate_luhn(NULL) == false, "null string fails luhn");

    /* 2. ISO 7064:1983.MOD 11-2 for Chinese 18-digit ID Card */
    /* Valid ID cards (Standard checksums) */
    TEST_ASSERT(guardrails_validate_id_card_mod11("110101199003072378") == true, "valid id card digit");
    TEST_ASSERT(guardrails_validate_id_card_mod11("110101199003072386") == true, "valid id card digit 2");
    TEST_ASSERT(guardrails_validate_id_card_mod11("110101199003072394") == true, "valid id card digit 3");
    /* Valid ID with 'X' check digit */
    TEST_ASSERT(guardrails_validate_id_card_mod11("11010119900307415X") == true, "valid id card with X");
    TEST_ASSERT(guardrails_validate_id_card_mod11("11010119900307415x") == true, "valid id card with lowercase x");
    /* Invalid ID cards */
    TEST_ASSERT(guardrails_validate_id_card_mod11("110101199003072379") == false, "wrong checksum digit fails");
    TEST_ASSERT(guardrails_validate_id_card_mod11("123456789012345678") == false, "random 18 digits fail MOD 11-2");
    TEST_ASSERT(guardrails_validate_id_card_mod11("11010119900307") == false, "short length fails");
    TEST_ASSERT(guardrails_validate_id_card_mod11(NULL) == false, "null fails");
}
```
And register `test_register("pii_checksum_algorithms", test_pii_checksum_algorithms);` in `tests/unit/run_tests.c`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Compilation failure or link error because `guardrails_validate_luhn` and `guardrails_validate_id_card_mod11` are not declared/defined.

- [ ] **Step 3: Implement `guardrails_validate_luhn` and `guardrails_validate_id_card_mod11`**

In `src/policy/guardrails.h`:
```c
/**
 * @brief Validate bank/credit card number using Luhn algorithm (Mod 10).
 * @param digits Numeric string containing only digits '0'-'9'.
 * @return True if length is 13..19 digits and Luhn checksum is valid; false otherwise.
 */
bool guardrails_validate_luhn(const char* digits);

/**
 * @brief Validate Chinese 18-digit resident ID card using ISO 7064:1983.MOD 11-2.
 * @param id_str 18-character string (17 digits + 1 digit/X).
 * @return True if format matches and check character matches MOD 11-2 remainder; false otherwise.
 */
bool guardrails_validate_id_card_mod11(const char* id_str);
```

In `src/policy/guardrails.c`:
```c
bool
guardrails_validate_luhn(const char* digits)
{
    if (digits == NULL) {
        return false;
    }
    size_t len = strlen(digits);
    if (len < 13 || len > 19) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)digits[i])) {
            return false;
        }
    }
    int  sum = 0;
    bool alternate = false;
    for (ssize_t i = (ssize_t)len - 1; i >= 0; i--) {
        int n = digits[i] - '0';
        if (alternate) {
            n *= 2;
            if (n > 9) {
                n = (n % 10) + 1;
            }
        }
        sum += n;
        alternate = !alternate;
    }
    return (sum % 10 == 0);
}

bool
guardrails_validate_id_card_mod11(const char* id_str)
{
    if (id_str == NULL) {
        return false;
    }
    if (strlen(id_str) != 18) {
        return false;
    }
    static const int  weights[17] = {7, 9, 10, 5, 8, 4, 2, 1, 6, 3, 7, 9, 10, 5, 8, 4, 2};
    static const char check_chars[11] = {'1', '0', 'X', '9', '8', '7', '6', '5', '4', '3', '2'};

    int sum = 0;
    for (int i = 0; i < 17; i++) {
        if (!isdigit((unsigned char)id_str[i])) {
            return false;
        }
        sum += (id_str[i] - '0') * weights[i];
    }
    int  mod = sum % 11;
    char expected = check_chars[mod];
    char actual = id_str[17];
    if (actual == 'x') {
        actual = 'X';
    }
    return actual == expected;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Build passes, `test_pii_checksum_algorithms` PASS, 0 failures.

- [ ] **Step 5: Commit Task 1**

```bash
git add src/policy/guardrails.h src/policy/guardrails.c tests/unit/policy/test_guardrails.c tests/unit/run_tests.c
git commit -m "feat(guardrails): implement Luhn and ISO 7064 MOD 11-2 checksum algorithms"
```

---

### Task 2: PII Data Models, Partial Masking, and Session Mapping

**Files:**
- Modify: `src/policy/guardrails.h`
- Modify: `src/policy/guardrails.c`
- Test: `tests/unit/policy/test_guardrails.c`

- [ ] **Step 1: Write failing tests for partial masking and session map operations**

In `tests/unit/policy/test_guardrails.c`:
```c
TEST_CASE(test_pii_session_map_and_partial_masking)
{
    /* 1. Partial masking tests */
    char masked[128];

    guardrails_mask_partial_phone("13812345678", masked, sizeof masked);
    TEST_ASSERT(strcmp(masked, "138****5678") == 0, "phone partial mask 138****5678");

    guardrails_mask_partial_id_card("110101199003072378", masked, sizeof masked);
    TEST_ASSERT(strcmp(masked, "110101********2378") == 0, "id card partial mask 110101********2378");

    guardrails_mask_partial_bank_card("6222021234567890", masked, sizeof masked);
    TEST_ASSERT(strcmp(masked, "622202******7890") == 0, "bank card partial mask 622202******7890");

    guardrails_mask_partial_email("alice.wonder@company.com", masked, sizeof masked);
    TEST_ASSERT(strcmp(masked, "a***r@company.com") == 0, "email partial mask a***r@company.com");

    guardrails_mask_partial_api_key("sk-proj-1234567890abcdef123456", masked, sizeof masked);
    TEST_ASSERT(strcmp(masked, "sk-proj-******3456") == 0, "api key partial mask sk-proj-******3456");

    guardrails_mask_partial_ip("192.168.1.100", masked, sizeof masked);
    TEST_ASSERT(strcmp(masked, "192.168.*.*") == 0, "ip partial mask 192.168.*.*");

    /* 2. Session mapping table operations */
    pii_session_map_t map;
    memset(&map, 0, sizeof map);

    const char* tok1 = pii_session_map_get_or_create(&map, PII_TYPE_PHONE, "13812345678");
    TEST_ASSERT(tok1 != NULL, "tok1 created");
    TEST_ASSERT(strcmp(tok1, "[PHONE_1]") == 0, "first phone token [PHONE_1]");
    TEST_ASSERT(map.count == 1, "map count is 1");

    /* Same value returns existing token (referential consistency) */
    const char* tok1_dup = pii_session_map_get_or_create(&map, PII_TYPE_PHONE, "13812345678");
    TEST_ASSERT(strcmp(tok1_dup, "[PHONE_1]") == 0, "duplicate returns same token");
    TEST_ASSERT(map.count == 1, "map count unchanged on duplicate");

    /* Second distinct value creates [PHONE_2] */
    const char* tok2 = pii_session_map_get_or_create(&map, PII_TYPE_PHONE, "13900001111");
    TEST_ASSERT(strcmp(tok2, "[PHONE_2]") == 0, "second phone token [PHONE_2]");
    TEST_ASSERT(map.count == 2, "map count is 2");

    /* Reverse lookup */
    const char* orig1 = pii_session_map_lookup_token(&map, "[PHONE_1]");
    TEST_ASSERT(orig1 != NULL && strcmp(orig1, "13812345678") == 0, "lookup [PHONE_1] returns original");
    const char* orig2 = pii_session_map_lookup_token(&map, "[PHONE_2]");
    TEST_ASSERT(orig2 != NULL && strcmp(orig2, "13900001111") == 0, "lookup [PHONE_2] returns original");
    TEST_ASSERT(pii_session_map_lookup_token(&map, "[PHONE_3]") == NULL, "lookup missing token returns NULL");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Compilation failure because structs and helper functions are not yet defined.

- [ ] **Step 3: Define data structures and implement partial masking & session map helpers**

In `src/policy/guardrails.h`:
```c
/** @brief Supported PII sensitive entity categories. */
typedef enum {
    PII_TYPE_PHONE = 0,
    PII_TYPE_ID_CARD = 1,
    PII_TYPE_BANK_CARD = 2,
    PII_TYPE_EMAIL = 3,
    PII_TYPE_API_KEY = 4,
    PII_TYPE_IP_ADDRESS = 5,
    PII_TYPE_COUNT = 6
} pii_type_t;

/** @brief Processing actions for detected PII entities. */
typedef enum {
    PII_ACTION_OFF = 0,
    PII_ACTION_ANONYMIZE_RESTORE = 1, /**< Replace with [TAG_N] and restore on response */
    PII_ACTION_MASK_PARTIAL = 2,      /**< Keep prefixes/suffixes and mask middle with '*' */
    PII_ACTION_REDACT_TAG = 3,        /**< One-way replacement with [TAG] */
    PII_ACTION_BLOCK = 4              /**< Block the entire request with HTTP 400 */
} pii_action_t;

/** @brief Configuration rule for a single PII entity. */
typedef struct {
    pii_type_t   type;
    char         name[32];            /**< e.g. "phone", "id_card", "bank_card" */
    char         tag[32];             /**< e.g. "PHONE", "ID_CARD", "BANK_CARD" */
    bool         enabled;
    pii_action_t action;
} pii_rule_t;

/** @brief Global PII configuration table. */
typedef struct {
    pii_rule_t rules[PII_TYPE_COUNT];
} pii_config_t;

#define PII_MAX_SESSION_ENTRIES 64

/** @brief One mapping entry between placeholder token and original sensitive text. */
typedef struct {
    char placeholder[32];   /**< e.g. "[PHONE_1]" */
    char original[128];     /**< e.g. "13812345678" */
    pii_type_t type;
} pii_entry_t;

/** @brief Request-bound session mapping table. */
typedef struct {
    pii_entry_t entries[PII_MAX_SESSION_ENTRIES];
    int         count;
} pii_session_map_t;

/* Partial masking functions */
void guardrails_mask_partial_phone(const char* src, char* out, size_t out_sz);
void guardrails_mask_partial_id_card(const char* src, char* out, size_t out_sz);
void guardrails_mask_partial_bank_card(const char* src, char* out, size_t out_sz);
void guardrails_mask_partial_email(const char* src, char* out, size_t out_sz);
void guardrails_mask_partial_api_key(const char* src, char* out, size_t out_sz);
void guardrails_mask_partial_ip(const char* src, char* out, size_t out_sz);

/* Session map functions */
const char* pii_session_map_get_or_create(pii_session_map_t* map, pii_type_t type, const char* original);
const char* pii_session_map_lookup_token(const pii_session_map_t* map, const char* placeholder);
```

In `src/policy/guardrails.c`:
Implement the 6 partial masking functions and the session map helpers `pii_session_map_get_or_create` and `pii_session_map_lookup_token`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Build passes, `test_pii_session_map_and_partial_masking` PASS.

- [ ] **Step 5: Commit Task 2**

```bash
git add src/policy/guardrails.h src/policy/guardrails.c tests/unit/policy/test_guardrails.c tests/unit/run_tests.c
git commit -m "feat(guardrails): add PII data structures, partial masking, and session map"
```

---

### Task 3: Inbound PII Scanning, Transformation, and Request Lifecycle

**Files:**
- Modify: `src/policy/guardrails.h`
- Modify: `src/policy/guardrails.c`
- Modify: `src/core/aigate_core_internal.h`
- Modify: `src/core/aigate_core.c`
- Modify: `src/policy/filter_chain.c`
- Test: `tests/unit/policy/test_guardrails.c`

- [ ] **Step 1: Write failing tests for inbound PII transformation with actions**

In `tests/unit/policy/test_guardrails.c`:
```c
TEST_CASE(test_pii_inbound_transformation)
{
    guardrails_ctx_t* ctx = guardrails_create();
    TEST_ASSERT(ctx != NULL, "guardrails_create");

    /* 1. Anonymize & Restore action (creates token in map) */
    pii_session_map_t map;
    memset(&map, 0, sizeof map);
    const char* txt1 = "联系张三 13812345678 或者李四 13812345678";
    int changed = 0;
    pii_action_t act = PII_ACTION_OFF;
    char* trans1 = guardrails_transform_pii_text(ctx, txt1, strlen(txt1), &map, &act, &changed);

    TEST_ASSERT(changed == 1, "text transformed");
    TEST_ASSERT(act == PII_ACTION_ANONYMIZE_RESTORE, "action anonymize");
    TEST_ASSERT(strstr(trans1, "[PHONE_1]") != NULL, "contains [PHONE_1]");
    TEST_ASSERT(strstr(trans1, "13812345678") == NULL, "original phone replaced");
    TEST_ASSERT(map.count == 1, "map has 1 entry");
    free(trans1);

    /* 2. Bank card with Luhn verification (valid vs invalid) */
    /* Valid UnionPay card passes checksum -> masked */
    const char* txt2 = "卡号 6222021234567890 请查收";
    changed = 0;
    char* trans2 = guardrails_transform_pii_text(ctx, txt2, strlen(txt2), &map, &act, &changed);
    TEST_ASSERT(changed == 1, "valid bank card recognized");
    free(trans2);

    /* Invalid number of same length fails Luhn -> untouched */
    const char* txt3 = "订单号 1234567890123456 请注意";
    changed = 0;
    char* trans3 = guardrails_transform_pii_text(ctx, txt3, strlen(txt3), &map, &act, &changed);
    TEST_ASSERT(changed == 0, "invalid card number ignored by Luhn");
    TEST_ASSERT(trans3 == NULL, "no change returns NULL");

    /* 3. Chinese ID card with MOD 11-2 verification (valid vs invalid) */
    const char* txt4 = "身份证 110101199003072378 归属地";
    changed = 0;
    char* trans4 = guardrails_transform_pii_text(ctx, txt4, strlen(txt4), &map, &act, &changed);
    TEST_ASSERT(changed == 1, "valid ID card recognized");
    free(trans4);

    const char* txt5 = "编号 123456789012345678 请核对";
    changed = 0;
    char* trans5 = guardrails_transform_pii_text(ctx, txt5, strlen(txt5), &map, &act, &changed);
    TEST_ASSERT(changed == 0, "invalid 18-digit number ignored by MOD 11-2");
    TEST_ASSERT(trans5 == NULL, "no change returns NULL");

    guardrails_destroy(ctx);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Compilation failure for `guardrails_transform_pii_text`.

- [ ] **Step 3: Implement `guardrails_transform_pii_text` and update regex compilation & inspection pipeline**

In `src/policy/guardrails.h`:
```c
/**
 * @brief Transform inbound text based on active PII rules and session map.
 * @param ctx Guardrails context.
 * @param text Raw text.
 * @param len Length of text.
 * @param map Pointer to request-bound session map (for anonymize_restore).
 * @param out_action Receives highest-severity action taken (BLOCK > MASK > PASS).
 * @param changed Receives 1 if modified, 0 otherwise.
 * @return Newly allocated string (caller frees), or NULL if unchanged.
 */
char* guardrails_transform_pii_text(guardrails_ctx_t*  ctx,
                                    const char*        text,
                                    size_t             len,
                                    pii_session_map_t* map,
                                    pii_action_t*      out_action,
                                    int*               changed);
```

In `src/policy/guardrails.c`:
- Add `re_bank_card` (`[0-9]{13,19}`) and `re_ip` (`\b([0-9]{1,3}\.){3}[0-9]{1,3}\b`) to `guardrails_ctx`.
- Default PII configuration: `phone` (ANONYMIZE_RESTORE), `id_card` (ANONYMIZE_RESTORE), `bank_card` (MASK_PARTIAL), `email` (ANONYMIZE_RESTORE), `api_key` (BLOCK), `ip_address` (REDACT_TAG).
- In `guardrails_transform_pii_text`, scan text for each entity. For ID cards, run `guardrails_validate_id_card_mod11`; for bank cards, run `guardrails_validate_luhn`. Apply the configured action (`anonymize_restore`, `mask_partial`, `redact_tag`, `block`).
- Update `guardrails_inspect_inbound_with_pii(...)` to pass `pii_session_map_t* map`.

In `src/core/aigate_core_internal.h`:
Add `pii_session_map_t pii_map;` to `chat_req_t`.

In `src/core/aigate_core.c`:
In `chat_req_cleanup(chat_req_t* q)`:
```c
    memset(&q->pii_map, 0, sizeof(q->pii_map));
```

In `src/policy/filter_chain.c`:
Pass `&q->pii_map` to inbound guardrails inspection; if `PII_ACTION_BLOCK`, return HTTP 400 with `sensitive_data_blocked` error JSON.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Build passes, `test_pii_inbound_transformation` PASS, all other tests pass.

- [ ] **Step 5: Commit Task 3**

```bash
git add src/policy/guardrails.h src/policy/guardrails.c src/core/aigate_core_internal.h src/core/aigate_core.c src/policy/filter_chain.c tests/unit/policy/test_guardrails.c tests/unit/run_tests.c
git commit -m "feat(guardrails): integrate high-precision inbound PII transformation and request lifecycle"
```

---

### Task 4: Outbound De-anonymization (Non-Streaming & SSE Streaming Sliding Window Filter)

**Files:**
- Modify: `src/policy/guardrails.h`
- Modify: `src/policy/guardrails.c`
- Modify: `src/core/aigate_core_internal.h`
- Modify: `src/core/aigate_core.c`
- Modify: `src/policy/filter_chain.c`
- Test: `tests/unit/policy/test_guardrails.c`

- [ ] **Step 1: Write failing tests for outbound restoration (full text & streaming chunks)**

In `tests/unit/policy/test_guardrails.c`:
```c
TEST_CASE(test_pii_outbound_restoration)
{
    pii_session_map_t map;
    memset(&map, 0, sizeof map);
    strcpy(map.entries[0].placeholder, "[PHONE_1]");
    strcpy(map.entries[0].original, "13812345678");
    strcpy(map.entries[1].placeholder, "[ID_CARD_1]");
    strcpy(map.entries[1].original, "110101199003072378");
    map.count = 2;

    /* 1. Full text restoration */
    const char* reply = "已经向用户 [PHONE_1]（身份证 [ID_CARD_1]）发送验证码。";
    int changed = 0;
    char* restored = guardrails_restore_pii_text(reply, strlen(reply), &map, &changed);

    TEST_ASSERT(changed == 1, "restored text changed");
    TEST_ASSERT(restored != NULL, "restored not null");
    TEST_ASSERT(strstr(restored, "[PHONE_1]") == NULL, "token replaced");
    TEST_ASSERT(strstr(restored, "13812345678") != NULL, "original phone present");
    TEST_ASSERT(strstr(restored, "110101199003072378") != NULL, "original ID present");
    free(restored);

    /* 2. SSE streaming chunk boundary split restoration */
    /* Simulate token [PHONE_1] split across two consecutive chunks */
    pii_stream_filter_t sf;
    guardrails_stream_filter_init(&sf, &map);

    char out1[128] = {0};
    size_t out1_len = 0;
    /* Chunk 1 ends in partial token "[PH" */
    guardrails_stream_filter_feed(&sf, "data: {\"content\":\"call [PH", 26, out1, sizeof out1, &out1_len);
    TEST_ASSERT(strstr(out1, "[PH") == NULL, "partial token buffered, not emitted yet");

    char out2[128] = {0};
    size_t out2_len = 0;
    /* Chunk 2 supplies "ONE_1] now\"}\n\n" */
    guardrails_stream_filter_feed(&sf, "ONE_1] now\"}\n\n", 14, out2, sizeof out2, &out2_len);
    TEST_ASSERT(strstr(out2, "13812345678") != NULL, "split token restored to real phone");
    TEST_ASSERT(strstr(out2, "[PHONE_1]") == NULL, "no placeholder in output");

    /* Flush filter */
    char out_fin[64] = {0};
    size_t fin_len = 0;
    guardrails_stream_filter_flush(&sf, out_fin, sizeof out_fin, &fin_len);
    TEST_ASSERT(fin_len == 0, "no residual bytes on clean termination");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Compilation failure because `guardrails_restore_pii_text` and `guardrails_stream_filter_*` are not defined.

- [ ] **Step 3: Implement `guardrails_restore_pii_text` and `guardrails_stream_filter_*`**

In `src/policy/guardrails.h`:
```c
/** @brief Outbound streaming sliding-window filter context for de-anonymization. */
typedef struct {
    const pii_session_map_t* map;
    char   win[32];   /**< Sliding window holding partial token starting with '[' */
    size_t win_len;
} pii_stream_filter_t;

/** @brief Reverse-replace session tokens in response text back to original values. */
char* guardrails_restore_pii_text(const char*              resp_text,
                                  size_t                   resp_len,
                                  const pii_session_map_t* map,
                                  int*                     changed);

void guardrails_stream_filter_init(pii_stream_filter_t* sf, const pii_session_map_t* map);
void guardrails_stream_filter_feed(pii_stream_filter_t* sf,
                                   const char*          chunk,
                                   size_t               len,
                                   char*                out,
                                   size_t               out_cap,
                                   size_t*              out_len);
void guardrails_stream_filter_flush(pii_stream_filter_t* sf,
                                    char*                out,
                                    size_t               out_cap,
                                    size_t*              out_len);
```

In `src/policy/guardrails.c`:
Implement `guardrails_restore_pii_text`, `guardrails_stream_filter_init`, `guardrails_stream_filter_feed`, and `guardrails_stream_filter_flush`.

In `src/policy/filter_chain.c`:
In `filter_chain_execute_outbound(...)`:
Before returning, if `q->pii_map.count > 0`, call `guardrails_restore_pii_text` on `resp_body` and return the restored body.

In `src/core/aigate_core_internal.h` & `src/core/aigate_core.c`:
In `stream_cache_acc_t`, add `pii_stream_filter_t pii_sf;` and initialize it with `&q->pii_map`. In `stream_cache_acc_write()`, pass incoming chunks through `guardrails_stream_filter_feed` before writing to `acc->orig_rc->write` if `acc->pii_sf.map != NULL && acc->pii_sf.map->count > 0`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Build passes, `test_pii_outbound_restoration` PASS, all 206+ tests pass.

- [ ] **Step 5: Commit Task 4**

```bash
git add src/policy/guardrails.h src/policy/guardrails.c src/core/aigate_core_internal.h src/core/aigate_core.c src/policy/filter_chain.c tests/unit/policy/test_guardrails.c tests/unit/run_tests.c
git commit -m "feat(guardrails): add outbound de-anonymization and SSE streaming sliding window filter"
```

---

### Task 5: Admin REST API Endpoints (`GET/PUT /admin/v1/guardrails/pii` & `POST /admin/v1/guardrails/pii/test`)

**Files:**
- Modify: `src/policy/guardrails.h`
- Modify: `src/policy/guardrails.c`
- Modify: `src/server/admin_api.c`
- Test: `tests/unit/server/test_admin_api.c`

- [ ] **Step 1: Write failing tests for PII admin endpoints**

In `tests/unit/server/test_admin_api.c`:
```c
TEST_CASE(test_admin_pii_endpoints)
{
    admin_ctx_t adm;
    memset(&adm, 0, sizeof adm);
    adm.ac = aigate_core_create(NULL);
    TEST_ASSERT(adm.ac != NULL, "aigate_core_create");

    int status = 0;
    char* body = NULL;
    size_t len = 0;

    /* 1. GET /admin/v1/guardrails/pii */
    int rc = handle_admin_request(&adm, "GET", "guardrails/pii", NULL, NULL, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "GET guardrails/pii returns 200");
    TEST_ASSERT(body != NULL && strstr(body, "\"rules\"") != NULL, "body contains rules");
    TEST_ASSERT(strstr(body, "phone") != NULL, "body contains phone entity");
    free(body);
    body = NULL;

    /* 2. PUT /admin/v1/guardrails/pii */
    const char* put_payload =
        "{\"rules\":[{\"entity\":\"phone\",\"enabled\":true,\"action\":\"mask_partial\"},"
        "{\"entity\":\"api_key\",\"enabled\":true,\"action\":\"block\"}]}";
    rc = handle_admin_request(&adm, "PUT", "guardrails/pii", NULL, put_payload, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "PUT guardrails/pii returns 200");
    free(body);
    body = NULL;

    /* 3. POST /admin/v1/guardrails/pii/test (Sandbox) */
    const char* test_payload =
        "{\"text\":\"联系客户 13812345678 查银行卡 6222021234567890\"}";
    rc = handle_admin_request(&adm, "POST", "guardrails/pii/test", NULL, test_payload, &status, &body, &len);
    TEST_ASSERT(rc == 0 && status == 200, "POST guardrails/pii/test returns 200");
    TEST_ASSERT(body != NULL && strstr(body, "\"anonymized\"") != NULL, "contains anonymized");
    TEST_ASSERT(strstr(body, "\"restored_preview\"") != NULL, "contains restored_preview");
    TEST_ASSERT(strstr(body, "\"detected_entities\"") != NULL, "contains detected_entities");
    free(body);

    aigate_core_destroy(adm.ac);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Compilation failure or 404 response because handlers for `guardrails/pii` and `guardrails/pii/test` are not yet implemented.

- [ ] **Step 3: Implement PII config APIs in `guardrails.c` and routing in `admin_api.c`**

In `src/policy/guardrails.h`:
```c
pii_config_t guardrails_get_pii_config(const guardrails_ctx_t* ctx);
int guardrails_set_pii_config(guardrails_ctx_t* ctx, const pii_config_t* cfg);
```

In `src/server/admin_api.c`:
- Implement `guardrails_pii_get(admin_ctx_t* adm, int* status, char** body, size_t* len)`: serializes active `pii_config_t` into JSON.
- Implement `guardrails_pii_put(admin_ctx_t* adm, int* status, char** body, size_t* len, const void* req_body)`: parses JSON, updates `pii_config_t` via `guardrails_set_pii_config`.
- Implement `guardrails_pii_test(admin_ctx_t* adm, int* status, char** body, size_t* len, const void* req_body)`: parses `{"text": "..."}`, executes `guardrails_transform_pii_text` and `guardrails_restore_pii_text`, returns detected entities, anonymized text, and restored preview.
- Register routes in `handle_admin_request()`:
  - `rest == "guardrails/pii"` -> GET: `guardrails_pii_get`, PUT: `guardrails_pii_put`.
  - `rest == "guardrails/pii/test"` -> POST: `guardrails_pii_test`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build -j && ./build/tests/aigate_unit_tests`
Expected: Build passes, `test_admin_pii_endpoints` PASS, all tests pass.

- [ ] **Step 5: Commit Task 5**

```bash
git add src/policy/guardrails.h src/policy/guardrails.c src/server/admin_api.c tests/unit/server/test_admin_api.c tests/unit/run_tests.c
git commit -m "feat(admin): add PII config management and real-time simulator API endpoints"
```

---

### Task 6: Web Console (`web/admin.html`) PII Matrix Panel & Simulation Sandbox

**Files:**
- Modify: `web/admin.html`

- [ ] **Step 1: Add PII Rules Configuration Matrix UI to `tab-guardrails`**

In `web/admin.html` inside `<div id="tab-guardrails">`:
Add a card panel with header "🛡️ 敏感数据脱敏与隐私合规 (PII Rules & De-identification)" containing:
- Table listing the 6 sensitive entities:
  - 手机号码 (Phone): `1[3-9]\d{9}` / E.164 国际格式
  - 居民身份证 (ID Card): 18 位大陆身份证，内置 ISO 7064:1983.MOD 11-2 校验和
  - 银行卡/信用卡 (Bank Card): 13~19 位纯数字，内置 Luhn 模 10 校验和
  - 电子邮箱 (Email): RFC 5322 邮箱规则
  - API 密钥凭据 (API Key): `sk-...`, `ghp-...`, JWT 等高敏感密钥
  - IP 地址 (IP Address): IPv4 网络地址
- Each row contains:
  - Enable/disable toggle switch
  - Algorithm/validation badge (e.g. `MOD 11-2 校验`, `Luhn 模 10 校验`, `高精度过滤`)
  - Action selector dropdown:
    - `anonymize_restore` (双向去标识化与无感还原 - 推荐)
    - `mask_partial` (部分掩码打码，如 138****5678)
    - `redact_tag` (标签替换，如 [PHONE])
    - `block` (直接拦截阻断，HTTP 400)
- "保存脱敏规则设置" button with save state indication.

- [ ] **Step 2: Add Real-time PII Simulation Sandbox (双向脱敏与还原实时演练沙箱) UI**

In `web/admin.html`:
Add sandbox panel with 2 columns:
- Left Column:
  - Textarea for input prompt
  - Quick preset buttons: "手机+银行卡", "身份证+API Key", "混合隐私样例"
  - Button "⚡ 执行脱敏演练"
- Right Column:
  - Tab / Split comparison view:
    - "发往公网大模型 (已脱敏)" (Displays anonymized string, e.g. `[PHONE_1]`, `622202******7890`)
    - "终端客户端接收 (已还原)" (Displays restored text, e.g. `13812345678`)
  - Detected entities badge summary bar (e.g. `手机号 x1 (双向还原)`, `银行卡 x1 (部分掩码)`).

- [ ] **Step 3: Implement JavaScript handlers in `web/admin.html`**

Implement:
- `fetchPiiConfig()`: fetches `GET /admin/v1/guardrails/pii` and populates the matrix table and action dropdowns.
- `savePiiConfig()`: reads the matrix and sends `PUT /admin/v1/guardrails/pii` with toast notification.
- `testPiiSandbox()`: sends `POST /admin/v1/guardrails/pii/test` with input text and renders the dual comparison and entity tags.
- `loadPiiPreset(presetType)`: sets predefined realistic text with sensitive data into the sandbox input.
- Wire `fetchPiiConfig()` into `switchTab('guardrails')` and initial page load.

- [ ] **Step 4: Build project to update embedded `admin_ui_html.h` and verify**

Run: `cmake --build build -j`
Expected: `scripts/embed_html.py` runs, `admin_ui_html.h` updates, compile finishes with 0 errors.

- [ ] **Step 5: Commit Task 6**

```bash
git add web/admin.html
git commit -m "feat(web): add PII rule configuration matrix and real-time simulation sandbox in guardrails tab"
```

---

### Task 7: Full System Verification, End-to-End Testing, and Doxygen Zero-Warning Audit

**Files:**
- Modify: `docs/superpowers/specs/2026-10-01-pii-masking-and-advanced-guardrails-design.md` (update status to Implemented)
- Verify: Full test suite (`ctest --test-dir build --output-on-failure`)
- Verify: Doxygen documentation (`cmake --build build --target doxygen` or doxygen check script)

- [ ] **Step 1: Run full test suite and memory checks**

Run: `ctest --test-dir build --output-on-failure`
Expected: 100% of test targets pass (6/6 targets, all 210+ unit tests passing).

- [ ] **Step 2: Run Doxygen documentation audit**

Run: `doxygen Doxyfile 2>&1 | grep -i warning || true`
Expected: Exactly 0 warnings. If any warning appears, fix the corresponding doc comment immediately.

- [ ] **Step 3: Update specification status**

Update `docs/superpowers/specs/2026-10-01-pii-masking-and-advanced-guardrails-design.md` header from `Draft / Pending Approval` to `Implemented & Verified`.

- [ ] **Step 4: Final Commit & Tag**

```bash
git add docs/superpowers/specs/2026-10-01-pii-masking-and-advanced-guardrails-design.md
git commit -m "docs: finalize PII masking and advanced guardrails implementation and verification"
```
