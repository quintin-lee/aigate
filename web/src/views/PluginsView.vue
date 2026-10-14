<template>
  <div id="tab-plugins" class="space-y-4">
    <!-- Header Controls -->
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">嵌入式 Lua 脚本插件引擎 (Lua Plugin Workbench)</h2>
        <span class="text-xs px-2 py-0.5 rounded-full bg-slate-800 text-slate-400 border border-slate-700">
          共 {{ plugins.length }} 个插件
        </span>
      </div>
      <div class="flex items-center space-x-2">
        <n-button size="small" secondary @click="loadPlugins">刷新列表</n-button>
        <n-button size="small" type="primary" @click="createNewPlugin">+ 编写新插件</n-button>
      </div>
    </div>

    <!-- Plugins Table -->
    <div class="glass-panel rounded-xl overflow-hidden">
      <n-data-table :columns="columns" :data="plugins" :loading="loading" />
    </div>

    <!-- Monaco Workbench Drawer / Full Modal -->
    <n-modal
      v-model:show="workbenchVisible"
      preset="card"
      :title="editingName ? `🛠️ 编辑 Lua 插件: ${editingName}` : '🛠️ 编写新 Lua 脚本插件'"
      class="max-w-6xl w-[96vw] h-[90vh] bg-slate-900 border border-slate-800 text-slate-100 flex flex-col"
    >
      <div class="flex flex-col h-full space-y-3">
        <!-- Metadata bar with Template Selector -->
        <div class="grid grid-cols-1 sm:grid-cols-5 gap-3 bg-slate-950 p-3 rounded-lg border border-slate-800 text-xs">
          <div class="sm:col-span-1">
            <label class="text-[11px] text-cyan-400 font-medium block mb-1">📐 预置场景模板库</label>
            <n-select
              v-model:value="selectedTemplateKey"
              :options="templateSelectOptions"
              size="small"
              placeholder="选择预置模板..."
              @update:value="onTemplateSelect"
            />
          </div>
          <div>
            <label class="text-[11px] text-slate-400 block mb-1">插件唯一标识 (Name)</label>
            <n-input v-model:value="currentPlugin.name" :disabled="!!editingName" size="small" placeholder="例如: rate_limit_guard" />
          </div>
          <div>
            <label class="text-[11px] text-slate-400 block mb-1">拦截生效阶段 (Phase)</label>
            <n-select v-model:value="currentPlugin.phase" :options="phaseOptions" size="small" />
          </div>
          <div>
            <label class="text-[11px] text-slate-400 block mb-1">执行优先级 (Priority)</label>
            <n-input-number v-model:value="currentPlugin.priority" :min="1" :max="1000" size="small" />
          </div>
          <div>
            <label class="text-[11px] text-slate-400 block mb-1">版本号 (Version)</label>
            <n-input v-model:value="currentPlugin.version" size="small" placeholder="1.0.0" />
          </div>
        </div>

        <!-- Editor Toolbar & Status -->
        <div id="ideStatusBar" class="flex items-center justify-between text-xs px-1">
          <div class="flex items-center space-x-2">
            <span class="font-mono text-slate-400">Lua 5.4 脚本源码</span>
            <span id="ideLintStatus">
              <n-tag v-if="syntaxStatus === 'valid'" size="tiny" type="success" :bordered="false">语法校验通过</n-tag>
              <n-tag v-else-if="syntaxStatus === 'error'" size="tiny" type="error" :bordered="false">{{ syntaxError }}</n-tag>
              <n-tag v-else size="tiny" type="warning" :bordered="false">正在语法检查...</n-tag>
            </span>
            <span class="text-[11px] text-slate-500 hidden sm:inline">提示: 按 Tab 可展开 Snippets 代码片段，光标悬浮可查看 API 文档</span>
            <span id="monacoFallbackBadge" class="hidden text-[10px] px-2 py-0.5 rounded-full bg-amber-500/10 text-amber-400 font-mono">离线模式</span>
          </div>

          <div class="flex items-center space-x-2">
            <n-button v-if="editingName" size="tiny" secondary @click="toggleDiffMode">
              {{ showDiff ? '切换普通编辑' : '查看代码变更 (Diff)' }}
            </n-button>
            <n-button size="tiny" type="info" secondary @click="openSandbox">
              🧪 打开沙箱测试器 (Test)
            </n-button>
          </div>
        </div>

        <!-- Monaco Editor Container -->
        <div id="monacoEditorContainer" class="flex-1 w-full rounded-lg border border-slate-800 bg-slate-950 overflow-hidden relative">
          <monaco-editor
            ref="monacoRef"
            v-model="currentPlugin.script"
            :diff-mode="showDiff"
            :original-value="originalScript"
            @change="onScriptChange"
            @save="savePlugin"
            @run-test="openSandbox"
          />
        </div>

        <!-- Diff & Logs Container hooks for test assertions -->
        <div id="monacoDiffContainer" v-show="showDiff" class="hidden"></div>
      </div>

      <template #footer>
        <div class="flex justify-between items-center w-full">
          <span class="text-xs text-slate-500">快捷键: <kbd class="px-1 py-0.5 bg-slate-800 rounded text-slate-300">Ctrl+S</kbd> 保存 | <kbd class="px-1 py-0.5 bg-slate-800 rounded text-slate-300">Ctrl+Enter</kbd> 运行沙箱测试</span>
          <div class="flex space-x-2">
            <n-button @click="workbenchVisible = false">取消</n-button>
            <n-button type="primary" :loading="saving" @click="savePlugin">保存并热装载</n-button>
          </div>
        </div>
      </template>
    </n-modal>

    <!-- Full-Featured Dual-Column Sandbox Test Modal -->
    <n-modal
      v-model:show="sandboxVisible"
      preset="card"
      title="🧪 Lua 插件沙箱隔离测试与控制台 (Sandbox Runner & Live Console)"
      class="max-w-6xl w-[96vw] bg-slate-900 border border-slate-800 text-slate-100"
    >
      <div class="grid grid-cols-1 lg:grid-cols-12 gap-4">
        <!-- Left Column: Mock Inspector & Configuration (7 cols) -->
        <div class="lg:col-span-7 space-y-3 bg-slate-950/70 p-3.5 rounded-lg border border-slate-800">
          <div class="flex items-center justify-between pb-2 border-b border-slate-800">
            <div class="flex items-center space-x-2">
              <span class="text-xs font-semibold text-cyan-400">Mock 参数构造器</span>
              <n-radio-group v-model:value="sandboxForm.phase" size="small">
                <n-radio-button value="request">入站请求 (on_request)</n-radio-button>
                <n-radio-button value="response">出站响应 (on_response)</n-radio-button>
              </n-radio-group>
            </div>
            <n-button size="tiny" secondary type="warning" @click="applyTemplatePreset">
              填入当前模板用例
            </n-button>
          </div>

          <!-- Request Phase Inputs -->
          <template v-if="sandboxForm.phase === 'request'">
            <div class="grid grid-cols-2 sm:grid-cols-4 gap-2">
              <div>
                <label class="text-[11px] text-slate-400 block mb-0.5">HTTP 方法</label>
                <n-select v-model:value="sandboxForm.method" :options="methodOptions" size="small" />
              </div>
              <div>
                <label class="text-[11px] text-slate-400 block mb-0.5">请求路径</label>
                <n-input v-model:value="sandboxForm.path" size="small" placeholder="/v1/chat/completions" />
              </div>
              <div>
                <label class="text-[11px] text-slate-400 block mb-0.5">客户端 IP</label>
                <n-input v-model:value="sandboxForm.client_ip" size="small" placeholder="127.0.0.1" />
              </div>
              <div>
                <label class="text-[11px] text-slate-400 block mb-0.5">目标模型 (Model)</label>
                <n-input v-model:value="sandboxForm.model" size="small" placeholder="gpt-4o" />
              </div>
            </div>

            <!-- Dynamic Headers Table -->
            <div class="space-y-1">
              <div class="flex items-center justify-between">
                <label class="text-[11px] text-slate-400">请求头 (Request Headers)</label>
                <n-button size="tiny" secondary @click="addHeaderRow(sandboxForm.requestHeaders)">+ 添加 Header</n-button>
              </div>
              <div class="max-h-32 overflow-y-auto space-y-1 pr-1">
                <div v-for="(h, idx) in sandboxForm.requestHeaders" :key="idx" class="flex items-center space-x-1.5">
                  <n-input v-model:value="h.key" size="tiny" placeholder="Header 键名 (如 Authorization)" class="w-1/2" />
                  <n-input v-model:value="h.value" size="tiny" placeholder="Header 键值 (如 Bearer token)" class="w-1/2" />
                  <n-button size="tiny" quaternary type="error" @click="removeHeaderRow(sandboxForm.requestHeaders, idx)">✕</n-button>
                </div>
              </div>
            </div>

            <!-- Request Body -->
            <div>
              <div class="flex items-center justify-between mb-1">
                <label class="text-[11px] text-slate-400">请求体 (Request Body JSON)</label>
                <n-button size="tiny" secondary @click="formatBodyJson">格式化 JSON</n-button>
              </div>
              <n-input
                v-model:value="sandboxForm.body"
                type="textarea"
                rows="4"
                class="font-mono text-xs"
                placeholder='{"messages": [{"role": "user", "content": "hello"}]}'
              />
            </div>
          </template>

          <!-- Response Phase Inputs -->
          <template v-else>
            <div class="grid grid-cols-2 gap-2">
              <div>
                <label class="text-[11px] text-slate-400 block mb-0.5">上游 HTTP 状态码</label>
                <n-input-number v-model:value="sandboxForm.status" size="small" :min="100" :max="599" />
              </div>
              <div>
                <label class="text-[11px] text-slate-400 block mb-0.5">响应路径</label>
                <n-input v-model:value="sandboxForm.path" size="small" placeholder="/v1/chat/completions" />
              </div>
            </div>

            <!-- Dynamic Response Headers Table -->
            <div class="space-y-1">
              <div class="flex items-center justify-between">
                <label class="text-[11px] text-slate-400">响应头 (Response Headers)</label>
                <n-button size="tiny" secondary @click="addHeaderRow(sandboxForm.responseHeaders)">+ 添加 Header</n-button>
              </div>
              <div class="max-h-32 overflow-y-auto space-y-1 pr-1">
                <div v-for="(h, idx) in sandboxForm.responseHeaders" :key="idx" class="flex items-center space-x-1.5">
                  <n-input v-model:value="h.key" size="tiny" placeholder="Header 键名" class="w-1/2" />
                  <n-input v-model:value="h.value" size="tiny" placeholder="Header 键值" class="w-1/2" />
                  <n-button size="tiny" quaternary type="error" @click="removeHeaderRow(sandboxForm.responseHeaders, idx)">✕</n-button>
                </div>
              </div>
            </div>

            <!-- Response Body -->
            <div>
              <div class="flex items-center justify-between mb-1">
                <label class="text-[11px] text-slate-400">响应体 (Upstream Response Body)</label>
                <n-button size="tiny" secondary @click="formatBodyJson">格式化 JSON</n-button>
              </div>
              <n-input
                v-model:value="sandboxForm.body"
                type="textarea"
                rows="5"
                class="font-mono text-xs"
                placeholder='{"choices": [{"message": {"role": "assistant", "content": "..."}}]}'
              />
            </div>
          </template>

          <div class="flex justify-end pt-1">
            <n-button type="primary" size="small" :loading="runningTest" @click="runSandboxTest">
              ▶ 运行沙箱测试 (Ctrl+Enter)
            </n-button>
          </div>
        </div>

        <!-- Right Column: Results & Live Log Console (5 cols) -->
        <div class="lg:col-span-5 flex flex-col space-y-3">
          <!-- Execution Summary Card -->
          <div class="p-3 bg-slate-950 rounded-lg border border-slate-800 space-y-2">
            <div class="flex items-center justify-between">
              <span class="text-xs font-semibold text-slate-300">执行结果与指标</span>
              <span v-if="testResult" class="font-mono text-xs text-cyan-400">
                纯执行耗时: {{ testResult.duration_us || 0 }} μs
              </span>
            </div>

            <div v-if="!testResult" class="text-xs text-slate-500 py-3 text-center">
              点击「运行沙箱测试」查看执行结果、改写输出与运行日志
            </div>

            <div v-else class="space-y-1.5 text-xs font-mono">
              <div class="flex items-center space-x-2">
                <span>状态:</span>
                <n-tag v-if="testResult.success" size="tiny" type="success" :bordered="false">✓ 执行通过 (OK)</n-tag>
                <n-tag v-else-if="testResult.aborted" size="tiny" type="warning" :bordered="false">⛔ 主动熔断 (Aborted)</n-tag>
                <n-tag v-else size="tiny" type="error" :bordered="false">❌ 异常 (Error)</n-tag>
              </div>

              <div v-if="testResult.abort_status" class="p-2 bg-amber-500/10 border border-amber-500/20 rounded text-amber-300">
                <div><strong>熔断 HTTP 状态码:</strong> {{ testResult.abort_status }}</div>
                <div v-if="testResult.abort_reason"><strong>熔断响应说明:</strong> {{ testResult.abort_reason }}</div>
              </div>

              <div v-if="testResult.error" class="p-2 bg-rose-500/10 border border-rose-500/20 rounded text-rose-400">
                <strong>执行错误:</strong> {{ testResult.error }}
              </div>

              <!-- Rewritten Model -->
              <div v-if="testResult.request && testResult.request.model && testResult.request.model !== sandboxForm.model" class="p-1.5 bg-cyan-500/10 border border-cyan-500/20 rounded text-cyan-300">
                <strong>动态路由改写模型:</strong> {{ sandboxForm.model }} ➔ {{ testResult.request.model }}
              </div>

              <!-- Rewritten Status -->
              <div v-if="testResult.response && testResult.response.status && testResult.response.status !== sandboxForm.status" class="p-1.5 bg-cyan-500/10 border border-cyan-500/20 rounded text-cyan-300">
                <strong>改写状态码:</strong> {{ sandboxForm.status }} ➔ {{ testResult.response.status }}
              </div>

              <!-- Rewritten Body preview -->
              <div v-if="testResultModifiedBody" class="space-y-0.5">
                <span class="text-slate-400 text-[11px]">改写后 Body 输出:</span>
                <pre class="p-2 bg-slate-900 rounded max-h-24 overflow-auto text-[11px] text-slate-300">{{ testResultModifiedBody }}</pre>
              </div>
            </div>
          </div>

          <!-- Live Log Console -->
          <div class="flex-1 flex flex-col bg-slate-950 rounded-lg border border-slate-800 overflow-hidden">
            <div class="flex items-center justify-between px-3 py-1.5 bg-slate-900/90 border-b border-slate-800 text-xs">
              <span class="font-semibold text-slate-300 flex items-center space-x-1">
                <span>📟 控制台运行日志 (Live Log Console)</span>
              </span>
              <div class="flex items-center space-x-1.5">
                <n-input v-model:value="logFilter" size="tiny" placeholder="过滤日志..." class="w-24 text-[11px]" clearable />
                <n-button size="tiny" secondary @click="copyLogs">复制</n-button>
                <n-button size="tiny" secondary @click="clearLogs">清空</n-button>
              </div>
            </div>

            <!-- DOM hook #ideConsoleLogs for automated tests & visual console -->
            <div
              id="ideConsoleLogs"
              class="flex-1 p-2.5 font-mono text-[11px] overflow-y-auto space-y-1 min-h-[160px] max-h-[260px] bg-[#020617]"
            >
              <div v-if="filteredLogs.length === 0" class="text-slate-600 text-center py-6">
                暂无日志输出 (调用 aigate.log_* 可在沙箱实时打印)
              </div>
              <div
                v-for="(log, idx) in filteredLogs"
                :key="idx"
                :class="getLogLineClass(log)"
                class="leading-tight break-all"
              >
                {{ log }}
              </div>
            </div>
          </div>
        </div>
      </div>
    </n-modal>
  </div>
