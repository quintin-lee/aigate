# 新人上手包设计（Docs Onboarding）

日期：2026-09-29。动机：新人上手。范围：用户文档 + API 文档。深度：完整上手包。

## §1 范围（已批准）

新建 `docs/ONBOARDING.md` 一站式上手包，5 节：

1. **依赖与构建**：前置依赖 + `cmake -B build` 标准构建（与根 README 同命令）。
2. **配置速查表**：核心环境变量表格（变量｜默认｜说明），以 `docs/DEVELOPMENT.md` 与 `.env.example` 为准。
3. **运行 + 首个请求演练**：启动服务 + 一条 `curl` 打通推理链路（含预期返回说明）。
4. **后台 + 测试**：管理后台访问 + `ctest` 全绿验证。
5. **排障 FAQ**：≥5 条（端口占用/数据库连不上/密钥缺失/构建失败/测试挂起）。

边界：现有文件正文一律不动；`docs/architecture/` 不动。

## §2 格式与验证（已批准）

- 格式：照抄根 README 命令块风格；配置项用表格；FAQ 用 `**Q:** / **A:**`；中文。
- 导航：根 README、docs/README.md、docs/DEVELOPMENT.md 各加 1 行指向 ONBOARDING 的链接。
- Doxyfile 不动（INPUT 已含 README，无需改）。
- 验证：ONBOARDING 内每条命令真跑一遍；导航 diff 各 1 行；工作树仅新增 1 文件 + 3 行修改。

## 方案

采用方案 A（独立 ONBOARDING.md + 三处导航）。备选 B（拆散补现有文件）弃用：改动散；备选 C（最小跑通）弃用：不满足"完整上手包"深度。
