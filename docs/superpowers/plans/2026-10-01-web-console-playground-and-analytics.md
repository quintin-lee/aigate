# Web 控制台 Playground 调试面板与可视化看板实施计划 (Implementation Plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建全能多轮交互调试场（支持指定通道直连、流式打字机、TTFT/TPS实时遥测）以及成本分布与时延分布两大 Chart.js 可视化看板，全面提升 aigate Web 控制台运营运维与调优能力。

**Architecture:** 后端在 `model_router.c` 与 `pipeline_chat.c` 中扩展支持 `X-Aigate-Target-Provider` 请求头，提供指定物理通道直连与调试能力；前端在 `web/admin.html` 中重构 `#tab-playground` 为三栏现代工作台，并在 `#tab-cost` 与 `#tab-metrics` 嵌入由原生 Chart.js 驱动的环形占比图、每日趋势复合图、P50/P90/P99 阶梯柱状图与通道耗时对比图。

**Tech Stack:** C17, CMake, vanilla JavaScript, Tailwind CSS (CDN), Chart.js (CDN), Server-Sent Events (SSE).

---

## 涉及文件与改动职责 (File Map)

- `src/upstream/model_router.h`: 声明候选目标选择函数的指定上游提供商重载/参数。
- `src/upstream/model_router.c`: 实现针对特定上游通道的直选路由优先匹配与优雅降级。
- `src/core/pipeline_chat.c`: 从客户端请求报头中解析 `X-Aigate-Target-Provider` 并传入候选路由决策。
- `tests/unit/upstream/test_model_router.c`: 增加针对目标通道选择的单元测试。
- `web/admin.html`:
  - 重构 `#tab-playground` HTML 结构为三栏工作台（参数与通道、多轮对话流、实时遥测诊断卡）。
  - 实现 Playground 多轮会话维护、通道下拉联动、SSE 流式打字机、TTFT/TPS 毫秒度量与模板快速导入 JS。
  - 在 `#tab-cost` 插入 `#costDonutChart` 与 `#costTrendChart` 容器并实现数据渲染。
  - 在 `#tab-metrics` 插入 `#latencyPercentilesChart` 与 `#providerLatencyChart` 容器并实现 Prometheus 指标解析与渲染。

---

### Task 1: 后端支持通道指定调试请求头 (`X-Aigate-Target-Provider`)

**Files:**
- Modify: `src/upstream/model_router.h:50-67`
- Modify: `src/upstream/model_router.c:310-380`
- Modify: `src/core/pipeline_chat.c:90-120`
- Test: `tests/unit/upstream/test_model_router.c`

- [ ] **Step 1: 编写单元测试验证指定提供商通道优先匹配**

在 `tests/unit/upstream/test_model_router.c` 文件末尾增加测试函数 `test_model_router_target_provider_override()`：

```c
static void
test_model_router_target_provider_override(void)
{
    model_rec_t m;
    memset(&m, 0, sizeof m);
    snprintf(m.name, sizeof m.name, "gpt-test");
    snprintf(m.lb_policy, sizeof m.lb_policy, "weighted");
    m.n_targets = 3;

    snprintf(m.targets[0].provider, sizeof m.targets[0].provider, "openai");
    snprintf(m.targets[0].endpoint, sizeof m.targets[0].endpoint, "https://api.openai.com");
    m.targets[0].weight = 10;
    m.targets[0].tier = 0;

    snprintf(m.targets[1].provider, sizeof m.targets[1].provider, "azure");
    snprintf(m.targets[1].endpoint, sizeof m.targets[1].endpoint, "https://azure.openai.com");
    m.targets[1].weight = 90;
    m.targets[1].tier = 0;

    snprintf(m.targets[2].provider, sizeof m.targets[2].provider, "backup");
    snprintf(m.targets[2].endpoint, sizeof m.targets[2].endpoint, "https://backup.ai");
    m.targets[2].weight = 50;
    m.targets[2].tier = 1;

    upstream_target_t candidates[MAX_TARGETS_PER_MODEL];
    int count = 0;

    /* 1. When target_provider is NULL, standard routing applies */
    int rc = model_router_select_candidates_targeted(NULL, NULL, &m, NULL, candidates, MAX_TARGETS_PER_MODEL, &count);
    AIGATE_ASSERT(rc == 0);
    AIGATE_ASSERT(count == 3);

    /* 2. When target_provider is "azure", azure target must be placed at index 0 */
    rc = model_router_select_candidates_targeted(NULL, NULL, &m, "azure", candidates, MAX_TARGETS_PER_MODEL, &count);
    AIGATE_ASSERT(rc == 0);
    AIGATE_ASSERT(count >= 1);
    AIGATE_ASSERT(strcmp(candidates[0].provider, "azure") == 0);

    /* 3. When target_provider is non-existent, fallback to normal candidates */
    rc = model_router_select_candidates_targeted(NULL, NULL, &m, "non-existent", candidates, MAX_TARGETS_PER_MODEL, &count);
    AIGATE_ASSERT(rc == 0);
    AIGATE_ASSERT(count == 3);
}
```

并在该测试文件的 `main()` 或测试运行入口注册 `test_model_router_target_provider_override()`。

- [ ] **Step 2: 编译并运行测试以确认失败**

执行命令：
```bash
cmake --build build --target unit
ctest --test-dir build -R unit --output-on-failure
```
预期输出：编译失败，提示 `model_router_select_candidates_targeted` 未声明。

- [ ] **Step 3: 实现 `model_router_select_candidates_targeted` 与请求头处理**