</template>

<script setup>
import { ref, h, onMounted, reactive, computed } from 'vue';
import {
  NButton,
  NDataTable,
  NModal,
  NInput,
  NInputNumber,
  NSelect,
  NSwitch,
  NTag,
  NPopconfirm,
  NRadioGroup,
  NRadioButton,
  useMessage,
} from 'naive-ui';
import MonacoEditor from '@/components/MonacoEditor.vue';
import { api } from '@/api/client';

const message = useMessage();
const loading = ref(false);
const plugins = ref([]);
const workbenchVisible = ref(false);
const editingName = ref(null);
const saving = ref(false);

const monacoRef = ref(null);
const showDiff = ref(false);
const originalScript = ref('');

const syntaxStatus = ref('valid');
const syntaxError = ref('');
let debounceTimer = null;

const sandboxVisible = ref(false);
const runningTest = ref(false);
const testResult = ref(null);
const capturedLogs = ref([]);
const logFilter = ref('');

// 5 Production Templates
const TEMPLATES = [
  {
    key: 'token_auth',
    label: '🛡️ Token 鉴权与黑白名单拦截 (on_request)',
    defaultName: 'auth_guard',
    phase: 'on_request',
    priority: 10,
    description: '入口安全校验 Authorization Bearer 格式与黑名单 Token 拦截',
    script: `-- Token 鉴权与黑白名单拦截插件
local aigate = require("aigate")

function on_request(req)
    local auth_header = req:get_header("authorization")
    if not auth_header or not string.match(auth_header, "^Bearer%s+") then
        aigate.log_warn("拦截未授权请求: 缺少有效 Authorization 头")
        aigate.abort_with(401, '{"error":{"code":"unauthorized","message":"Missing or invalid Bearer token"}}')
        return
    end

    local token = string.gsub(auth_header, "^Bearer%s+", "")
    -- 模拟黑名单或受限 token 阻断
    if token == "banned-test-token" then
        aigate.log_warn("拦截黑名单 Token: " .. token)
        aigate.abort_with(403, '{"error":{"code":"forbidden","message":"Your token has been revoked"}}')
        return
    end

    aigate.log_info("Token 鉴权校验通过")
end
`,
    mockPreset: {
      phase: 'request',
      method: 'POST',
      path: '/v1/chat/completions',
      client_ip: '192.168.1.100',
      model: 'gpt-4o',
      headers: [
        { key: 'Authorization', value: 'Bearer sk-enterprise-token' },
        { key: 'Content-Type', value: 'application/json' },
      ],
      body: '{"messages":[{"role":"user","content":"hello security"}]}',
      status: 200,
    },
  },
  {
    key: 'model_router',
    label: '🔀 基于用户的模型动态路由改写 (on_request)',
    defaultName: 'model_dynamic_router',
    phase: 'on_request',
    priority: 50,
    description: '识别 VIP 用户等级，将 gpt-4o 动态改写路由至 deepseek-chat',
    script: `-- 基于用户的模型动态路由改写插件
local aigate = require("aigate")

function on_request(req)
    local current_model = req:get_model()
    local user_tier = req:get_header("x-user-tier")

    aigate.log_info("收到请求，原始模型: " .. (current_model or "nil") .. ", 用户等级: " .. (user_tier or "free"))

    -- 若为 vip 客户且请求为 gpt-4o，无缝路由至定制高性能集群 deepseek-chat
    if user_tier == "vip" and current_model == "gpt-4o" then
        req:set_model("deepseek-chat")
        req:set_header("x-gateway-routed", "deepseek-cluster")
        aigate.log_info("VIP 用户模型动态路由重写: gpt-4o -> deepseek-chat")
    end
end
`,
    mockPreset: {
      phase: 'request',
      method: 'POST',
      path: '/v1/chat/completions',
      client_ip: '10.0.0.8',
      model: 'gpt-4o',
      headers: [
        { key: 'X-User-Tier', value: 'vip' },
        { key: 'Content-Type', value: 'application/json' },
      ],
      body: '{"model":"gpt-4o","messages":[{"role":"user","content":"Hello AI"}]}',
      status: 200,
    },
  },
  {
    key: 'trace_logger',
    label: '📡 全链路追踪日志与 Header 注入 (on_request)',
    defaultName: 'trace_logger',
    phase: 'on_request',
    priority: 20,
    description: '自动生成或透传分布式 Trace ID 并注入入站请求头',
    script: `-- 全链路追踪日志与 Header 注入
local aigate = require("aigate")

function on_request(req)
    local trace_id = req:get_header("x-request-id")
    if not trace_id or trace_id == "" then
        trace_id = "trace-" .. tostring(os.time()) .. "-" .. tostring(math.random(1000, 9999))
        req:set_header("x-request-id", trace_id)
    end

    local client_ip = req:get_client_ip() or "unknown"
    local model = req:get_model() or "default"
    aigate.log_info(string.format("[%s] 收到请求: IP=%s, Model=%s", trace_id, client_ip, model))
end
`,
    mockPreset: {
      phase: 'request',
      method: 'POST',
      path: '/v1/chat/completions',
      client_ip: '172.16.5.20',
      model: 'gpt-4o',
      headers: [
        { key: 'X-Request-ID', value: 'trace-demo-123456' },
      ],
      body: '{"messages":[{"role":"user","content":"trace test"}]}',
      status: 200,
    },
  },
  {
    key: 'resp_masking',
    label: '🎭 响应内容敏感信息脱敏与水印 (on_response)',
    defaultName: 'privacy_masking',
    phase: 'on_response',
    priority: 100,
    description: '出站阶段正则脱敏中国大陆 11 位手机号并注入溯源水印',
    script: `-- 响应内容敏感信息脱敏与水印注入
local aigate = require("aigate")

function on_response(resp)
    local body = resp:get_body()
    if not body or body == "" then return end

    -- 正则脱敏 11 位手机号码为 138****1234
    local masked_body, count = string.gsub(body, "(1[3-9]%d)(%d%d%d%d)(%d%d%d%d)", "%1****%3")
    if count > 0 then
        aigate.log_info("响应体脱敏完成，共掩码敏感手机号: " .. tostring(count) .. " 处")
        resp:set_body(masked_body)
        resp:set_header("x-privacy-masked", "true")
    end

    -- 注入出站安全水印标
    resp:set_header("x-gateway-watermark", "verified-by-aigate")
end
`,
    mockPreset: {
      phase: 'response',
      method: 'POST',
      path: '/v1/chat/completions',
      client_ip: '127.0.0.1',
      model: 'gpt-4o',
      headers: [
        { key: 'Content-Type', value: 'application/json' },
      ],
      body: '{"choices":[{"message":{"role":"assistant","content":"请联系客户经理电话：13812345678 或 13987654321 获取支持。"}}]}',
      status: 200,
    },
  },
  {
    key: 'rate_limit_stub',
    label: '⚡ 访问频控检查与 429 短路熔断 (on_request)',
    defaultName: 'rate_limit_stub',
    phase: 'on_request',
    priority: 5,
    description: '检测异常来源 IP 网段并在微秒内快速熔断返回 429',
    script: `-- 访问频控检查与 429 短路熔断插件
local aigate = require("aigate")

function on_request(req)
    local client_ip = req:get_client_ip()
    local banned_subnets = { "10.99.", "192.0.2." }

    for _, prefix in ipairs(banned_subnets) do
        if client_ip and string.sub(client_ip, 1, #prefix) == prefix then
            aigate.log_warn("触发频控短路熔断: 来源 IP 受限 " .. client_ip)
            aigate.abort_with(429, '{"error":{"code":"rate_limited","message":"Too many requests from your IP, please retry later"}}')
            return
        end
    end
end
`,
    mockPreset: {
      phase: 'request',
      method: 'POST',
      path: '/v1/chat/completions',
      client_ip: '10.99.3.42',
      model: 'gpt-4o',
      headers: [
        { key: 'Content-Type', value: 'application/json' },
      ],
      body: '{"messages":[{"role":"user","content":"rapid request"}]}',
      status: 200,
    },
  },
];

