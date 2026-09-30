# Vision / Multimodal Image Support Design

## §0 概述

为 aigate 的 Anthropic 和 Gemini provider adapter 添加 Vision（图片）消息支持。OpenAI adapter 已天然 pass-through，不需改动。

**范围：**
- 入站格式：OpenAI `content: [{type:"text",...}, {type:"image_url", image_url:{url:"https://..."}}]`
- 出站格式：各 provider 的原生图片协议
- URL 处理策略：直接透传（F1），不下载/不转 base64
- base64 data URI：原样透传（B2），不解析

---

## §1 协议映射

### 输入（OpenAI content parts）

```json
{
  "role": "user",
  "content": [
    {"type": "text", "text": "描述这张图片"},
    {"type": "image_url", "image_url": {"url": "https://example.com/photo.jpg"}}
  ]
}
```

### → Anthropic

```json
{
  "role": "user",
  "content": [
    {"type": "text", "text": "描述这张图片"},
    {
      "type": "image",
      "source": {
        "type": "url",
        "url": "https://example.com/photo.jpg"
      }
    }
  ]
}
```

- `image_url.url` → `source.url`，`source.type` 固定为 `"url"`
- `detail` 字段（OpenAI 特有）忽略，Anthropic 无等价字段
- data URI（`data:` 前缀）原样放入 `source.url`（Anthropic 会报错，透传上游响应）

### → Gemini

```json
{
  "role": "user",
  "parts": [
    {"text": "描述这张图片"},
    {
      "fileData": {
        "fileUri": "https://example.com/photo.jpg",
        "mimeType": "image/jpeg"
      }
    }
  ]
}
```

- `image_url.url` → `fileData.fileUri`
- `mimeType`：从 URL 路径后缀推断：
  - `.jpg` / `.jpeg` → `image/jpeg`
  - `.png` → `image/png`
  - `.webp` → `image/webp`
  - `.gif` → `image/gif`
  - 无法推断 → `image/jpeg`（fallback），写 `AIGATE_LOG_DEBUG`

---

## §2 架构与代码改动面

### 2.1 改动范围

| 文件 | 改动类型 | 说明 |
|---|---|---|
| `src/upstream/provider_anthropic.c` | 修改 | 新增 `ant_build_content_array()` 静态函数；消息循环中对 user/assistant 普通文本消息改用新函数 |
| `src/upstream/provider_gemini.c` | 修改 | 新增 `gemini_infer_mime_type()` 静态 helper；parts 构建循环扩展 image_url 支持 |
| `src/upstream/provider_openai.c` | **不改动** | 天然 pass-through |
| `tests/unit/upstream/test_provider_anthropic_vision.c` | **新建** | 3 个 Anthropic vision 单元测试 |
| `tests/unit/upstream/test_provider_gemini_vision.c` | **新建** | 3 个 Gemini vision 单元测试 |
| `tests/unit/run_tests.c` | 修改 | 注册 6 个新测试 |

**不改动：** `aigate_core.c`、`model_router.c`、`upstream_client.c`、所有 policy/store/observe 模块

### 2.2 Anthropic 核心设计

新增静态函数 `ant_build_content_array(json_t* jcontent) → json_t*`：

- 若 `jcontent` 为字符串：返回 `[{"type":"text","text":"..."}]`
- 若 `jcontent` 为 array：遍历每个 part：
  - `type == "text"` → 追加 `{"type":"text","text":"..."}`
  - `type == "image_url"` → 追加 `{"type":"image","source":{"type":"url","url":"..."}}`
  - 其他类型 → 跳过，写 `AIGATE_LOG_WARN`
- `jcontent` 为 null 且 role==assistant+tool_calls → 返回 NULL（由调用方处理）

`ant_extract_text()` 保留，仅用于 system 消息提取（system 消息不含图片）。

在 `provider_anthropic_build` 消息循环中：
- `role == "system"` → 继续用 `ant_extract_text()` 提取文本
- `role == "user"` 或 `role == "assistant"`（无 tool_calls）→ 改用 `ant_build_content_array(jcontent)` 生成完整 content array