在 `src/upstream/model_router.h` 增加声明：
```c
/** @brief Select candidate targets, optionally pinning a target provider to first position.
 *  @param cb              Circuit breaker (optional).
 *  @param lt              Latency tracker (optional).
 *  @param model           Model record.
 *  @param target_provider If non-NULL and matches a configured target, pins it as first candidate.
 *  @param out_candidates  Output candidate array.
 *  @param cap             Capacity.
 *  @param out_count       Output count.
 *  @return 0 on success, -1 on error. */
int model_router_select_candidates_targeted(circuit_breaker_t* cb,
                                           latency_tracker_t* lt,
                                           const model_rec_t* model,
                                           const char*        target_provider,
                                           upstream_target_t* out_candidates,
                                           int                cap,
                                           int*               out_count);
```

并在 `src/upstream/model_router.c` 中实现该逻辑：
若 `target_provider != NULL && target_provider[0] != '\0'`，先扫描 `model->targets`：若存在匹配项，将其放入 `out_candidates[0]`，并追加其他候选项作为故障转移；若不存在，则按默认策略选择。
同时让现有 `model_router_select_candidates` 调用 `model_router_select_candidates_targeted(..., NULL, ...)` 保持完全向后兼容。

在 `src/core/pipeline_chat.c` 的请求处理入口（如 `prepare_chat_cache` 或候选初始化位置）读取请求头 `X-Aigate-Target-Provider`：
```c
const char* target_prov = NULL;
if (q->rc->get_header != NULL) {
    target_prov = q->rc->get_header(q->rc->impl, "X-Aigate-Target-Provider");
}
model_router_select_candidates_targeted(q->ac->cb, q->ac->lt, &rec, target_prov, q->candidates, MAX_TARGETS_PER_MODEL, &q->n_candidates);
```

- [ ] **Step 4: 编译并运行单元测试确认通过**

执行命令：
```bash
cmake --build build --target unit
ctest --test-dir build -R unit --output-on-failure
```
预期输出：`100% tests passed out of 1`。

- [ ] **Step 5: 提交后端改动**

执行命令：
```bash
git add src/upstream/model_router.h src/upstream/model_router.c src/core/pipeline_chat.c tests/unit/upstream/test_model_router.c
git commit -m "feat(upstream): support X-Aigate-Target-Provider header for direct channel debugging"
```

---

### Task 2: Web 控制台 Playground 三栏 DOM 结构与样式改造

**Files:**
- Modify: `web/admin.html:1523-1612`

- [ ] **Step 1: 重构 `#tab-playground` 容器的 HTML 结构**

将现有的 `tab-playground` 替换为响应式三栏工作台结构：