const selectedTemplateKey = ref(null);
const templateSelectOptions = TEMPLATES.map((t) => ({
  label: t.label,
  value: t.key,
}));

const methodOptions = [
  { label: 'POST', value: 'POST' },
  { label: 'GET', value: 'GET' },
  { label: 'PUT', value: 'PUT' },
  { label: 'DELETE', value: 'DELETE' },
];

const sandboxForm = reactive({
  phase: 'request',
  method: 'POST',
  path: '/v1/chat/completions',
  client_ip: '127.0.0.1',
  model: 'gpt-4o',
  status: 200,
  requestHeaders: [
    { key: 'Authorization', value: 'Bearer sk-dev-test' },
    { key: 'Content-Type', value: 'application/json' },
  ],
  responseHeaders: [
    { key: 'Content-Type', value: 'application/json' },
  ],
  body: '{"messages":[{"role":"user","content":"hello"}]}',
});

const currentPlugin = reactive({
  name: '',
  version: '1.0.0',
  phase: 'on_request',
  priority: 100,
  enabled: true,
  script: '',
});

const phaseOptions = [
  { label: '请求阶段 (on_request) - 认证 / 改写 / 熔断', value: 'on_request' },
  { label: '响应阶段 (on_response) - 脱敏 / 审计 / 水印', value: 'on_response' },
];

