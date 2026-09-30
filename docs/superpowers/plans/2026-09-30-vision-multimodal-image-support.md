# Vision Multimodal Image Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add multimodal vision image support to Anthropic and Gemini provider adapters in aigate, translating OpenAI `image_url` content parts into native upstream image structures.

**Architecture:** Extend request building in `provider_anthropic.c` (`ant_build_content_array` for user/assistant messages) and `provider_gemini.c` (multimodal parts handling + `gemini_infer_mime_type`) to convert OpenAI `image_url` parts into Anthropic `image` blocks and Gemini `fileData` parts respectively, with zero modifications to existing core/policy/store layers.

**Tech Stack:** Pure C17, libjansson, ctest.

---

### File Structure Map

- **Modify:** `src/upstream/provider_anthropic.c`
  - Add `ant_build_content_array(json_t* jcontent)` helper function.
  - Update user/assistant message handling in `provider_anthropic_build` to use `ant_build_content_array`.
- **Create:** `tests/unit/upstream/test_provider_anthropic_vision.c`
  - Unit tests for Anthropic vision translation (single image, text-only string compatibility, multiple images).
- **Modify:** `src/upstream/provider_gemini.c`
  - Add `#include <strings.h>` for `strncasecmp`.
  - Add `gemini_infer_mime_type(const char* url)` helper function.
  - Update user/assistant message handling in `provider_gemini_build` to parse content arrays with `text` and `image_url` parts.
- **Create:** `tests/unit/upstream/test_provider_gemini_vision.c`
  - Unit tests for Gemini vision translation (single image with MIME detection, fallback MIME detection, text-only string compatibility).
- **Modify:** `tests/unit/run_tests.c`
  - Register new Anthropic and Gemini vision test functions in the test suite runner.

---

### Task 1: Anthropic Vision - Unit Test Scaffolding (Failing Tests)

**Files:**
- Create: `tests/unit/upstream/test_provider_anthropic_vision.c`
- Modify: `tests/unit/run_tests.c:338-345`

- [ ] **Step 1: Create Anthropic vision unit tests file**

Write `tests/unit/upstream/test_provider_anthropic_vision.c`:

```c
/** @file test_provider_anthropic_vision.c
 *  @brief Unit tests for Anthropic vision/multimodal protocol translation.
 */
#include "run_tests.h"
#include "provider_anthropic.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static model_rec_t
make_anthropic_route(void)
{
    model_rec_t r;
    memset(&r, 0, sizeof r);
    snprintf(r.name,         sizeof r.name,         "claude-3-5-sonnet-20241022");
    snprintf(r.provider,     sizeof r.provider,     "anthropic");
    snprintf(r.endpoint,     sizeof r.endpoint,     "http://127.0.0.1:8080");
    snprintf(r.upstream_key, sizeof r.upstream_key, "sk-ant-test");
    return r;
}

/* --------------------------------------------- Test 1: Single Image + Text */
TEST_CASE(test_anthropic_vision_single_image)
{
    model_rec_t route = make_anthropic_route();
    const char* in_body =
        "{\"model\":\"claude-3-5-sonnet-20241022\","
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"text\",\"text\":\"Describe this photo\"},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://example.com/cat.jpg\"}}"
        "]}]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n    = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int         rc   = provider_anthropic_build(&route, in_body, url, sizeof url,
                                                hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");
    TEST_ASSERT(body != NULL, "body not null");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");

    json_t* msgs = json_object_get(out, "messages");
    TEST_ASSERT(msgs && json_is_array(msgs) && json_array_size(msgs) == 1, "1 message");

    json_t* m0 = json_array_get(msgs, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(m0, "role")), "user") == 0, "role=user");

    json_t* content = json_object_get(m0, "content");
    TEST_ASSERT(content && json_is_array(content), "content is array");
    TEST_ASSERT(json_array_size(content) == 2, "content has 2 blocks");

    /* Block 0: text */
    json_t* b0 = json_array_get(content, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(b0, "type")), "text") == 0, "b0 is text");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(b0, "text")), "Describe this photo") == 0,
                "b0 text value");

    /* Block 1: image */
    json_t* b1 = json_array_get(content, 1);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(b1, "type")), "image") == 0, "b1 is image");
    json_t* src = json_object_get(b1, "source");
    TEST_ASSERT(src && json_is_object(src), "source object present");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(src, "type")), "url") == 0, "source type=url");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(src, "url")), "https://example.com/cat.jpg") == 0,
                "source url value");

    json_decref(out);
    free(body);
}

/* --------------------------------- Test 2: Text-only String Compatibility */
TEST_CASE(test_anthropic_vision_text_only_string)
{
    model_rec_t route = make_anthropic_route();
    const char* in_body =
        "{\"model\":\"claude-3-5-sonnet-20241022\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"Just a text prompt\"}]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n    = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int         rc   = provider_anthropic_build(&route, in_body, url, sizeof url,
                                                hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");

    json_t* msgs = json_object_get(out, "messages");
    json_t* m0   = json_array_get(msgs, 0);
    json_t* content = json_object_get(m0, "content");
    TEST_ASSERT(content && json_is_string(content), "content is string");
    TEST_ASSERT(strcmp(json_string_value(content), "Just a text prompt") == 0,
                "content string matches");

    json_decref(out);
    free(body);
}

/* ------------------------------------------- Test 3: Multiple Images */
TEST_CASE(test_anthropic_vision_multi_image)
{
    model_rec_t route = make_anthropic_route();
    const char* in_body =
        "{\"model\":\"claude-3-5-sonnet-20241022\","
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"text\",\"text\":\"Compare these two photos\"},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://example.com/cat1.png\"}},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://example.com/cat2.png\"}}"
        "]}]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n    = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int         rc   = provider_anthropic_build(&route, in_body, url, sizeof url,
                                                hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");

    json_t* msgs = json_object_get(out, "messages");
    json_t* m0   = json_array_get(msgs, 0);
    json_t* content = json_object_get(m0, "content");
    TEST_ASSERT(content && json_is_array(content) && json_array_size(content) == 3,
                "content array has 3 blocks");

    json_t* b1 = json_array_get(content, 1);
    json_t* b2 = json_array_get(content, 2);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(b1, "type")), "image") == 0, "b1 image");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(b2, "type")), "image") == 0, "b2 image");

    json_t* src1 = json_object_get(b1, "source");
    json_t* src2 = json_object_get(b2, "source");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(src1, "url")), "https://example.com/cat1.png") == 0,
                "url 1");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(src2, "url")), "https://example.com/cat2.png") == 0,
                "url 2");

    json_decref(out);
    free(body);
}
```

- [ ] **Step 2: Register Anthropic vision unit tests in `tests/unit/run_tests.c`**

In `tests/unit/run_tests.c`, after `test_register("anthropic_tool_choice_required_mapping", ...);` (around line 338), add:

```c
    extern void test_anthropic_vision_single_image(void);
    extern void test_anthropic_vision_text_only_string(void);
    extern void test_anthropic_vision_multi_image(void);
    test_register("anthropic_vision_single_image", test_anthropic_vision_single_image);
    test_register("anthropic_vision_text_only_string", test_anthropic_vision_text_only_string);
    test_register("anthropic_vision_multi_image", test_anthropic_vision_multi_image);
```

- [ ] **Step 3: Build and run test to verify failure**

Run:
```bash
cmake --build .build --target aigate_unit_tests
cd .build && ctest -R "anthropic_vision" --output-on-failure
```
Expected: FAIL on `test_anthropic_vision_single_image` (because current Anthropic adapter ignores `image_url` parts and turns array into single string or ignores blocks).

---

### Task 2: Anthropic Vision - Implement `ant_build_content_array`

**Files:**
- Modify: `src/upstream/provider_anthropic.c:20-65, 222-232`

- [ ] **Step 1: Add `ant_build_content_array` helper**

In `src/upstream/provider_anthropic.c`, right after `ant_extract_text` (around line 65), define `ant_build_content_array`:

```c
/** @brief Build Anthropic content: returns json_string (if plain string input) or json_array (if content parts).
 *  Caller takes ownership of returned json_t*. */
static json_t*
ant_build_content_array(json_t* jcontent)
{
    if (jcontent == NULL) {
        return json_string("");
    }
    if (json_is_string(jcontent)) {
        return json_string(json_string_value(jcontent));
    }
    if (!json_is_array(jcontent)) {
        return json_string("");
    }
    json_t* arr = json_array();
    size_t  idx;
    json_t* part;
    json_array_foreach(jcontent, idx, part)
    {
        json_t* jtype = json_object_get(part, "type");
        if (!jtype || !json_is_string(jtype)) {
            continue;
        }
        const char* type_str = json_string_value(jtype);
        if (strcmp(type_str, "text") == 0) {
            json_t* jt = json_object_get(part, "text");
            if (jt && json_is_string(jt)) {
                json_t* tb = json_object();
                json_object_set_new(tb, "type", json_string("text"));
                json_object_set_new(tb, "text", json_string(json_string_value(jt)));
                json_array_append_new(arr, tb);
            }
        } else if (strcmp(type_str, "image_url") == 0) {
            json_t* jiu = json_object_get(part, "image_url");
            json_t* ju = jiu ? json_object_get(jiu, "url") : NULL;
            if (ju && json_is_string(ju)) {
                json_t* img = json_object();
                json_object_set_new(img, "type", json_string("image"));
                json_t* src = json_object();
                json_object_set_new(src, "type", json_string("url"));
                json_object_set_new(src, "url", json_string(json_string_value(ju)));
                json_object_set_new(img, "source", src);
                json_array_append_new(arr, img);
            } else {
                AIGATE_LOG_WARN("image_url content part missing url");
            }
        } else {
            AIGATE_LOG_WARN("unsupported content part type '%s'", type_str);
        }
    }
    return arr;
}
```

- [ ] **Step 2: Update user/assistant message handling in `provider_anthropic_build`**

In `src/upstream/provider_anthropic.c`, replace lines 222-230:

```c
                } else {
                    /* plain text or multimodal user/assistant message */
                    json_t* ant_content = ant_build_content_array(jcontent);
                    json_t* m = json_object();
                    json_object_set_new(m, "role", json_string(ant_role));
                    json_object_set_new(m, "content", ant_content ? ant_content : json_string(""));
                    json_array_append_new(ant_msgs, m);
                }
```

- [ ] **Step 3: Build and run Anthropic vision tests**

Run:
```bash
cmake --build .build --target aigate_unit_tests
cd .build && ctest --output-on-failure
```
Expected: 100% tests passed.

- [ ] **Step 4: Commit Anthropic vision support**

```bash
git add src/upstream/provider_anthropic.c tests/unit/upstream/test_provider_anthropic_vision.c tests/unit/run_tests.c
git commit -m "feat(upstream): ✨ add Anthropic vision multimodal image support"
```

---

### Task 3: Gemini Vision - Unit Test Scaffolding (Failing Tests)

**Files:**
- Create: `tests/unit/upstream/test_provider_gemini_vision.c`
- Modify: `tests/unit/run_tests.c:380-390`

- [ ] **Step 1: Create Gemini vision unit tests file**

Write `tests/unit/upstream/test_provider_gemini_vision.c`:

```c
/** @file test_provider_gemini_vision.c
 *  @brief Unit tests for Gemini vision/multimodal protocol translation.
 */
#include "run_tests.h"
#include "provider_gemini.h"
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static model_rec_t
make_gemini_route(void)
{
    model_rec_t r;
    memset(&r, 0, sizeof r);
    snprintf(r.name,         sizeof r.name,         "gemini-1.5-pro");
    snprintf(r.provider,     sizeof r.provider,     "gemini");
    snprintf(r.endpoint,     sizeof r.endpoint,     "https://generativelanguage.googleapis.com");
    snprintf(r.upstream_key, sizeof r.upstream_key, "AIzaSyTest");
    return r;
}

/* ------------------------------------- Test 1: Single Image with Extension */
TEST_CASE(test_gemini_vision_single_image)
{
    model_rec_t route = make_gemini_route();
    const char* in_body =
        "{\"model\":\"gemini-1.5-pro\","
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"text\",\"text\":\"Describe this photo\"},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://example.com/cat.png\"}}"
        "]}]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n    = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int         rc   = provider_gemini_build(&route, in_body, url, sizeof url,
                                             hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");
    TEST_ASSERT(body != NULL, "body not null");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");

    json_t* contents = json_object_get(out, "contents");
    TEST_ASSERT(contents && json_is_array(contents) && json_array_size(contents) == 1, "1 content");

    json_t* entry = json_array_get(contents, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(entry, "role")), "user") == 0, "role=user");

    json_t* parts = json_object_get(entry, "parts");
    TEST_ASSERT(parts && json_is_array(parts) && json_array_size(parts) == 2, "2 parts");

    /* Part 0: text */
    json_t* p0 = json_array_get(parts, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(p0, "text")), "Describe this photo") == 0,
                "text match");

    /* Part 1: fileData */
    json_t* p1 = json_array_get(parts, 1);
    json_t* fd = json_object_get(p1, "fileData");
    TEST_ASSERT(fd && json_is_object(fd), "fileData present");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fd, "fileUri")), "https://example.com/cat.png") == 0,
                "fileUri match");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fd, "mimeType")), "image/png") == 0,
                "mimeType=image/png");

    json_decref(out);
    free(body);
}

/* --------------------------------------- Test 2: Fallback MIME Type */
TEST_CASE(test_gemini_vision_unknown_mime_fallback)
{
    model_rec_t route = make_gemini_route();
    const char* in_body =
        "{\"model\":\"gemini-1.5-pro\","
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://example.com/media?file=123\"}}"
        "]}]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n    = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int         rc   = provider_gemini_build(&route, in_body, url, sizeof url,
                                             hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");

    json_t* contents = json_object_get(out, "contents");
    json_t* entry = json_array_get(contents, 0);
    json_t* parts = json_object_get(entry, "parts");
    TEST_ASSERT(parts && json_array_size(parts) == 1, "1 part");

    json_t* fd = json_object_get(json_array_get(parts, 0), "fileData");
    TEST_ASSERT(fd != NULL, "fileData present");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fd, "fileUri")), "https://example.com/media?file=123") == 0,
                "fileUri match");
    TEST_ASSERT(strcmp(json_string_value(json_object_get(fd, "mimeType")), "image/jpeg") == 0,
                "mimeType fallback to image/jpeg");

    json_decref(out);
    free(body);
}

/* --------------------------------- Test 3: Text-only String Compatibility */
TEST_CASE(test_gemini_vision_text_only_string)
{
    model_rec_t route = make_gemini_route();
    const char* in_body =
        "{\"model\":\"gemini-1.5-pro\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"Hello Gemini\"}]}";

    char        url[512];
    const char* hdrs[4][2];
    int         n    = 0;
    char*       body = NULL;
    size_t      blen = 0;
    int         rc   = provider_gemini_build(&route, in_body, url, sizeof url,
                                             hdrs, &n, &body, &blen);
    TEST_ASSERT(rc == 0, "build ok");

    json_t* out = json_loads(body, 0, NULL);
    TEST_ASSERT(out != NULL, "valid json");

    json_t* contents = json_object_get(out, "contents");
    json_t* entry = json_array_get(contents, 0);
    json_t* parts = json_object_get(entry, "parts");
    TEST_ASSERT(parts && json_array_size(parts) == 1, "1 part");
    json_t* p0 = json_array_get(parts, 0);
    TEST_ASSERT(strcmp(json_string_value(json_object_get(p0, "text")), "Hello Gemini") == 0,
                "text match");

    json_decref(out);
    free(body);
}
```

- [ ] **Step 2: Register Gemini vision unit tests in `tests/unit/run_tests.c`**

In `tests/unit/run_tests.c`, after `test_register("gemini_sse_function_call_stream", ...);` (around line 382), add:

```c
    extern void test_gemini_vision_single_image(void);
    extern void test_gemini_vision_unknown_mime_fallback(void);
    extern void test_gemini_vision_text_only_string(void);
    test_register("gemini_vision_single_image", test_gemini_vision_single_image);
    test_register("gemini_vision_unknown_mime_fallback", test_gemini_vision_unknown_mime_fallback);
    test_register("gemini_vision_text_only_string", test_gemini_vision_text_only_string);
```

- [ ] **Step 3: Build and run test to verify failure**

Run:
```bash
cmake --build .build --target aigate_unit_tests
cd .build && ctest -R "gemini_vision" --output-on-failure
```
Expected: FAIL on `test_gemini_vision_single_image` (because current Gemini adapter parses `plain_content` as string and outputs empty text block for array contents).

---

### Task 4: Gemini Vision - Implement `gemini_infer_mime_type` and Content Parts Building