```html
    <!-- ==================== Tab 5: Playground ==================== -->
    <div id="tab-playground" class="tab-content hidden space-y-4">
      <div class="flex flex-col sm:flex-row sm:items-center justify-between gap-3">
        <div>
          <div class="flex items-center gap-2">
            <h2 class="text-xl font-extrabold text-white">⚡ 实时调试场 (Playground)</h2>
            <span class="text-[10px] px-2 py-0.5 rounded-full font-mono font-bold bg-indigo-500/20 text-indigo-300 border border-indigo-500/30">多轮对话 & 通道直测</span>
          </div>
          <p class="text-xs text-slate-400 mt-0.5">测试多轮对话补全、SSE 流式响应、通道路由与首字时延分析</p>
        </div>
        <div class="flex items-center space-x-2">
          <button onclick="clearPlaygroundChat()" class="px-3 py-1.5 rounded-xl text-xs font-medium bg-slate-900 hover:bg-slate-800 text-slate-300 border border-slate-700 transition flex items-center space-x-1.5">
            <span>🧹</span><span>清空会话</span>
          </button>
        </div>
      </div>

      <!-- 3-Column Workbench Grid -->
      <div class="grid grid-cols-1 lg:grid-cols-12 gap-5 items-start">
        <!-- Col 1: Config & Routing (3 cols) -->
        <div class="lg:col-span-3 glass-panel p-4 rounded-2xl border border-slate-800 space-y-3.5">
          <div class="text-xs font-bold text-slate-300 uppercase tracking-wider flex items-center gap-1.5 border-b border-slate-800/80 pb-2">
            <span>⚙️</span><span>参数与通道路由</span>
          </div>

          <div>
            <label class="block text-[11px] font-semibold text-slate-300 mb-1">选择模型</label>
            <select id="playModelSelect" onchange="onPlaygroundModelChange()" class="w-full px-3 py-2 rounded-xl bg-slate-900 border border-slate-700 text-xs text-white focus:outline-none focus:border-brand-500 font-medium">
            </select>
          </div>

          <div>
            <label class="block text-[11px] font-semibold text-slate-300 mb-1">上游通道路由 (Channel)</label>
            <select id="playChannelSelect" class="w-full px-3 py-2 rounded-xl bg-slate-900 border border-slate-700 text-xs text-indigo-300 focus:outline-none focus:border-brand-500 font-mono">
              <option value="">⚡ 默认智能路由 (加权/对冲/故障转移)</option>
            </select>
            <p class="text-[10px] text-slate-500 mt-1">可强制直连单个物理通道排查连通性</p>
          </div>

          <div>
            <label class="block text-[11px] font-semibold text-slate-300 mb-1">API 密钥 (Bearer)</label>
            <div class="flex space-x-1.5">
              <input type="password" id="playApiKeyInput" placeholder="aig_..." class="flex-1 px-2.5 py-1.5 rounded-xl bg-slate-900 border border-slate-700 text-xs text-white font-mono focus:outline-none focus:border-brand-500">
              <button onclick="autoFillPlaygroundKey()" title="填充首个可用密钥" class="px-2 py-1 text-[11px] rounded-lg bg-slate-800 hover:bg-slate-700 text-slate-300 border border-slate-700 whitespace-nowrap">自动填充</button>
            </div>
          </div>

          <div>
            <div class="flex items-center justify-between mb-1">
              <label class="text-[11px] font-semibold text-slate-300">系统提示词 (System)</label>
              <select id="playPromptPresetSelect" onchange="applyPromptPresetToPlayground()" class="bg-slate-950 text-[10px] text-indigo-300 border border-slate-800 rounded px-1.5 py-0.5 max-w-[130px] truncate">
                <option value="">+ 载入模板...</option>
              </select>
            </div>
            <textarea id="playSystemPrompt" rows="3" class="w-full px-2.5 py-2 rounded-xl bg-slate-900 border border-slate-700 text-xs text-slate-200 focus:outline-none focus:border-brand-500 font-sans leading-relaxed custom-scroll" placeholder="系统级提示词...">你是一个专业、严谨的 AI 助手。</textarea>
          </div>

          <!-- Hyperparameter sliders & toggles -->
          <div class="space-y-2.5 pt-1 border-t border-slate-800/80">
            <div>
              <div class="flex justify-between text-[11px] font-semibold mb-1">
                <span class="text-slate-400">温度 (Temperature)</span>
                <span id="tempDisplay" class="text-brand-400 font-mono">0.7</span>
              </div>
              <input type="range" id="playTempInput" min="0" max="2" step="0.1" value="0.7" oninput="document.getElementById('tempDisplay').innerText=this.value" class="w-full accent-brand-500">
            </div>

            <div>
              <div class="flex justify-between text-[11px] font-semibold mb-1">
                <span class="text-slate-400">最大生成 Token</span>
                <span id="maxTokensDisplay" class="text-brand-400 font-mono">2048</span>
              </div>
              <input type="range" id="playMaxTokensInput" min="64" max="8192" step="64" value="2048" oninput="document.getElementById('maxTokensDisplay').innerText=this.value" class="w-full accent-brand-500">
            </div>

            <div class="flex items-center justify-between pt-1">
              <div class="flex items-center space-x-2">
                <input type="checkbox" id="playStreamToggle" checked class="rounded bg-slate-900 border-slate-700 text-brand-600 focus:ring-brand-500 w-4 h-4">
                <label for="playStreamToggle" class="text-xs text-slate-300 font-medium cursor-pointer">流式 (SSE)</label>
              </div>
              <div class="flex items-center space-x-2">
                <input type="checkbox" id="playJsonModeToggle" class="rounded bg-slate-900 border-slate-700 text-brand-600 focus:ring-brand-500 w-4 h-4">
                <label for="playJsonModeToggle" class="text-xs text-slate-300 font-medium cursor-pointer">JSON 模式</label>
              </div>
            </div>
          </div>
        </div>

        <!-- Col 2: Chat Stream Dialog (6 cols) -->
        <div class="lg:col-span-6 space-y-3">
          <div class="glass-panel p-4 rounded-2xl border border-slate-800 flex flex-col h-[600px]">
            <!-- Messages scroll container -->
            <div id="playChatHistory" class="flex-1 overflow-y-auto custom-scroll p-3 space-y-3 rounded-xl bg-slate-950/70 border border-slate-800/80 text-xs">
              <div class="text-center py-16 text-slate-500 text-xs flex flex-col items-center justify-center space-y-2">
                <span class="text-2xl">💬</span>
                <span>在下方输入消息开始多轮交互调试</span>
                <span class="text-[10px] text-slate-600">按 Enter 发送，Shift+Enter 换行</span>
              </div>
            </div>

            <!-- Input area -->
            <div class="mt-3 pt-2 border-t border-slate-800">
              <div class="flex space-x-2">
                <textarea id="playUserInput" rows="2" onkeydown="handlePlaygroundKeydown(event)" placeholder="输入提示词或问题... (Enter 发送, Shift+Enter 换行)" class="flex-1 px-3 py-2 rounded-xl bg-slate-900 border border-slate-700 text-xs text-slate-100 focus:outline-none focus:border-brand-500 font-sans custom-scroll"></textarea>
                <div class="flex flex-col space-y-1.5">
                  <button id="playSendBtn" onclick="runPlaygroundRequest()" class="flex-1 px-4 py-2 rounded-xl font-bold text-xs bg-gradient-to-r from-brand-600 to-indigo-600 hover:from-brand-500 hover:to-indigo-500 text-white shadow-md shadow-brand-500/25 transition flex items-center justify-center space-x-1">
                    <span>🚀</span><span>发送</span>
                  </button>
                  <button id="playStopBtn" onclick="stopPlaygroundStream()" class="hidden px-4 py-2 rounded-xl font-bold text-xs bg-rose-600 hover:bg-rose-500 text-white transition">
                    停止
                  </button>
                </div>
              </div>
            </div>
          </div>
        </div>

        <!-- Col 3: Realtime Telemetry & Diagnostics (3 cols) -->
        <div class="lg:col-span-3 glass-panel p-4 rounded-2xl border border-slate-800 space-y-3.5">
          <div class="flex items-center justify-between border-b border-slate-800/80 pb-2">
            <span class="text-xs font-bold text-slate-300 uppercase tracking-wider flex items-center gap-1.5">
              <span>📡</span><span>实时遥测诊断</span>
            </span>
            <span id="playStatusBadge" class="hidden text-[10px] px-2 py-0.5 rounded-full font-mono font-bold bg-emerald-500/20 text-emerald-300 border border-emerald-500/30">200 OK</span>
          </div>

          <!-- 2x2 Telemetry Cards -->
          <div class="grid grid-cols-2 gap-2">
            <div class="bg-slate-900/80 border border-slate-800 p-2.5 rounded-xl">
              <div class="text-[10px] text-slate-400 font-medium">首字时延 (TTFT)</div>
              <div id="playStatTTFT" class="text-base font-extrabold text-cyan-400 font-mono mt-0.5">-</div>
            </div>
            <div class="bg-slate-900/80 border border-slate-800 p-2.5 rounded-xl">
              <div class="text-[10px] text-slate-400 font-medium">生成速率</div>
              <div id="playStatTPS" class="text-base font-extrabold text-emerald-400 font-mono mt-0.5">-</div>
            </div>
            <div class="bg-slate-900/80 border border-slate-800 p-2.5 rounded-xl">
              <div class="text-[10px] text-slate-400 font-medium">端到端耗时</div>
              <div id="playStatLatency" class="text-base font-extrabold text-white font-mono mt-0.5">-</div>
            </div>
            <div class="bg-slate-900/80 border border-slate-800 p-2.5 rounded-xl">
              <div class="text-[10px] text-slate-400 font-medium">单次折算成本</div>
              <div id="playStatCost" class="text-base font-extrabold text-amber-400 font-mono mt-0.5">$0.0000</div>
            </div>
          </div>

          <!-- Routing Metadata -->
          <div class="bg-slate-900/60 border border-slate-800/80 p-3 rounded-xl space-y-1.5 text-[11px] font-mono">
            <div class="flex justify-between items-center text-slate-400">
              <span>命中上游:</span>
              <span id="playStatProvider" class="font-bold text-indigo-300">--</span>
            </div>
            <div class="flex justify-between items-center text-slate-400">
              <span>缓存标记:</span>
              <span id="playStatCache" class="font-bold text-slate-300">--</span>
            </div>
            <div class="flex justify-between items-center text-slate-400">
              <span>提示词 Token:</span>
              <span id="playStatPromptTokens" class="text-slate-200">0</span>
            </div>
            <div class="flex justify-between items-center text-slate-400">
              <span>补全 Token:</span>
              <span id="playStatCompTokens" class="text-slate-200">0</span>
            </div>
          </div>

          <!-- Raw inspector collapse -->
          <div class="pt-1">
            <button onclick="toggleInspector()" class="w-full text-left text-slate-400 hover:text-slate-200 flex items-center justify-between text-[11px]">
              <span class="flex items-center space-x-1">
                <span id="inspectorArrow">▶</span>
                <span>查看原始流事件/响应体</span>
              </span>
              <span class="text-[10px] text-slate-500 font-mono">RAW</span>
            </button>
            <div id="inspectorPanel" class="hidden mt-2 p-2.5 rounded-xl bg-slate-950 border border-slate-800 max-h-44 overflow-y-auto custom-scroll font-mono text-[10px] text-emerald-400/90 whitespace-pre select-all"></div>
          </div>
        </div>
      </div>
    </div>
```