const defaultScriptTemplate = TEMPLATES[0].script;

const columns = [
  { title: '插件名称', key: 'name', width: 160 },
  { title: '版本', key: 'version', width: 90 },
  {
    title: '生效阶段',
    key: 'phase',
    width: 140,
    render(row) {
      const isReq = row.phase === 'on_request';
      return h(
        NTag,
        { size: 'small', type: isReq ? 'info' : 'warning', bordered: false },
        { default: () => row.phase }
      );
    },
  },
  { title: '优先级', key: 'priority', width: 90 },
  {
    title: '运行统计 (调用 / 错误 / 耗时)',
    key: 'stats',
    render(row) {
      const calls = row.execution_count || 0;
      const errors = row.error_count || 0;
      const lat = row.avg_latency_us || 0;
      return `${calls.toLocaleString()} 次 / ${errors} 错 / ${lat} μs`;
    },
  },
  {
    title: '启用状态',
    key: 'enabled',
    width: 100,
    render(row) {
      return h(NSwitch, {
        value: row.enabled,
        onUpdateValue: (val) => togglePluginStatus(row, val),
      });
    },
  },
  {
    title: '操作',
    key: 'actions',
    width: 140,
    render(row) {
      return h('div', { class: 'flex space-x-2' }, [
        h(
          NButton,
          {
            size: 'tiny',
            secondary: true,
            onClick: () => editPlugin(row),
          },
          { default: () => 'IDE 源码' }
        ),
        h(
          NPopconfirm,
          { onPositiveClick: () => deletePlugin(row.name) },
          {
            trigger: () => h(NButton, { size: 'tiny', type: 'error', secondary: true }, { default: () => '删除' }),
            default: () => `确定卸载删除插件 ${row.name} 吗？`,
          }
        ),
      ]);
    },
  },
];

