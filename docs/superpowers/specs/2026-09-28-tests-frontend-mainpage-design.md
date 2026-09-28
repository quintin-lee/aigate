# Tests＋前端＋首页注释 Design

日期：2026-09-28｜前置：第八轮 doxygen-groups（全仓 doxygen 0 警告）｜动机：查漏补缺收尾

## §1 范围清单（3 批，方案 A）

- Task1 tests 辅助 static：152 处（总数 161，mock_upstream server_thread 与 run_tests 3 处已补），19 文件；大户 admin_api 36、pg_store 35、aigate_core 18。169 个 TEST_CASE 名自解释，不动。
- Task2 admin.html JS：107 个无注释函数（共 122），每函数上一行加一句中文 `//` 说明。embed_html.py 已有 docstring，不动。
- Task3 首页：Doxyfile 加一行 `USE_MDFILE_AS_MAINPAGE = README.md`，复用根 README，零新文件。

## §2 格式约定

- tests C：沿用前轮 `/** @brief … */`（多行签名注在返回类型首行；@param/@return 按需）。
- JS：只写 `//` 单行，不引入 `/** */`。
- Doxyfile：只加首页一行，其余不动。

## §3 验证标准（每批独立执行）

1. 构建零警告；2. ctest 6/6；3. 纯注释机检（加行 `grep -vE '/\*|\*/|//'` 为空；JS 批改用 `grep -vE '^\+\s*//'` 为空）；4. doxygen 零警告。
提交信息：`docs: annotate test helpers`／`docs: annotate admin UI scripts`／`docs: set README as doxygen mainpage`；每次只 `git add` 本批路径。