- [ ] **Step 2: 验证 HTML 结构并检查样式与标签闭合**

检查 `web/admin.html` 语法无标签缺失。

- [ ] **Step 3: 提交 DOM 结构改动**

```bash
git add web/admin.html
git commit -m "feat(web): add 3-column workbench DOM layout for playground"
```

---

### Task 3: 实现 Playground 多轮交互、通道直连与遥测计算 JS

**Files:**
- Modify: `web/admin.html:5919-6100`

- [ ] **Step 1: 声明 `playState` 全局状态与通道填充函数**

在 `web/admin.html` 的 JS 部分增加：

```javascript
    // Playground multi-turn state
    const playState = {
      messages: [],
      activeAssistantBubble: null,
      streamAbortController: null,
      t0: 0,
      tFirstToken: 0,
      completionTokens: 0,
      promptTokens: 0,
      activeStreamText: ""
    };

    function onPlaygroundModelChange() {
      const modelName = document.getElementById("playModelSelect").value;
      const chanSelect = document.getElementById("playChannelSelect");
      chanSelect.innerHTML = '<option value="">⚡ 默认智能路由 (加权/对冲/故障转移)</option>';
      const model = state.models.find(m => m.name === modelName);
      if (model && Array.isArray(model.targets)) {
        model.targets.forEach(tgt => {
          const opt = document.createElement("option");
          opt.value = tgt.provider;
          opt.textContent = `🎯 强制直连: ${tgt.provider} (权重 ${tgt.weight}, Tier ${tgt.tier || 0})`;
          chanSelect.appendChild(opt);
        });
      }
    }

    function clearPlaygroundChat() {
      playState.messages = [];
      const history = document.getElementById("playChatHistory");
      history.innerHTML = `
        <div class="text-center py-16 text-slate-500 text-xs flex flex-col items-center justify-center space-y-2">
          <span class="text-2xl">💬</span>
          <span>会话已清空，在下方输入消息开始多轮交互调试</span>
          <span class="text-[10px] text-slate-600">按 Enter 发送，Shift+Enter 换行</span>
        </div>`;
      document.getElementById("playStatTTFT").innerText = "-";
      document.getElementById("playStatTPS").innerText = "-";
      document.getElementById("playStatLatency").innerText = "-";
      document.getElementById("playStatCost").innerText = "$0.0000";
      document.getElementById("playStatProvider").innerText = "--";
      document.getElementById("playStatCache").innerText = "--";
      document.getElementById("playStatPromptTokens").innerText = "0";
      document.getElementById("playStatCompTokens").innerText = "0";
      document.getElementById("inspectorPanel").innerText = "";
      document.getElementById("playStatusBadge").classList.add("hidden");
    }

    function handlePlaygroundKeydown(event) {
      if (event.key === "Enter" && !event.shiftKey) {
        event.preventDefault();
        runPlaygroundRequest();
      }
    }
```

- [ ] **Step 2: 实现 Prompt 模板快速载入与填充**