const testResultModifiedBody = computed(() => {
  if (!testResult.value) return '';
  if (testResult.value.modified_body) return testResult.value.modified_body;
  const target = testResult.value.phase === 'response' ? testResult.value.response : testResult.value.request;
  if (target && target.body) {
    return typeof target.body === 'object' ? JSON.stringify(target.body, null, 2) : target.body;
  }
  return '';
});

const filteredLogs = computed(() => {
  if (!logFilter.value) return capturedLogs.value;
  const q = logFilter.value.toLowerCase();
  return capturedLogs.value.filter((l) => l.toLowerCase().includes(q));
});

function getLogLineClass(line) {
  if (line.includes('[ERROR]')) return 'text-rose-400 font-semibold';
  if (line.includes('[WARN]')) return 'text-amber-400';
  if (line.includes('[INFO]')) return 'text-cyan-400';
  if (line.includes('[DEBUG]')) return 'text-slate-400';
  return 'text-slate-200';
}

function clearLogs() {
  capturedLogs.value = [];
}

function copyLogs() {
  const text = capturedLogs.value.join('\n');
  if (navigator.clipboard && text) {
    navigator.clipboard.writeText(text);
    message.success('已复制日志到剪贴板');
  }
}

function addHeaderRow(list) {
  list.push({ key: '', value: '' });
}

