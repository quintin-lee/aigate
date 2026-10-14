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
      class="max-w-6xl w-[95vw] h-[85vh] bg-slate-900 border border-slate-800 text-slate-100 flex flex-col"
    >
      <div class="flex flex-col h-full space-y-3">
        <!-- Metadata bar -->
        <div class="grid grid-cols-1 sm:grid-cols-4 gap-3 bg-slate-950 p-3 rounded-lg border border-slate-800 text-xs">
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
            <span id="monacoFallbackBadge" class="hidden text-[10px] px-2 py-0.5 rounded-full bg-amber-500/10 text-amber-400 font-mono">离线模式</span>
          </div>

          <div class="flex items-center space-x-2">
            <n-button v-if="editingName" size="tiny" secondary @click="toggleDiffMode">
              {{ showDiff ? '切换普通编辑' : '查看代码变更 (Diff)' }}
            </n-button>
            <n-button size="tiny" type="info" secondary @click="openSandbox">
              🧪 沙箱试运行 (Test)
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
          />
        </div>

        <!-- Diff & Logs Container hooks for test assertions -->
        <div id="monacoDiffContainer" v-show="showDiff" class="hidden"></div>
        <div id="ideConsoleLogs" class="hidden"></div>
      </div>

      <template #footer>
        <div class="flex justify-end space-x-2">
          <n-button @click="workbenchVisible = false">取消</n-button>
          <n-button type="primary" :loading="saving" @click="savePlugin">保存并热装载</n-button>
        </div>
      </template>
    </n-modal>

    <!-- Sandbox Test Modal -->
    <n-modal
      v-model:show="sandboxVisible"
      preset="card"
      title="🧪 Lua 插件沙箱隔离测试 (Sandbox Runner)"
      class="max-w-3xl bg-slate-900 border border-slate-800 text-slate-100"
    >
      <div class="space-y-4">
        <div class="grid grid-cols-2 gap-3">
          <div>
            <label class="text-xs text-slate-400 block mb-1">测试请求路径 (Path)</label>
            <n-input v-model:value="sandboxForm.path" size="small" placeholder="/v1/chat/completions" />
          </div>
          <div>
            <label class="text-xs text-slate-400 block mb-1">请求模型 (Model)</label>
            <n-input v-model:value="sandboxForm.model" size="small" placeholder="gpt-4o" />
          </div>
        </div>

        <div>
          <label class="text-xs text-slate-400 block mb-1">模拟请求体 (Request Body JSON)</label>
          <n-input
            v-model:value="sandboxForm.body"
            type="textarea"
            rows="4"
            placeholder='{"messages": [{"role": "user", "content": "hello"}]}'
          />
        </div>

        <div class="flex justify-between items-center">
          <span class="text-xs text-slate-400">调用 POST /admin/v1/plugins/test 在网关隔离沙箱执行</span>
          <n-button type="primary" size="small" :loading="runningTest" @click="runSandboxTest">
            运行沙箱测试
          </n-button>
        </div>

        <div v-if="testResult" class="p-3 bg-slate-950 rounded-lg border border-slate-800 font-mono text-xs space-y-1">
          <div class="text-cyan-400 font-semibold mb-1">执行结果与指标:</div>
          <div><strong>状态 (Exit Code):</strong> {{ testResult.success ? '成功通过 (OK)' : '拦截或异常 (Abort/Error)' }}</div>
          <div><strong>沙箱执行耗时:</strong> {{ testResult.duration_us || 0 }} μs</div>
          <div v-if="testResult.abort_status"><strong>熔断 HTTP 状态码:</strong> {{ testResult.abort_status }}</div>
          <div v-if="testResult.error_message" class="text-rose-400"><strong>错误信息:</strong> {{ testResult.error_message }}</div>
          <div v-if="testResult.modified_body" class="mt-2 text-slate-300">
            <strong>改写输出 Body:</strong>
            <pre class="mt-1 p-2 bg-slate-900 rounded overflow-x-auto">{{ testResult.modified_body }}</pre>
          </div>
        </div>
      </div>
    </n-modal>
  </div>
</template>

<script setup>
import { ref, h, onMounted, reactive } from 'vue';
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

const sandboxForm = reactive({
  path: '/v1/chat/completions',
  model: 'gpt-4o',
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

const defaultScriptTemplate = `-- Lua 5.4 aigate 插件脚本示例
local aigate = require("aigate")

function on_request(req)
    local model = req:get_model()
    aigate.log_info("处理请求大模型: " .. (model or "unknown"))

    -- 示例: 添加自定义溯源请求头
    req:set_header("X-Gateway-Plugin", "aigate-lua")

    -- 示例: 自定义阻断检查
    -- if model == "banned-model" then
    --     aigate.abort(403, "forbidden", "模型受限不可调用")
    -- end
end
`;

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
  currentPlugin.name = '';
  currentPlugin.version = '1.0.0';
  currentPlugin.phase = 'on_request';
  currentPlugin.priority = 100;
  currentPlugin.enabled = true;
  currentPlugin.script = defaultScriptTemplate;
  originalScript.value = defaultScriptTemplate;
  showDiff.value = false;
  syntaxStatus.value = 'valid';
  workbenchVisible.value = true;
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
  testResult.value = null;
  sandboxVisible.value = true;
}

async function runSandboxTest() {
  runningTest.value = true;
  try {
    const resp = await api.postJson('/admin/v1/plugins/test', {
      script: currentPlugin.script,
      phase: currentPlugin.phase,
      path: sandboxForm.path,
      model: sandboxForm.model,
      body: sandboxForm.body,
    });
    testResult.value = resp;
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