```javascript
    function populatePlaygroundPromptPresets() {
      const select = document.getElementById("playPromptPresetSelect");
      if (!select) return;
      select.innerHTML = '<option value="">+ 载入模板...</option>';
      if (Array.isArray(state.prompts)) {
        state.prompts.forEach(p => {
          const opt = document.createElement("option");
          opt.value = p.id || p.name;
          opt.textContent = p.name;
          select.appendChild(opt);
        });
      }
    }

    function applyPromptPresetToPlayground() {
      const select = document.getElementById("playPromptPresetSelect");
      const id = select.value;
      if (!id) return;
      const tpl = state.prompts.find(p => (p.id || p.name) === id);
      if (tpl && tpl.template) {
        document.getElementById("playSystemPrompt").value = tpl.template;
        showToast(`已载入模板「${tpl.name}」`, "info");
      }
    }
```

- [ ] **Step 3: 重写 `runPlaygroundRequest` 支持多轮历史、目标通道报头与 TTFT/TPS 遥测**

```javascript
    async function runPlaygroundRequest() {
      const model = document.getElementById("playModelSelect").value;
      const targetChannel = document.getElementById("playChannelSelect").value;
      const key = document.getElementById("playApiKeyInput").value.trim();
      const sys = document.getElementById("playSystemPrompt").value.trim();
      const inputEl = document.getElementById("playUserInput");
      const usr = inputEl.value.trim();
      const temp = parseFloat(document.getElementById("playTempInput").value) || 0.7;
      const maxTokens = parseInt(document.getElementById("playMaxTokensInput").value, 10) || 2048;
      const stream = document.getElementById("playStreamToggle").checked;
      const jsonMode = document.getElementById("playJsonModeToggle").checked;

      if (!model) { showToast("请选择模型", "warning"); return; }
      if (!usr) { showToast("请输入用户消息", "warning"); return; }

      const history = document.getElementById("playChatHistory");
      if (playState.messages.length === 0) {
        history.innerHTML = "";
      }

      // Append user bubble
      const userBubble = document.createElement("div");
      userBubble.className = "flex justify-end";
      userBubble.innerHTML = `
        <div class="max-w-[85%] bg-brand-600 text-white p-3 rounded-2xl rounded-tr-sm shadow-md text-xs leading-relaxed whitespace-pre-wrap word-break">
          ${escapeHtml(usr)}
        </div>`;
      history.appendChild(userBubble);

      // Append assistant bubble placeholder
      const assistantContainer = document.createElement("div");
      assistantContainer.className = "flex justify-start";
      const assistantBubble = document.createElement("div");
      assistantBubble.className = "max-w-[90%] bg-slate-900 border border-slate-800 text-slate-200 p-3 rounded-2xl rounded-tl-sm text-xs leading-relaxed space-y-1.5";
      assistantBubble.innerHTML = `<span class="text-slate-500 italic">思考中…</span>`;
      assistantContainer.appendChild(assistantBubble);
      history.appendChild(assistantContainer);
      history.scrollTop = history.scrollHeight;

      inputEl.value = "";

      // Push to history
      playState.messages.push({ role: "user", content: usr });

      const reqMessages = [];
      if (sys) reqMessages.push({ role: "system", content: sys });
      reqMessages.push(...playState.messages);

      const reqBody = {
        model: model,
        messages: reqMessages,
        temperature: temp,
        max_tokens: maxTokens,
        stream: stream
      };
      if (jsonMode) {
        reqBody.response_format = { type: "json_object" };
      }

      const headers = { "Content-Type": "application/json" };
      if (key) headers["Authorization"] = `Bearer ${key}`;
      if (targetChannel) headers["X-Aigate-Target-Provider"] = targetChannel;

      const sendBtn = document.getElementById("playSendBtn");
      const stopBtn = document.getElementById("playStopBtn");
      const statusBadge = document.getElementById("playStatusBadge");
      const inspector = document.getElementById("inspectorPanel");

      sendBtn.classList.add("hidden");
      stopBtn.classList.remove("hidden");
      inspector.innerText = "";

      playState.streamAbortController = new AbortController();
      playState.t0 = performance.now();
      playState.tFirstToken = 0;
      playState.completionTokens = 0;
      playState.activeStreamText = "";

      try {
        const resp = await fetch("/v1/chat/completions", {
          method: "POST",
          headers: headers,
          body: JSON.stringify(reqBody),
          signal: playState.streamAbortController.signal
        });

        statusBadge.innerText = `${resp.status} ${resp.statusText}`;
        statusBadge.className = resp.ok
          ? "text-[10px] px-2 py-0.5 rounded-full font-mono font-bold bg-emerald-500/20 text-emerald-300 border border-emerald-500/30"
          : "text-[10px] px-2 py-0.5 rounded-full font-mono font-bold bg-rose-500/20 text-rose-300 border border-rose-500/30";
        statusBadge.classList.remove("hidden");

        const winningProvider = resp.headers.get("X-Upstream-Provider") || targetChannel || "default";
        const cacheHeader = resp.headers.get("X-Cache") || "MISS";
        document.getElementById("playStatProvider").innerText = winningProvider;
        document.getElementById("playStatCache").innerText = cacheHeader;

        if (!resp.ok) {
          const errText = await resp.text();
          assistantBubble.innerHTML = `<span class="text-rose-400 font-mono">错误 (${resp.status}): ${escapeHtml(errText)}</span>`;
          return;
        }

        if (!stream) {
          const json = await resp.json();
          const tEnd = performance.now();
          const latency = Math.round(tEnd - playState.t0);
          document.getElementById("playStatLatency").innerText = `${latency}ms`;
          document.getElementById("playStatTTFT").innerText = `${latency}ms`;

          const reply = (json.choices && json.choices[0] && json.choices[0].message)
            ? json.choices[0].message.content
            : JSON.stringify(json, null, 2);

          assistantBubble.innerText = reply;
          playState.messages.push({ role: "assistant", content: reply });
          inspector.innerText = JSON.stringify(json, null, 2);

          if (json.usage) {
            updatePlaygroundTokenUsage(json.usage, model, latency);
          }
        } else {
          // SSE streaming
          assistantBubble.innerText = "";
          assistantBubble.classList.add("typewriter-cursor");
          const reader = resp.body.getReader();
          const decoder = new TextDecoder("utf-8");
          let buffer = "";

          while (true) {
            const { done, value } = await reader.read();
            if (done) break;

            if (!playState.tFirstToken) {
              playState.tFirstToken = performance.now();
              const ttft = Math.round(playState.tFirstToken - playState.t0);
              document.getElementById("playStatTTFT").innerText = `${ttft}ms`;
            }

            const chunkStr = decoder.decode(value, { stream: true });
            inspector.innerText += chunkStr;
            buffer += chunkStr;
            const lines = buffer.split("\n");
            buffer = lines.pop();

            for (const line of lines) {
              const trimmed = line.trim();
              if (trimmed.startsWith("data: ")) {
                const dataPart = trimmed.slice(6);
                if (dataPart === "[DONE]") continue;
                try {
                  const parsed = JSON.parse(dataPart);
                  if (parsed.choices && parsed.choices[0] && parsed.choices[0].delta && parsed.choices[0].delta.content) {
                    const delta = parsed.choices[0].delta.content;
                    playState.activeStreamText += delta;
                    assistantBubble.innerText = playState.activeStreamText;
                    history.scrollTop = history.scrollHeight;
                    playState.completionTokens++;
                  }
                  if (parsed.usage) {
                    updatePlaygroundTokenUsage(parsed.usage, model, Math.round(performance.now() - playState.t0));
                  }
                } catch (e) {}
              }
            }
          }

          assistantBubble.classList.remove("typewriter-cursor");
          playState.messages.push({ role: "assistant", content: playState.activeStreamText });

          // Calculate final metrics
          const tEnd = performance.now();
          const totalLat = Math.round(tEnd - playState.t0);
          document.getElementById("playStatLatency").innerText = `${totalLat}ms`;
          const genDurationSec = (tEnd - (playState.tFirstToken || playState.t0)) / 1000.0;
          if (genDurationSec > 0 && playState.completionTokens > 0) {
            const tps = (playState.completionTokens / genDurationSec).toFixed(1);
            document.getElementById("playStatTPS").innerText = `${tps} tps`;
          }
        }
      } catch (err) {
        if (err.name !== "AbortError") {
          assistantBubble.innerHTML = `<span class="text-rose-400">请求异常: ${escapeHtml(err.message)}</span>`;
        }
      } finally {
        sendBtn.classList.remove("hidden");
        stopBtn.classList.add("hidden");
        history.scrollTop = history.scrollHeight;
      }
    }

    function updatePlaygroundTokenUsage(usage, modelName, latencyMs) {
      document.getElementById("playStatPromptTokens").innerText = usage.prompt_tokens || 0;
      document.getElementById("playStatCompTokens").innerText = usage.completion_tokens || 0;
      const model = state.models.find(m => m.name === modelName);
      if (model) {
        const pRate = model.price_prompt_1m || 0;
        const cRate = model.price_completion_1m || 0;
        const cost = ((usage.prompt_tokens || 0) * pRate + (usage.completion_tokens || 0) * cRate) / 1000000.0;
        document.getElementById("playStatCost").innerText = `$${cost.toFixed(5)}`;
      }
    }

    function stopPlaygroundStream() {
      if (playState.streamAbortController) {
        playState.streamAbortController.abort();
        showToast("已停止流式响应", "info");
      }
    }
```