function removeHeaderRow(list, index) {
  list.splice(index, 1);
}

function formatBodyJson() {
  try {
    const parsed = JSON.parse(sandboxForm.body);
    sandboxForm.body = JSON.stringify(parsed, null, 2);
    message.success('JSON 格式化成功');
  } catch {
    message.warning('当前 Body 不是合法 JSON 格式');
  }
}

function onTemplateSelect(key) {
  const t = TEMPLATES.find((item) => item.key === key);
  if (!t) return;
  if (!editingName.value && (!currentPlugin.name || currentPlugin.name.startsWith('auth_') || currentPlugin.name.startsWith('model_') || currentPlugin.name.startsWith('trace_') || currentPlugin.name.startsWith('privacy_') || currentPlugin.name.startsWith('rate_'))) {
    currentPlugin.name = t.defaultName;
  }
  currentPlugin.phase = t.phase;
  currentPlugin.priority = t.priority;
  currentPlugin.script = t.script;
  applyTemplatePreset();
  message.info(`已载入模板: ${t.label}`);
  validateSyntax();
}

function applyTemplatePreset() {
  const t = TEMPLATES.find((item) => item.key === selectedTemplateKey.value) || TEMPLATES[0];
  if (!t || !t.mockPreset) return;
  const p = t.mockPreset;
  sandboxForm.phase = p.phase || (currentPlugin.phase === 'on_response' ? 'response' : 'request');
  sandboxForm.method = p.method || 'POST';
  sandboxForm.path = p.path || '/v1/chat/completions';
  sandboxForm.client_ip = p.client_ip || '127.0.0.1';
  sandboxForm.model = p.model || 'gpt-4o';
  sandboxForm.status = p.status || 200;
  sandboxForm.body = p.body || '{}';
  if (sandboxForm.phase === 'request') {
    sandboxForm.requestHeaders = (p.headers || []).map((h) => ({ ...h }));
  } else {
    sandboxForm.responseHeaders = (p.headers || []).map((h) => ({ ...h }));
  }
  message.success('已根据模板配置 Mock 测试参数');
}

