# AIGATE vs New API v2 差距分析:评估 + 路线图(设计)

日期:2026-09-23 · 状态:approved(brainstorming 产物)
前置:v1 差距文档 `docs/superpowers/specs/2026-09-23-aigate-vs-newapi-gap-analysis.md`(保留作历史)

## 0. 目标与范围

- **目标**:对 aigate(C17 单二进制网关)与 QuantumNous/new-api(Go 平台)做一次**源码级**能力对照,重判 16 项差距,补充 v1 未覆盖的新差异项,并产出下一立项的路线图。
- **场景锚定**:内部单组织自用(非多租户运营)。new-api 多租户专属能力默认「不追」,除非其单用户退化形态改动面可控。
- **范围外**:aigate 代码改动(本 spec 只产出分析报告 + 路线图;实施走 writing-plans 单独立项)。

## 1. 证据采集(实施前置)

- **克隆失败兜底**:若 shallow clone 不可得(网络/私有仓库),退回 web_search + 官方 README/release notes 的文档级证据,并在 §0 显式标注证据降级;锚点方法不变。
1. **浅克隆 new-api**:`git clone --depth 1 https://github.com/QuantumNous/new-api ~/Data/source/go/new-api`。报告头部记录克隆 HEAD commit hash,锚点(file:line)以此为基准可复现。
2. **骨架映射**(主线程):读 `controller/`、`relay/`、`billing/`、`channel/`、`task/`、`model/` 的目录树与承重入口文件,划定 5 个 scout 领域边界。
3. **5 个并行 scout**(只读子代理,各返回 ≤15 条带 new-api `文件:行` 锚点的压缩发现):

| 领域 | 关注点 | aigate 对照系 |
|---|---|---|
| S1 计费/配额 | 定价表达式引擎、用户余额、扣减时机、缓存折扣 | usage_daily + 日配额(P1-2 成本核算候选) |
| S2 协议中继 | RelayKit 四协议转换边界、流式/SSE 处理、/v1/responses 端点 | provider_openai/anthropic/gemini(P1-3 候选) |
| S3 通道管理 | 优先级/权重/亲和、健康测试、自动降级、重试策略 | model_router + circuit_breaker(P1-4 候选;aigate 反超项复核) |
| S4 任务系统 | 图像/视频任务模型、JS 插件、异步队列 | aigate 无对应(「不追」项复核:确认单用户退化形态成本) |
| S5 身份/运维 | 用户/组/角色、会话、审计日志、admin 端点面 | admin token 族 + 锁留(「不追」项复核) |

- **scout 产出约束**:每条发现 = 能力描述 + new-api `文件:行` + 「aigate 场景下是否成立」预判;不写 Go 实现细节,不翻译新-api 代码。

## 2. v2 报告结构

输出:`docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2.md`

```
§0 定位与证据基准(两仓 HEAD/锚点方法、v1 与 v2 关系)
§1 能力矩阵 v2(v1 16 行全部重判:采纳/暂缓/不追 + 新增行,三态各附理由)
§2 领域深潜(S1-S5 发现,aigate 侧 file:line 锚点,差距实质)
§3 判定演变(v1→v2 diff:翻转/强化条目逐条给理由,禁止静默翻转)
§4 状态回填(P0-1 审计日志已交付:8 提交 + 验收证据;硬化 20 项完成清单)
§5 路线图(候选 {P1-2 成本核算, P1-3 responses, P1-4 通道测试, 新发现项}
   按 场景价值÷改动面 排序,标注依赖,给出建议下一立项)
```

## 3. 判定准则

- 三态词汇沿用 v1:**采纳**(场景收益高、改动可控)/ **暂缓**(有收益但依赖外部条件,附触发条件)/ **不追**(场景不成立,附理由)。
- aigate 反超项(熔断器 + 探活、HDR 延迟直方图、4xx 原样透传、master-key 加密 provider key、优先级分层 failover)**必须**在 S2/S3 中对照 new-api 实现复核,确认反超成立后再保留,不允许沿袭 v1 结论不复核。
- 每条「采纳/不追」判定必须两侧锚点齐:aigate `文件:行` + new-api `文件:行`(或明确标注「文档级证据」)。
- 路线图排序准则:**场景价值 ÷ 改动面**,附条目化计算依据,不允许直觉排序。
- YAGNI:表达式计费引擎、多租户、插件系统默认砍;内部用「单价 × 用量 + 缓存折扣系数」即止(沿用 v1 §2.2 结论,除非 S1 证据推翻)。

## 4. 中间产物

- scout 原始发现存档:`docs/superpowers/reports/2026-09-23-aigate-vs-newapi-v2-scout-notes.md`(逐领域一节,保留原始锚点,供报告引用与复现)。
- 克隆留在 `~/Data/source/go/new-api`(与 aigate 的 `~/Data/source/c_cpp/aigate` 对称,长期可作锚点参考,不删除)。

## 5. 验收(报告自查单)

- [ ] §1 矩阵无静默翻转:v1→v2 每条判定变化在 §3 有理由
- [ ] 两侧锚点覆盖率 100%(或显式标注证据级别)
- [ ] 反超项复核记录在 §2 S2/S3
- [ ] §5 排序附价值/成本条目
- [ ] scout notes 已存档
- [ ] 报告 + notes 已提交 git(gitmoji)

## 6. 不做

- 不修改 aigate 代码(实施走 writing-plans)
- 不下钻 new-api 的 React 前端实现(控制台 UI 能力只按端点/行为描述)
- 不翻译/复制 new-api 代码(AGPL,仅读行为,锚点引用文件名:行号即可)