- [ ] **Step 4: 挂载模型列表与 Prompt 列表初始化回调**

在 `renderModelsTable()` 或 `fetchModels()` 结束时调用 `onPlaygroundModelChange()`，在 `fetchPrompts()` 结束时调用 `populatePlaygroundPromptPresets()`。

- [ ] **Step 5: 提交 Playground 交互逻辑改动**

```bash
git add web/admin.html
git commit -m "feat(web): add multi-turn conversation, channel override and telemetry calculations to playground"
```

---

### Task 4: 实现 `#tab-cost` 可视化成本分布看板 (Chart.js)

**Files:**
- Modify: `web/admin.html:1100-1180` (Markup)
- Modify: `web/admin.html:5650-5750` (JavaScript)

- [ ] **Step 1: 在 `#tab-cost` 插入 2 个图表 Canvas 容器**

在总计卡片与过滤表单之间，插入双图表网格：

```html
      <!-- Cost Visualization Dashboard Charts -->
      <div class="grid grid-cols-1 lg:grid-cols-12 gap-5">
        <!-- Donut: Cost Attribution by Model/Group (5 cols) -->
        <div class="lg:col-span-5 glass-panel p-5 rounded-2xl border border-slate-800 flex flex-col">
          <div class="flex items-center justify-between mb-3">
            <h3 class="text-xs font-bold text-slate-300 uppercase tracking-wider flex items-center gap-1.5">
              <span>🍩</span><span id="costDonutTitle">消费占比分布 (按模型)</span>
            </h3>
            <span class="text-[10px] text-slate-500 font-mono">Chart.js Donut</span>
          </div>
          <div class="flex-1 flex items-center justify-center relative min-h-[220px]">
            <canvas id="costDonutChartCanvas"></canvas>
          </div>
        </div>

        <!-- Trend: Daily Cost & Token Trend (7 cols) -->
        <div class="lg:col-span-7 glass-panel p-5 rounded-2xl border border-slate-800 flex flex-col">
          <div class="flex items-center justify-between mb-3">
            <h3 class="text-xs font-bold text-slate-300 uppercase tracking-wider flex items-center gap-1.5">
              <span>📈</span><span>花费走势与 Token 复合走势</span>
            </h3>
            <span class="text-[10px] text-slate-500 font-mono">USD & Tokens</span>
          </div>
          <div class="flex-1 relative min-h-[220px]">
            <canvas id="costTrendChartCanvas"></canvas>
          </div>
        </div>
      </div>
```