async function loadPlugins() {
  loading.value = true;
  try {
    const data = await api.getJson('/admin/v1/plugins');
    if (data && Array.isArray(data)) {
      plugins.value = data;
    }
  } finally {
    loading.value = false;
  }
}

function createNewPlugin() {
  editingName.value = null;
  selectedTemplateKey.value = 'token_auth';
  const t = TEMPLATES[0];
  currentPlugin.name = t.defaultName;
  currentPlugin.version = '1.0.0';
  currentPlugin.phase = t.phase;
  currentPlugin.priority = t.priority;
  currentPlugin.enabled = true;
  currentPlugin.script = t.script;
  originalScript.value = t.script;
  showDiff.value = false;
  syntaxStatus.value = 'valid';
  workbenchVisible.value = true;
  applyTemplatePreset();
}

function editPlugin(row) {
  editingName.value = row.name;
  currentPlugin.name = row.name;
  currentPlugin.version = row.version || '1.0.0';
  currentPlugin.phase = row.phase || 'on_request';
  currentPlugin.priority = row.priority || 100;
  currentPlugin.enabled = row.enabled !== false;
  currentPlugin.script = row.script || defaultScriptTemplate;
  originalScript.value = row.script || defaultScriptTemplate;
  showDiff.value = false;
  syntaxStatus.value = 'valid';
  workbenchVisible.value = true;
  validateSyntax();
}

