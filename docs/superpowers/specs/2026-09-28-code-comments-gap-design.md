# 代码注释查漏补缺设计（方案 A）

**日期：** 2026-09-28
**动机：** 可读性（查漏补缺）。此前 7 批 Doxygen 已覆盖全部 static 函数与几乎全部公开函数。
**批准方案：** A —— 一行注释补全局变量 + metrics_render 补 @brief，一次提交。

## 1. 实测缺口结论

- 头文件公开函数：宽窗口（10 行）扫描仅 1 处真缺口（metrics_render）；其余为多行签名/长注释块导致的误报。
- static 全局量：约 20 处无注释（互斥锁/表/配置旋钮）。
- 明确不动：`/* 分组线 */`、长函数内部分支、schema_sql.h 内 SQL 字符串。

## 2. 范围清单（约 21 处）

A 类（单行 `/** 用途 */`，紧贴声明上一行）：
core/aigate_log.c `g_log_mtx`；core/secrets.c `HEXD`；main.c `g_stop`；
observe/metrics.c `g_failovers`、`g_failover_mtx`、`g_failover_warned`、`BUCKET_LE`；
server/admin_api.c `g_lockout_fails`、`g_lockout_window_s`、`g_lockout_pool`、`g_lockout_sha`、`g_lockout`、`g_lockout_mtx`；
upstream/model_router.c `g_rr_counter`；upstream/provider_adapter.c `s_adapters`；
upstream/provider_openai.c `s_responses_bearer_auth`；
upstream/upstream_client.c `g_curl_sh`、`g_curl_sh_dns_mtx`、`g_curl_sh_ssl_mtx`、`g_curl_once`、`g_curl_tkey`、`g_curl_tkey_once`。

B 类（完整 Doxygen @brief/@param/@return）：src/observe/metrics.h `metrics_render`
（现有注释只列指标名，补行为说明：把 usage_meter 快照渲染为 Prometheus 文本，写入 out/cap）。

## 3. 格式

- 中文，一句话用途 + 关键约束；可配置项注明默认值。
- 与已落地的 7 批风格一致；纯注释行，不碰代码。

## 4. 验证标准

1. 零警告全量构建；2. ctest 6/6 与基线一致；
3. 纯注释机检（新增行全为注释/空白，零代码改动）；
4. 单次提交 `docs: annotate globals and metrics_render`。

## 5. 非目标

复杂逻辑行内注释（方案 B，已否决：标准主观、易成噪音）；中英转换；Doxygen 建站。