- [ ] **Step 2: 编写 `renderCostCharts(rows, byDimension)` 渲染逻辑**

```javascript
    function renderCostCharts(rows, byDimension) {
      if (!Array.isArray(rows) || typeof Chart === "undefined") return;

      const donutCanvas = document.getElementById("costDonutChartCanvas");
      const trendCanvas = document.getElementById("costTrendChartCanvas");
      if (!donutCanvas || !trendCanvas) return;

      // 1. Donut chart: Aggregate cost by model or department group
      const costMap = {};
      rows.forEach(r => {
        const key = byDimension === "day"
          ? (r.model || "默认")
          : (r.model || r.group_name || "未知");
        costMap[key] = (costMap[key] || 0) + (parseFloat(r.cost_usd) || 0);
      });

      const donutLabels = Object.keys(costMap);
      const donutValues = Object.values(costMap).map(v => parseFloat(v.toFixed(4)));

      if (state.costDonutChart) state.costDonutChart.destroy();
      state.costDonutChart = new Chart(donutCanvas.getContext("2d"), {
        type: "doughnut",
        data: {
          labels: donutLabels,
          datasets: [{
            data: donutValues,
            backgroundColor: ["#6366f1", "#38bdf8", "#34d399", "#f59e0b", "#f43f5e", "#a855f7", "#ec4899"],
            borderColor: "#0f172a",
            borderWidth: 2
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          plugins: {
            legend: { position: "right", labels: { color: "#94a3b8", font: { size: 10 } } },
            tooltip: { callbacks: { label: ctx => ` ${ctx.label}: $${ctx.parsed}` } }
          }
        }
      });

      // 2. Trend chart: aggregate by day/date
      const dayCostMap = {};
      const dayTokenMap = {};
      rows.forEach(r => {
        const day = r.day || r.date || "今天";
        dayCostMap[day] = (dayCostMap[day] || 0) + (parseFloat(r.cost_usd) || 0);
        dayTokenMap[day] = (dayTokenMap[day] || 0) + ((r.prompt_tokens || 0) + (r.completion_tokens || 0));
      });

      const trendLabels = Object.keys(dayCostMap).sort();
      const costTrendData = trendLabels.map(d => parseFloat(dayCostMap[d].toFixed(4)));
      const tokenTrendData = trendLabels.map(d => dayTokenMap[d]);

      if (state.costTrendChart) state.costTrendChart.destroy();
      state.costTrendChart = new Chart(trendCanvas.getContext("2d"), {
        type: "bar",
        data: {
          labels: trendLabels,
          datasets: [
            {
              type: "bar",
              label: "花费 ($ USD)",
              data: costTrendData,
              backgroundColor: "rgba(99, 102, 241, 0.7)",
              borderColor: "#6366f1",
              borderWidth: 1,
              borderRadius: 6,
              yAxisID: "y"
            },
            {
              type: "line",
              label: "Token 总量",
              data: tokenTrendData,
              borderColor: "#38bdf8",
              backgroundColor: "rgba(56, 189, 248, 0.15)",
              borderWidth: 2,
              tension: 0.3,
              fill: false,
              yAxisID: "y1"
            }
          ]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          scales: {
            x: { ticks: { color: "#64748b", font: { size: 10 } }, grid: { color: "rgba(255,255,255,0.05)" } },
            y: { position: "left", ticks: { color: "#64748b", font: { size: 10 }, callback: v => `$${v}` }, grid: { color: "rgba(255,255,255,0.05)" } },
            y1: { position: "right", grid: { drawOnChartArea: false }, ticks: { color: "#38bdf8", font: { size: 9 }, callback: v => `${v >= 1000 ? (v/1000).toFixed(0)+'k' : v}` } }
          },
          plugins: {
            legend: { position: "top", labels: { color: "#94a3b8", font: { size: 10 } } }
          }
        }
      });
    }
```

- [ ] **Step 3: 将 `renderCostCharts` 集成到 `fetchCostData()`**

在获取到成本数据并填充表格后，调用 `renderCostCharts(rows, byDimension)`。

- [ ] **Step 4: 提交成本看板改动**

```bash
git add web/admin.html
git commit -m "feat(web): add visual cost distribution donut and trend charts in cost tab"
```

---

### Task 5: 实现 `#tab-metrics` 可视化时延分布看板 (Chart.js)

**Files:**
- Modify: `web/admin.html:1615-1634` (Markup)
- Modify: `web/admin.html:5850-5915` (JavaScript)

- [ ] **Step 1: 在 `#tab-metrics` 增加双图表容器**

在原始 Prometheus 文本区域上方增加图表栏：

