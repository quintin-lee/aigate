# 测试辅助函数注释设计

> 范围：tests/ 辅助函数收尾补注（前四轮已覆盖 src 全部符号）。TEST_CASE 不动，embed_html.py 已有 docstring 不动，无 union 缺口。实测后收窄为 4 块（mock 公开函数 .h 已有文档，不重复）。

## §1 范围清单（4 块，2 文件）

实测修正：`mock_upstream.h` 8 个公开函数已有完整 Doxygen（start/stop/base/
fail_all/request_count/status/last_body/last_path），按既定约定（公开注 .h、
static 注 .c）无需重复加注。mock 真缺口仅 static 的 `server_thread` 1 处。

### tests/unit/mock_upstream.c（1 处）

- `server_thread`（static）：accept 循环 + 按路径回固定响应 + 记录末次请求。

### tests/unit/run_tests.c（3 处）

- `test_fn` 类型定义：单个测试用例函数指针。
- `test_register`：注册用例进全局表。
- `main`：跑完全部用例并返回失败数。

### 明确不动

- 169 个 TEST_CASE：一行一名自解释，不加注（本轮既定范围）。
- `scripts/embed_html.py`：已有 docstring。
- union：全仓无缺口。

## §2 格式（Doxygen，单行优先）

沿用前四轮标准：`/** @brief … */` 单行优先；有参数/返回值加
`@param` / `@return`。示例写法：

```c
/** @brief 启动 mock 上游服务器并返回监听端口。 */
```

```c
/** @brief 注册一个测试用例。
 *  @param name 用例名。@param fn 用例函数。 */
```

```c
/** @brief 跑完全部注册用例。
 *  @return 失败用例数，0 表全过。 */
```

## §3 验证与提交

- 单批次、单提交：`docs: annotate test helpers`，只 `git add` 本批 2 文件。
- 三验证沿用前四轮：零警告构建、ctest 6/6、纯注释机检
  （`git diff -U0 | grep '^+' | grep -v '^+++' | grep -vE '/\*|\*/'` 为空）。
