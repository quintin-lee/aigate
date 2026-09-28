# Test Helper Comments Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 tests/ 剩余 4 处辅助函数补 Doxygen 注释（纯加行）。

**Architecture:** 单批次直接编辑两文件：mock_upstream.c 加 server_thread 块注释；run_tests.c 加 test_fn/test_register/main 块注释；随后三验证并单提交。

**Tech Stack:** C (Doxygen `/** @brief */` 风格，与前四轮一致），CMake/ctest。

---

### Task 1: mock_upstream.c 加 server_thread 注释

**Files:**
- Modify: `tests/unit/mock_upstream.c:42-43`

签名跨两行，注释加在返回类型行（`static void*`）之上：

- [ ] **Step 1: 加注释**

```c
/** @brief Accept 循环：按请求路径回固定响应，并记录末次请求的路径与 body 供断言。 */
static void*
server_thread(void* arg)
```

- [ ] **Step 2: 确认 diff 只有加行**

Run: `git diff --stat && git diff -U0 -- tests/unit/mock_upstream.c | grep '^+' | grep -v '^+++' | grep -vE '/\*|\*/'`
Expected: stat 显示 1 insertion、0 deletions；第二条命令无输出。

### Task 2: run_tests.c 加 3 处注释

**Files:**
- Modify: `tests/unit/run_tests.c:13`, `tests/unit/run_tests.c:20-21`, `tests/unit/run_tests.c:30-31`

- [ ] **Step 1: 加 test_fn 注释**

```c
/** @brief 单个测试用例函数指针。 */
typedef void (*test_fn)(void);
```

- [ ] **Step 2: 加 test_register 注释**

```c
/** @brief 注册一个测试用例。
 *  @param name 用例名。@param fn 用例函数。 */
void
test_register(const char* name, test_fn fn)
```

- [ ] **Step 3: 加 main 注释**

```c
/** @brief 顺序跑完全部注册用例。
 *  @return 失败用例数，0 表全过。 */
int
main(void)
```

- [ ] **Step 4: 确认 diff 只有加行**

Run: `git diff -U0 -- tests/unit/run_tests.c | grep '^+' | grep -v '^+++' | grep -vE '/\*|\*/'`
Expected: 无输出。

### Task 3: 三验证并提交

- [ ] **Step 1: 零警告构建**

Run: `cmake --build build -j 2>&1 | grep -iE "warning|error"`
Expected: 无输出。

- [ ] **Step 2: ctest 全绿**

Run: `ctest --test-dir build 2>&1 | grep -E "tests passed|tests failed"`
Expected: `100% tests passed out of 6`

- [ ] **Step 3: 全范围纯注释机检**

Run: `git diff -U0 -- 'tests/*.c' 'tests/*/*.c' | grep '^+' | grep -v '^+++' | grep -vE '/\*|\*/'`
Expected: 无输出。

- [ ] **Step 4: 提交**

```bash
git add tests/unit/mock_upstream.c tests/unit/run_tests.c
git commit -m "docs: annotate test helpers with Doxygen comments"
```

Expected: 2 files changed，约 4 insertions(+)、0 deletions。