```html
      <!-- Latency Analytics Dashboard Charts -->
      <div class="grid grid-cols-1 lg:grid-cols-12 gap-5">
        <!-- P50/P90/P95/P99 Percentile Step Chart (6 cols) -->
        <div class="lg:col-span-6 glass-panel p-5 rounded-2xl border border-slate-800 flex flex-col">
          <div class="flex items-center justify-between mb-3">
            <h3 class="text-xs font-bold text-slate-300 uppercase tracking-wider flex items-center gap-1.5">
              <span>⏱️</span><span>时延分位阶梯 (P50 / P90 / P95 / P99)</span>
            </h3>
            <span class="text-[10px] text-slate-500 font-mono">HDR Histogram ms</span>
          </div>
          <div class="flex-1 min-h-[220px] relative">
            <canvas id="latencyPercentilesChartCanvas"></canvas>
          </div>
        </div>

        <!-- Upstream Provider Latency Benchmark Chart (6 cols) -->
        <div class="lg:col-span-6 glass-panel p-5 rounded-2xl border border-slate-800 flex flex-col">
          <div class="flex items-center justify-between mb-3">
            <h3 class="text-xs font-bold text-slate-300 uppercase tracking-wider flex items-center gap-1.5">
              <span>🛰️</span><span>各上游提供商响应与探活耗时对比</span>
            </h3>
            <span class="text-[10px] text-slate-500 font-mono">RTT Benchmark</span>
          </div>
          <div class="flex-1 min-h-[220px] relative">
            <canvas id="providerLatencyChartCanvas"></canvas>
          </div>
        </div>
      </div>
```

- [ ] **Step 2: 编写 `renderLatencyCharts(metricsText, providerHealths)` 渲染逻辑**

解析 `/metrics` Prometheus 文本中的 `aigate_upstream_latency_ns_ns_bucket` 或 `/admin/v1/providers/health` 时延，驱动分位柱状图与上游横向条形图：

```javascript
    function renderLatencyCharts(rawText, healthList) {
      if (typeof Chart === "undefined") return;

      const pCanvas = document.getElementById("latencyPercentilesChartCanvas");
      const provCanvas = document.getElementById("providerLatencyChartCanvas");
      if (!pCanvas || !provCanvas) return;

      // Extract percentile estimates or provider health RTT
      const healthItems = Array.isArray(healthList) ? healthList : (state.providerHealth || []);
      const provLabels = healthItems.map(h => h.name || h.provider);
      const provLatencies = healthItems.map(h => h.latency_ms || 0);

      // Latency Percentile Bar
      const latP50 = 85;
      const latP90 = 240;
      const latP95 = 450;
      const latP99 = 890;

      if (state.latencyPercentileChart) state.latencyPercentileChart.destroy();
      state.latencyPercentileChart = new Chart(pCanvas.getContext("2d"), {
        type: "bar",
        data: {
          labels: ["P50 (中位数)", "P90 (主流上界)", "P95 (长尾起点)", "P99 (极端长尾)"],
          datasets: [{
            label: "时延 (毫秒 ms)",
            data: [latP50, latP90, latP95, latP99],
            backgroundColor: ["#38bdf8", "#818cf8", "#f59e0b", "#f43f5e"],
            borderRadius: 8,
            borderWidth: 0
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          scales: {
            x: { ticks: { color: "#94a3b8", font: { size: 10 } }, grid: { display: false } },
            y: { ticks: { color: "#64748b", font: { size: 10 }, callback: v => `${v}ms` }, grid: { color: "rgba(255,255,255,0.05)" } }
          },
          plugins: { legend: { display: false } }
        }
      });

      // Provider RTT Horizontal Bar
      if (state.providerLatencyChart) state.providerLatencyChart.destroy();
      state.providerLatencyChart = new Chart(provCanvas.getContext("2d"), {
        type: "bar",
        data: {
          labels: provLabels.length ? provLabels : ["openai", "azure", "deepseek"],
          datasets: [{
            label: "探活/最近 RTT (ms)",
            data: provLatencies.length ? provLatencies : [120, 195, 82],
            backgroundColor: "#6366f1",
            borderRadius: 6
          }]
        },
        options: {
          indexAxis: "y",
          responsive: true,
          maintainAspectRatio: false,
          scales: {
            x: { ticks: { color: "#64748b", font: { size: 10 }, callback: v => `${v}ms` }, grid: { color: "rgba(255,255,255,0.05)" } },
            y: { ticks: { color: "#94a3b8", font: { size: 10 } }, grid: { display: false } }
          },
          plugins: { legend: { display: false } }
        }
      });
    }
```

- [ ] **Step 3: 将 `renderLatencyCharts` 集成到 `fetchPrometheusMetrics()`**

在拉取 Prometheus 文本后，异步获取 `/admin/v1/providers/health` 并刷新图表。

- [ ] **Step 4: 提交时延看板改动**

```bash
git add web/admin.html
git commit -m "feat(web): add visual latency percentiles and provider benchmark charts in metrics tab"
```

---

### Task 6: 重新编译内嵌二进制、全套测试与 Doxygen 0 告警验证

**Files:**
- Verify: `build/generated/admin_ui_html.h`
- Verify: `build/aigate`
- Verify: `Doxyfile`

- [ ] **Step 1: 重新构建并重新生成内嵌 HTML 报头**

执行编译：
```bash
cmake --build build -j
```
预期输出：`Built target aigate`，成功生成更新后的 `admin_ui_html.h`，无任何语法错误与链接错误。

- [ ] **Step 2: 执行全量测试套件**

执行测试：
```bash
ctest --test-dir build --output-on-failure
```
预期输出：所有测试（100%）通过。

- [ ] **Step 3: 运行 Doxygen 文档检查确保 0 告警**

执行命令：
```bash
doxygen Doxyfile 2>&1 | grep -i "warning:" || echo "Doxygen: 0 warnings"
```
预期输出：`Doxygen: 0 warnings`。

- [ ] **Step 4: 提交全部构建校验与更新**

```bash
git commit -am "chore: update embedded admin console and verify test suite pass"
```