**Files:**
- Modify: `src/upstream/provider_gemini.c:10-35, 230-245`

- [ ] **Step 1: Add `#include <strings.h>` and `gemini_infer_mime_type` helper**

In `src/upstream/provider_gemini.c`, add `#include <strings.h>` around line 13.
Then, add the `gemini_infer_mime_type` function around line 27:

```c
#include <strings.h>
```

```c
/** @brief Infer image MIME type from URL path extension. Defaults to image/jpeg. */
static const char*
gemini_infer_mime_type(const char* url)
{
    if (!url) {
        return "image/jpeg";
    }
    const char* q = strchr(url, '?');
    size_t path_len = q ? (size_t)(q - url) : strlen(url);
    if (path_len >= 4 && strncasecmp(url + path_len - 4, ".png", 4) == 0) {
        return "image/png";
    }
    if (path_len >= 5 && strncasecmp(url + path_len - 5, ".jpeg", 5) == 0) {
        return "image/jpeg";
    }
    if (path_len >= 4 && strncasecmp(url + path_len - 4, ".jpg", 4) == 0) {
        return "image/jpeg";
    }
    if (path_len >= 5 && strncasecmp(url + path_len - 5, ".webp", 5) == 0) {
        return "image/webp";
    }
    if (path_len >= 4 && strncasecmp(url + path_len - 4, ".gif", 4) == 0) {
        return "image/gif";
    }
    AIGATE_LOG_DEBUG("gemini_infer_mime_type: cannot infer from url, defaulting to image/jpeg");
    return "image/jpeg";
}
```

- [ ] **Step 2: Update parts building loop in `provider_gemini_build`**

In `src/upstream/provider_gemini.c`, replace lines 231-240:

```c
                } else {
                    /* user or assistant plain message / multimodal parts */
                    json_t* entry = json_object();
                    json_object_set_new(entry, "role", json_string(gemini_role));
                    json_t* parts = json_array();

                    if (jcontent && json_is_array(jcontent)) {
                        size_t  pi;
                        json_t* p;
                        json_array_foreach(jcontent, pi, p)
                        {
                            json_t*     jtype = json_object_get(p, "type");
                            const char* ptype = (jtype && json_is_string(jtype)) ? json_string_value(jtype) : "";
                            if (strcmp(ptype, "text") == 0) {
                                json_t* jt = json_object_get(p, "text");
                                if (jt && json_is_string(jt)) {
                                    json_t* tp = json_object();
                                    json_object_set_new(tp, "text", json_string(json_string_value(jt)));
                                    json_array_append_new(parts, tp);
                                }
                            } else if (strcmp(ptype, "image_url") == 0) {
                                json_t* jiu = json_object_get(p, "image_url");
                                json_t* ju = jiu ? json_object_get(jiu, "url") : NULL;
                                if (ju && json_is_string(ju)) {
                                    const char* u = json_string_value(ju);
                                    const char* mime = gemini_infer_mime_type(u);
                                    json_t*     fd = json_object();
                                    json_object_set_new(fd, "fileUri", json_string(u));
                                    json_object_set_new(fd, "mimeType", json_string(mime));
                                    json_t* fdp = json_object();
                                    json_object_set_new(fdp, "fileData", fd);
                                    json_array_append_new(parts, fdp);
                                } else {
                                    AIGATE_LOG_WARN("image_url part missing url");
                                }
                            } else {
                                AIGATE_LOG_WARN("gemini: unsupported content part type '%s'", ptype);
                            }
                        }
                    } else {
                        json_t* part = json_object();
                        json_object_set_new(part, "text", json_string(plain_content));
                        json_array_append_new(parts, part);
                    }
                    json_object_set_new(entry, "parts", parts);
                    json_array_append_new(contents, entry);
                }
```

- [ ] **Step 3: Build and run all tests**

Run:
```bash
cmake --build .build --target aigate_unit_tests
cd .build && ctest --output-on-failure
```
Expected: 100% tests passed.

- [ ] **Step 4: Commit Gemini vision support**

```bash
git add src/upstream/provider_gemini.c tests/unit/upstream/test_provider_gemini_vision.c tests/unit/run_tests.c
git commit -m "feat(upstream): ✨ add Gemini vision multimodal image support"
```
