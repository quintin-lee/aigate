# 开发指南

## 构建变体

默认：

```bash
cmake -B build -S . && cmake --build build -j
```

Debug：

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j
```

Debug 下顶层 `CMakeLists.txt` 会自动关掉 CivetWeb 的 ASan——CivetWeb 的
Debug 构建自带 sanitizer，会把 `-fsanitize` 泄漏到主可执行文件的链接行导致
`__ubsan_*` / `__asan_*` 未定义（见 `CMakeLists.txt` 内注释）。

Sanitizer（address + undefined，显式开启才加）：

```bash
cmake -B build-asan -S . -DAIGATE_SANITIZERS=ON && cmake --build build-asan -j
```

`build-asan/` 已在 `.gitignore`。构建目录约定：`build/` 是唯一文档化目录；
`.build/`、`build-asan/`、`Testing/` 等均为忽略项，不要提交。

> 新人一站式入口见 [ONBOARDING.md](ONBOARDING.md)。

## 加模块 / 加测试（镜像规则）

- 源码 `src/<layer>/<module>.c` ↔ 测试 `tests/unit/<layer>/test_<module>.c`，
  同名同层（7 层见 [docs/README.md](README.md)）。
- 两边 `file(GLOB … CONFIGURE_DEPENDS)`，新建文件无需改 CMake，
  下次构建自动感知。
- 测试用例是无框架的 assert 风格（`tests/unit/run_tests.h` 的 `TEST_CASE`）：
  新用例要在 `tests/unit/run_tests.c` 里做两件事——顶部加 `extern` 声明，
  `main()` 里加一行 `test_register("用例名", test_函数名);`。
  runner 退出码 = 失败用例数，`ctest` 只看这一项（`unit`）。

## 改管理后台页面

`web/admin.html` 是单页运维面板，构建时由 `scripts/embed_html.py` 烘焙进
二进制（`build/generated/admin_ui_html.h`）。改完页面直接重新构建即可，
custom command 会按依赖自动重跑脚本。

## 代码风格

- C17，`-Wall -Wextra -Werror`（lib 与可执行文件均开，见顶层 `CMakeLists.txt`）。
- 根目录 `.clang-format` 是唯一风格配置，提交前对改动文件跑
  `clang-format -i`。
- 内嵌 schema 是生成的：`src/store/schema_sql.h` 头部注明 do not edit，
  改表结构先改 `schema/schema.sql` 再同步（目前靠手工保持一致，改完务必
  diff 两边）。

## 本地联调常用命令

```bash
# 只起依赖（网关本地跑）
docker compose up postgres redis

# 冒烟（另见 tests/integration/smoke.sh 头部注释）
TEST_PG_DSN="postgresql://postgres:postgres@127.0.0.1:5432/aigate_test" ./tests/integration/smoke.sh
```

环境变量全量清单见根目录 [.env.example](../.env.example)（含注释说明的
默认值与越界回退行为）。