function onScriptChange() {
  clearTimeout(debounceTimer);
  syntaxStatus.value = 'checking';
  debounceTimer = setTimeout(() => {
    validateSyntax();
  }, 400);
}

async function validateSyntax() {
  try {
    const resp = await api.postJson('/admin/v1/plugins/validate', { script: currentPlugin.script });
    if (resp && resp.valid) {
      syntaxStatus.value = 'valid';
      syntaxError.value = '';
      if (monacoRef.value) monacoRef.value.setMarkers([]);
    } else if (resp) {
      syntaxStatus.value = 'error';
      syntaxError.value = resp.error || '语法错误';
      if (monacoRef.value && resp.line) {
        monacoRef.value.setMarkers([
          {
            startLineNumber: resp.line,
            endLineNumber: resp.line,
            startColumn: 1,
            endColumn: 100,
            message: resp.error || 'Syntax error',
            severity: 8, // MarkerSeverity.Error
          },
        ]);
      }
    }
  } catch {
    syntaxStatus.value = 'valid';
  }
}

function toggleDiffMode() {
  showDiff.value = !showDiff.value;
}

function openSandbox() {
  sandboxVisible.value = true;
}

async function runSandboxTest() {
  runningTest.value = true;
  try {
    const headersObj = {};
    const headerList = sandboxForm.phase === 'request' ? sandboxForm.requestHeaders : sandboxForm.responseHeaders;
    headerList.forEach((h) => {
      if (h.key && h.key.trim()) {
        headersObj[h.key.trim()] = h.value || '';
      }
    });

    const payload = {
      script_code: currentPlugin.script,
      phase: sandboxForm.phase,
      mock_payload: {
        method: sandboxForm.method,
        path: sandboxForm.path,
        client_ip: sandboxForm.client_ip,
        model: sandboxForm.model,
        headers: headersObj,
        body: sandboxForm.body,
        status: sandboxForm.status,
      },
    };

    const resp = await api.postJson('/admin/v1/plugins/test', payload);
    testResult.value = resp;
    if (resp && Array.isArray(resp.logs)) {
      capturedLogs.value = [...resp.logs];
    } else {
      capturedLogs.value = [];
    }
    if (resp && resp.success) {
      message.success('沙箱仿真测试通过');
    } else if (resp && resp.aborted) {
      message.warning(`沙箱测试被主动熔断 (HTTP ${resp.abort_status})`);
    } else {
      message.error(resp?.error || '沙箱执行出错');
    }
  } catch (err) {
    message.error(err.message || '运行沙箱测试失败');
  } finally {
    runningTest.value = false;
  }
}

async function savePlugin() {
  if (!currentPlugin.name || !currentPlugin.script) {
    message.warning('请填写插件名称与源码');
    return;
  }
  saving.value = true;
  try {
    let resp;
    if (editingName.value) {
      resp = await api.put(`/admin/v1/plugins/${editingName.value}`, currentPlugin);
    } else {
      resp = await api.post('/admin/v1/plugins', currentPlugin);
    }
    if (resp && resp.ok) {
      message.success('Lua 插件已成功保存并热重载生效！');
      workbenchVisible.value = false;
      loadPlugins();
    } else {
      message.error('保存插件失败');
    }
  } finally {
    saving.value = false;
  }
}

async function togglePluginStatus(row, val) {
  row.enabled = val;
  const resp = await api.put(`/admin/v1/plugins/${row.name}`, { ...row, enabled: val });
  if (resp && resp.ok) {
    message.success(`已${val ? '启用' : '禁用'}插件 ${row.name}`);
  } else {
    row.enabled = !val;
    message.error('修改插件状态失败');
  }
}

async function deletePlugin(name) {
  const resp = await api.delete(`/admin/v1/plugins/${name}`);
  if (resp && resp.ok) {
    message.success('插件已卸载删除');
    loadPlugins();
  }
}

onMounted(() => {
  loadPlugins();
});
</script>