### 2.3 Gemini 核心设计

新增静态函数 `gemini_infer_mime_type(const char* url) → const char*`：

```c
static const char*
gemini_infer_mime_type(const char* url)
{
    if (!url) return "image/jpeg";
    const char* q = strchr(url, '?');
    size_t path_len = q ? (size_t)(q - url) : strlen(url);
    if (path_len >= 4 && strncasecmp(url + path_len - 4, ".png",  4) == 0) return "image/png";
    if (path_len >= 5 && strncasecmp(url + path_len - 5, ".jpeg", 5) == 0) return "image/jpeg";
    if (path_len >= 4 && strncasecmp(url + path_len - 4, ".jpg",  4) == 0) return "image/jpeg";
    if (path_len >= 5 && strncasecmp(url + path_len - 5, ".webp", 5) == 0) return "image/webp";
    if (path_len >= 4 && strncasecmp(url + path_len - 4, ".gif",  4) == 0) return "image/gif";
    AIGATE_LOG_DEBUG("gemini_infer_mime_type: cannot infer from url, defaulting to image/jpeg");
    return "image/jpeg";
}
```

在 `provider_gemini_build` 消息循环的 parts 构建段，将现有：

```c
json_t* part = json_object();
json_object_set_new(part, "text", json_string(plain_content));
json_array_append_new(parts, part);
```

扩展为：迭代 `jcontent` array（若是 array），对每个 part：
- `type == "text"` → `{"text":"..."}`
- `type == "image_url"` → `{"fileData":{"fileUri":"...","mimeType":"image/jpeg"}}`
- `jcontent` 为字符串 → `{"text":"..."}` 单 part（保持原行为）

---

## §3 错误处理

| 场景 | 行为 |
|---|---|
| `content` 为普通字符串 | 原样处理为纯文本，行为完全向后兼容 |
| `content` array 无 image_url | 只构建文本 blocks，行为不变 |
| `image_url.url` 为 `data:` 前缀 | 原样透传，上游若报错则透传 4xx（不影响 aigate 自身） |
| Gemini 收到非 GCS URL | Gemini 返回 4xx，gateway 透传；若有多 candidate 则 failover |
| `image_url` 缺少 `url` 字段 | 跳过该 part，写 `AIGATE_LOG_WARN` |
| `type == "image_url"` 但 `image_url` 字段为 null | 跳过该 part，写 `AIGATE_LOG_WARN` |

---

## §4 测试策略

### Anthropic（`tests/unit/upstream/test_provider_anthropic_vision.c`）

| 用例 | 输入 | 验证点 |
|---|---|---|
| `test_anthropic_vision_single_image` | user: [text + image_url] | content array 含 text block + image block，source.url 正确 |
| `test_anthropic_vision_text_only_string` | user: string content | content array 等价于原行为（1 个 text block）|
| `test_anthropic_vision_multi_image` | user: [text + 2×image_url] | content array 共 3 个 block，顺序正确 |

### Gemini（`tests/unit/upstream/test_provider_gemini_vision.c`）

| 用例 | 输入 | 验证点 |
|---|---|---|
| `test_gemini_vision_single_image` | user: [text + image_url(.png)] | parts 含 text + fileData，mimeType=image/png |
| `test_gemini_vision_unknown_mime_fallback` | user: [image_url(.bin)] | fileData.mimeType fallback 为 image/jpeg |
| `test_gemini_vision_text_only_string` | user: string content | parts 含单 text part，行为不退步 |

---

## §5 验收标准

- [ ] Anthropic provider：含图片的 messages 正确转换为 Anthropic content array（含 image blocks）
- [ ] Gemini provider：含图片的 messages 正确转换为 parts（含 fileData blocks），mimeType 正确推断
- [ ] 纯文本消息（string content）行为不变，全量 ctest 100% 通过
- [ ] 所有新增 6 个单元测试全部通过
- [ ] 无新增编译 warning（`-Wall -Wextra -Werror`）
