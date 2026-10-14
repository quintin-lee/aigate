<template>
  <div id="tab-models" class="space-y-4">
    <!-- Header Controls -->
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">上游模型 (Models & Routing)</h2>
        <span class="text-xs px-2 py-0.5 rounded-full bg-slate-800 text-slate-400 border border-slate-700">
          共 {{ models.length }} 个
        </span>
      </div>
      <div class="flex items-center space-x-2">
        <n-button size="small" secondary :loading="loading" @click="loadModels">刷新</n-button>
        <n-button size="small" type="primary" @click="openModal()">+ 添加模型</n-button>
      </div>
    </div>

    <!-- Models Table -->
    <div class="glass-panel rounded-xl overflow-hidden">
      <n-data-table
        :columns="columns"
        :data="models"
        :loading="loading"
        :pagination="pagination"
        class="custom-table"
      />
    </div>

    <!-- Model Edit/Add Modal -->
    <n-modal
      v-model:show="modalVisible"
      preset="card"
      :title="editingName ? '编辑模型与路由配置' : '添加模型与多目标路由'"
      class="max-w-2xl bg-slate-900 border border-slate-800 text-slate-100"
    >
      <n-form :model="form" label-placement="left" label-width="120" class="space-y-4">
        <!-- Section 1: Basic Info -->
        <div class="bg-slate-950/40 p-3 rounded-lg border border-slate-800 space-y-2">
          <div class="text-xs font-semibold text-sky-400 mb-2">📋 基础标识与公开名称</div>
          <div class="grid grid-cols-2 gap-3">
            <n-form-item label="公开模型名" required>
              <n-input v-model:value="form.name" :disabled="!!editingName" placeholder="例如: gpt-4o" />
            </n-form-item>
            <n-form-item label="显示名称">
              <n-input v-model:value="form.display_name" placeholder="例如: GPT-4o 旗舰多模态" />
            </n-form-item>
          </div>
          <div class="grid grid-cols-2 gap-3">
            <n-form-item label="上下文长度">
              <n-input-number v-model:value="form.context_length" :min="1024" :step="1024" />
            </n-form-item>
            <n-form-item label="启用状态">
              <n-switch v-model:value="form.enabled" />
            </n-form-item>
          </div>
        </div>

        <!-- Section 2: Multi-target Routing Pool -->
        <div class="bg-slate-950/40 p-3 rounded-lg border border-slate-800 space-y-3">
          <div class="flex items-center justify-between">
            <div class="flex items-center space-x-2">
              <span class="text-xs font-semibold text-purple-400">🔀 多目标上游路由池 (Targets)</span>
              <span class="text-[10px] px-2 py-0.5 rounded bg-purple-950 text-purple-300 border border-purple-800">
                {{ form.targets.length }} 个节点
              </span>
            </div>
            <div class="flex items-center space-x-2">
              <span class="text-xs text-slate-400">调度算法:</span>
              <n-select
                v-model:value="form.lb_policy"
                size="tiny"
                class="w-36"
                :options="lbPolicyOptions"
              />
              <n-button size="tiny" secondary type="primary" @click="addTarget">
                + 添加目标
              </n-button>
            </div>
          </div>

          <!-- Traffic Ratio Preview Bar (for priority 0 targets) -->
          <div v-if="primaryRatioPreview.length > 0" class="bg-slate-900 p-2 rounded border border-slate-800">
            <div class="flex justify-between text-[11px] text-slate-400 mb-1">
              <span>🎯 主力流量分配预览 (Priority 0):</span>
              <span class="font-mono text-purple-300">合计权重: {{ primaryTotalWeight }}</span>
            </div>
            <div class="h-3 rounded overflow-hidden flex bg-slate-800">
              <div
                v-for="(item, idx) in primaryRatioPreview"
                :key="idx"
                :style="{ width: item.pct + '%', background: item.color }"
                class="h-full text-[9px] text-white font-bold flex items-center justify-center truncate px-1"
                :title="`${item.name} (${item.pct}%)`"
              >
                {{ item.pct >= 15 ? `${item.name} ${item.pct}%` : '' }}
              </div>
            </div>
          </div>

          <!-- Target Items List -->
          <div v-if="form.targets.length > 0" class="space-y-2">
            <div
              v-for="(tgt, idx) in form.targets"
              :key="idx"
              class="bg-slate-900/80 p-2.5 rounded-lg border border-slate-800/90 text-xs space-y-2"
            >
              <div class="flex items-center justify-between">
                <div class="flex items-center space-x-2">
                  <span
                    class="px-1.5 py-0.5 rounded text-[10px] font-bold"
                    :class="tgt.priority === 0 ? 'bg-sky-950 text-sky-400 border border-sky-800' : 'bg-amber-950 text-amber-400 border border-amber-800'"
                  >
                    Priority {{ tgt.priority }} ({{ tgt.priority === 0 ? '主力' : '热备' }})
                  </span>
                  <span class="font-semibold text-slate-200">目标 #{{ idx + 1 }}</span>
                </div>
                <n-button size="tiny" type="error" text @click="removeTarget(idx)">
                  ✕ 移除
                </n-button>
              </div>

              <div class="grid grid-cols-2 gap-2">
                <n-form-item label="供应商" :show-feedback="false">
                  <n-select v-model:value="tgt.provider" size="small" :options="providerOptions" />
                </n-form-item>
                <n-form-item label="上游 Base URL" :show-feedback="false">
                  <n-input v-model:value="tgt.endpoint" size="small" placeholder="https://api.openai.com/v1" />
                </n-form-item>
              </div>

              <div class="grid grid-cols-3 gap-2">
                <n-form-item label="梯队 (Priority)" :show-feedback="false">
                  <n-input-number v-model:value="tgt.priority" size="small" :min="0" :max="5" />
                </n-form-item>
                <n-form-item label="分流权重 (Weight)" :show-feedback="false">
                  <n-input-number v-model:value="tgt.weight" size="small" :min="1" :max="100" />
                </n-form-item>
                <n-form-item label="最大并发" :show-feedback="false">
                  <n-input-number v-model:value="tgt.max_concurrent" size="small" :min="0" />
                </n-form-item>
              </div>

              <div>
                <n-form-item label="密钥引用 (可选)" :show-feedback="false">
                  <n-input
                    v-model:value="tgt.upstream_key_ref"
                    size="small"
                    placeholder="留空继承服务商配置，或填 env:OPENAI_API_KEY / pg:KEY_ID"
                  />
                </n-form-item>
              </div>
            </div>
          </div>

          <!-- Fallback single provider inputs if targets empty -->
          <div v-else class="space-y-2 pt-1">
            <div class="text-[11px] text-slate-400">未配置多目标，使用单一主上游映射：</div>
            <div class="grid grid-cols-2 gap-3">
              <n-form-item label="关联供应商" required>
                <n-select v-model:value="form.provider_id" :options="providerOptions" />
              </n-form-item>
              <n-form-item label="上游实际模型名" required>
                <n-input v-model:value="form.provider_model_name" placeholder="例如: gpt-4o-2024-08-06" />
              </n-form-item>
            </div>
          </div>
        </div>

        <!-- Section 3: SLA Fallback & Pricing -->
        <div class="bg-slate-950/40 p-3 rounded-lg border border-slate-800 space-y-3">
          <div class="text-xs font-semibold text-amber-400">🛡️ SLA 容灾降级与费率计量</div>
          <div class="grid grid-cols-2 gap-3">
            <n-form-item label="SLA 兜底模型">
              <n-select
                v-model:value="form.fallback_model"
                clearable
                placeholder="主模型故障时自动切换 (如 deepseek-v3)"
                :options="fallbackModelOptions"
              />
            </n-form-item>
            <div class="flex items-center text-[11px] text-slate-400 pt-1">
              <span>当全部上游熔断或时延超限，自动透明重写至该模型。</span>
            </div>
          </div>
          <div class="grid grid-cols-2 gap-3">
            <n-form-item label="输入费率 ($/1M)">
              <n-input-number v-model:value="form.pricing_prompt_1k" :min="0" :precision="4" :step="0.001" />
            </n-form-item>
            <n-form-item label="输出费率 ($/1M)">
              <n-input-number v-model:value="form.pricing_completion_1k" :min="0" :precision="4" :step="0.001" />
            </n-form-item>
          </div>
        </div>

        <!-- Section 4: Deep Reasoning (Thinking) Parameters -->
        <div class="bg-slate-950/40 p-3 rounded-lg border border-slate-800 space-y-2">
          <div class="flex items-center justify-between mb-1">
            <span class="text-xs font-semibold text-sky-400">🧠 深度思考推理 (Reasoning) 参数视窗</span>
            <span class="text-[10px] text-slate-400">适配 DeepSeek-R1 / o1 / o3 / Claude 3.7</span>
          </div>
          <div class="grid grid-cols-2 gap-3">
            <n-form-item label="支持深度推理">
              <n-switch id="mFormSupportsReasoning" v-model:value="form.supports_reasoning" />
            </n-form-item>
            <n-form-item label="默认思考预算">
              <n-input-number id="mFormThinkingBudget" v-model:value="form.thinking_budget" :min="0" />
            </n-form-item>
          </div>
          <div class="grid grid-cols-2 gap-3">
            <n-form-item label="模拟思考标签">
              <n-switch id="mFormTagEmulationDefault" v-model:value="form.tag_emulation_default" />
            </n-form-item>
            <n-form-item label="推理计费倍率">
              <n-input-number id="mFormPricingReasoning" v-model:value="form.pricing_reasoning" :min="0" :precision="4" />
            </n-form-item>
          </div>
        </div>
      </n-form>

      <template #footer>
        <div class="flex justify-end space-x-2">
          <n-button @click="modalVisible = false">取消</n-button>
          <n-button type="primary" :loading="saving" @click="saveModel">保存配置</n-button>
        </div>
      </template>
    </n-modal>

    <!-- Simulation Drawer -->
    <n-drawer v-model:show="simulationDrawerVisible" :width="500" placement="right">
      <n-drawer-content title="🔬 智能路由仿真沙箱 (Routing Simulator)" closable>
        <div v-if="selectedModelForSim" class="space-y-4 text-xs font-mono">
          <!-- Model Card Header -->
          <div class="bg-slate-900 p-3 rounded-lg border border-slate-800 space-y-1">
            <div class="flex justify-between items-center">
              <span class="text-sm font-bold text-white">{{ selectedModelForSim.name }}</span>
              <span class="px-2 py-0.5 rounded bg-purple-950 text-purple-300 border border-purple-800 text-[10px]">
                策略: {{ selectedModelForSim.lb_policy || 'priority' }}
              </span>
            </div>
            <div class="text-[11px] text-slate-400">
              公开标识: {{ selectedModelForSim.display_name || selectedModelForSim.name }} · 上下文: {{ Math.round((selectedModelForSim.context_length || 128000) / 1024) }}K
            </div>
          </div>

          <!-- Simulation Result Box -->
          <div class="bg-slate-900 p-3 rounded-lg border border-sky-800/80 space-y-2">
            <div class="flex items-center justify-between text-sky-400 font-bold">
              <span>⚡ 模拟选路结果报告</span>
              <span class="text-[10px] bg-sky-950 px-2 py-0.5 rounded border border-sky-800">Dry Run 命中</span>
            </div>
            <div class="text-slate-200">
              命中节点: <strong class="text-emerald-400">{{ simulatedTarget.provider || selectedModelForSim.provider || 'openai' }}</strong>
            </div>
            <div class="text-slate-400 text-[11px]">
              端点: <span class="text-slate-300">{{ simulatedTarget.endpoint || selectedModelForSim.endpoint || '默认服务商端点' }}</span>
            </div>
            <div class="text-[11px] text-slate-400 pt-1 border-t border-slate-800">
              决策逻辑: <span class="text-sky-300">{{ simulationReason }}</span>
            </div>
          </div>

          <!-- Failure / Fallback Path Deduction -->
          <div class="bg-slate-900 p-3 rounded-lg border border-amber-800/80 space-y-2">
            <div class="text-amber-400 font-bold">🛡️ 容灾逃生推演 (Fault Simulation)</div>
            <div class="text-[11px] text-slate-300">
              若主力节点突发故障或超时：
            </div>
            <div class="text-[11px] text-slate-400 space-y-1 pl-2 border-l border-amber-800">
              <div>• 优先级梯队降级：{{ fallbackTargetDesc }}</div>
              <div>• 全局 SLA 兜底模型：<strong class="text-amber-300">{{ selectedModelForSim.fallback_model || '未绑定（将直接返回 502/504）' }}</strong></div>
            </div>
          </div>

          <!-- Injected Params Validation -->
          <div class="bg-slate-900 p-3 rounded-lg border border-slate-800 space-y-1.5">
            <div class="text-slate-300 font-bold text-[11px]">⚙️ 数据面参数注入校验</div>
            <div class="text-[11px] text-slate-400">
              • 思考推理支持: <span :class="selectedModelForSim.supports_reasoning ? 'text-emerald-400' : 'text-slate-400'">{{ selectedModelForSim.supports_reasoning ? '已开启' : '未开启' }}</span>
            </div>
            <div v-if="selectedModelForSim.supports_reasoning" class="text-[11px] text-slate-400">
              • 默认思考预算: <span class="text-sky-300 font-bold">{{ selectedModelForSim.thinking_budget || selectedModelForSim.default_thinking_budget || 8192 }} Tokens</span>
            </div>
            <div class="text-[11px] text-slate-400">
              • &lt;thought&gt; 标签模拟: <span :class="selectedModelForSim.tag_emulation_default ? 'text-sky-400' : 'text-slate-400'">{{ selectedModelForSim.tag_emulation_default ? '已启用模拟' : '直通' }}</span>
            </div>
          </div>

          <div class="flex justify-end pt-2">
            <n-button size="small" type="primary" secondary @click="runSimulation(selectedModelForSim)">
              🔄 再次运行单次选路推演
            </n-button>
          </div>
        </div>
      </n-drawer-content>
    </n-drawer>
  </div>
</template>

<script setup>
import { ref, h, onMounted, reactive, computed } from 'vue';
import {
  NButton,
  NDataTable,
  NModal,
  NDrawer,
  NDrawerContent,
  NForm,
  NFormItem,
  NInput,
  NInputNumber,
  NSelect,
  NSwitch,
  NTag,
  NPopconfirm,
  useMessage,
} from 'naive-ui';
import { api } from '@/api/client';

const message = useMessage();
const loading = ref(false);
const saving = ref(false);
const models = ref([]);
const providerOptions = ref([]);
const modalVisible = ref(false);
const simulationDrawerVisible = ref(false);
const selectedModelForSim = ref(null);
const simulatedTarget = ref({});
const simulationReason = ref('');
const fallbackTargetDesc = ref('');
const editingName = ref(null);

const pagination = reactive({
  page: 1,
  pageSize: 10,
  showSizePicker: true,
  pageSizes: [10, 20, 50],
  onChange: (page) => { pagination.page = page; },
  onUpdatePageSize: (pageSize) => {
    pagination.pageSize = pageSize;
    pagination.page = 1;
  },
});

const lbPolicyOptions = [
  { label: 'Priority (主备顺序容灾)', value: 'priority' },
  { label: 'Weighted (按权重比例分流)', value: 'weighted' },
  { label: 'Weighted Round Robin (平滑轮询)', value: 'weighted_round_robin' },
  { label: 'P2C (最小延迟自适应调度)', value: 'p2c' },
];

const fallbackModelOptions = computed(() => {
  return models.value
    .filter(m => m.name !== form.name)
    .map(m => ({ label: `${m.display_name || m.name} (${m.name})`, value: m.name }));
});

const form = reactive({
  name: '',
  display_name: '',
  provider_id: '',
  provider_model_name: '',
  context_length: 128000,
  pricing_prompt_1k: 0.0025,
  pricing_completion_1k: 0.01,
  enabled: true,
  lb_policy: 'priority',
  fallback_model: null,
  targets: [],
  supports_reasoning: false,
  thinking_budget: 0,
  tag_emulation_default: false,
  pricing_reasoning: 0,
});

// Dynamic traffic weight ratio preview for Priority 0 targets
const primaryRatioPreview = computed(() => {
  const pri0 = (form.targets || []).filter(t => (t.priority || 0) === 0);
  const totalWeight = pri0.reduce((acc, t) => acc + (t.weight || 1), 0);
  if (totalWeight === 0 || pri0.length === 0) return [];
  const colors = ['#0284c7', '#7c3aed', '#059669', '#d97706', '#db2777', '#4f46e5'];
  return pri0.map((t, idx) => ({
    name: t.provider || `Target #${idx + 1}`,
    pct: Math.round(((t.weight || 1) / totalWeight) * 100),
    color: colors[idx % colors.length],
  }));
});

const primaryTotalWeight = computed(() => {
  return (form.targets || [])
    .filter(t => (t.priority || 0) === 0)
    .reduce((acc, t) => acc + (t.weight || 1), 0);
});

function addTarget() {
  if (form.targets.length >= 8) {
    message.warning('最多支持添加 8 个上游目标节点');
    return;
  }
  form.targets.push({
    provider: providerOptions.value[0]?.value || 'openai',
    endpoint: '',
    upstream_key_ref: '',
    weight: 10,
    priority: form.targets.length === 0 ? 0 : 0,
    max_concurrent: 100,
  });
}

function removeTarget(idx) {
  form.targets.splice(idx, 1);
}

const columns = [
  {
    title: '公开模型标识',
    key: 'name',
    width: 170,
    render(row) {
      return h('div', { class: 'space-y-0.5' }, [
        h('div', { class: 'flex items-center space-x-1.5' }, [
          h('span', { class: 'font-mono font-semibold text-white text-xs' }, row.name),
          row.supports_reasoning
            ? h('span', { class: 'px-1 py-0.2 rounded bg-sky-950 text-sky-400 border border-sky-800 text-[10px]' }, '🧠')
            : null,
        ]),
        row.display_name
          ? h('div', { class: 'text-[11px] text-slate-400 truncate max-w-[150px]' }, row.display_name)
          : null,
      ]);
    },
  },
  {
    title: '上游路由与负载调度',
    key: 'routing',
    width: 220,
    render(row) {
      const targets = Array.isArray(row.targets) ? row.targets : [];
      if (targets.length > 0) {
        return h('div', { class: 'space-y-1' }, [
          h('div', { class: 'flex items-center space-x-1.5' }, [
            h(NTag, { size: 'tiny', type: 'info', bordered: false }, { default: () => `${targets.length} 个目标` }),
            h(NTag, { size: 'tiny', type: 'success', bordered: false }, { default: () => row.lb_policy || 'priority' }),
          ]),
          h('div', { class: 'text-[10px] text-slate-400 font-mono truncate' },
            targets.map(t => `${t.provider}(P${t.priority ?? 0}:W${t.weight ?? 1})`).join(' · ')
          ),
        ]);
      }
      return h('div', { class: 'space-y-0.5' }, [
        h(NTag, { size: 'tiny', type: 'default', bordered: false }, { default: () => row.provider_id || row.provider || 'openai' }),
        h('div', { class: 'text-[10px] text-slate-400 font-mono truncate' }, row.provider_model_name || row.endpoint || '--'),
      ]);
    },
  },
  {
    title: 'SLA 容灾兜底',
    key: 'fallback_model',
    width: 130,
    render(row) {
      const fb = row.fallback_model || (row.default_params && parseFallback(row.default_params));
      if (fb) {
        return h('span', { class: 'px-1.5 py-0.5 rounded bg-amber-950 text-amber-400 border border-amber-800 font-mono text-xs' }, fb);
      }
      return h('span', { class: 'text-xs text-slate-400' }, '未配置');
    },
  },
  {
    title: '上下文',
    key: 'context_length',
    width: 100,
    render(row) {
      const k = Math.round((row.context_length || 0) / 1024);
      return `${k}K`;
    },
  },
  {
    title: '启用状态',
    key: 'enabled',
    width: 90,
    render(row) {
      return h(NSwitch, {
        value: row.enabled !== false && row.enabled !== 0,
        onUpdateValue: (val) => toggleModelStatus(row, val),
      });
    },
  },
  {
    title: '操作',
    key: 'actions',
    width: 180,
    render(row) {
      return h('div', { class: 'flex space-x-1.5' }, [
        h(
          NButton,
          {
            size: 'tiny',
            secondary: true,
            type: 'primary',
            onClick: () => openSimulationDrawer(row),
          },
          { default: () => '🔬 仿真' }
        ),
        h(
          NButton,
          {
            size: 'tiny',
            secondary: true,
            onClick: () => openModal(row),
          },
          { default: () => '编辑' }
        ),
        h(
          NPopconfirm,
          {
            onPositiveClick: () => deleteModel(row.name),
          },
          {
            trigger: () =>
              h(NButton, { size: 'tiny', type: 'error', secondary: true }, { default: () => '删除' }),
            default: () => `确定下线删除模型 ${row.name} 吗？`,
          }
        ),
      ]);
    },
  },
];

function parseFallback(paramsStr) {
  try {
    const obj = typeof paramsStr === 'string' ? JSON.parse(paramsStr) : paramsStr;
    return obj?.fallback_model || null;
  } catch {
    return null;
  }
}

async function loadModels() {
  loading.value = true;
  try {
    const [mList, pList] = await Promise.all([
      api.getJson('/admin/v1/models'),
      api.getJson('/admin/v1/providers'),
    ]);
    if (mList) {
      models.value = Array.isArray(mList) ? mList : (mList.models || []);
    }
    if (pList && Array.isArray(pList)) {
      providerOptions.value = pList.map(p => ({ label: `${p.name} (${p.id})`, value: p.id }));
    }
  } finally {
    loading.value = false;
  }
}

function openModal(row = null) {
  if (row) {
    editingName.value = row.name;
    form.name = row.name;
    form.display_name = row.display_name || '';
    form.provider_id = row.provider_id || row.provider || '';
    form.provider_model_name = row.provider_model_name || '';
    form.context_length = row.context_length || 128000;
    form.pricing_prompt_1k = row.pricing_prompt_1k || (row.pricing?.in_mtok ? row.pricing.in_mtok / 1000 : 0.0025);
    form.pricing_completion_1k = row.pricing_completion_1k || (row.pricing?.out_mtok ? row.pricing.out_mtok / 1000 : 0.01);
    form.enabled = row.enabled !== false && row.enabled !== 0;
    form.lb_policy = row.lb_policy || 'priority';
    form.fallback_model = row.fallback_model || (row.default_params && parseFallback(row.default_params)) || null;
    form.supports_reasoning = !!row.supports_reasoning;
    form.thinking_budget = row.thinking_budget || row.default_thinking_budget || 0;
    form.tag_emulation_default = !!row.tag_emulation_default;
    form.pricing_reasoning = row.pricing_reasoning || 0;

    if (Array.isArray(row.targets) && row.targets.length > 0) {
      form.targets = JSON.parse(JSON.stringify(row.targets));
    } else {
      form.targets = [];
    }
  } else {
    editingName.value = null;
    form.name = '';
    form.display_name = '';
    form.provider_id = providerOptions.value[0]?.value || '';
    form.provider_model_name = '';
    form.context_length = 128000;
    form.pricing_prompt_1k = 0.0025;
    form.pricing_completion_1k = 0.01;
    form.enabled = true;
    form.lb_policy = 'priority';
    form.fallback_model = null;
    form.targets = [];
    form.supports_reasoning = false;
    form.thinking_budget = 0;
    form.tag_emulation_default = false;
    form.pricing_reasoning = 0;
  }
  modalVisible.value = true;
}

async function saveModel() {
  if (!form.name) {
    message.warning('请补全公开模型名');
    return;
  }

  saving.value = true;
  try {
    const payload = {
      name: form.name,
      display_name: form.display_name,
      provider_id: form.provider_id,
      provider_model_name: form.provider_model_name,
      context_length: form.context_length,
      pricing_prompt_1k: form.pricing_prompt_1k,
      pricing_completion_1k: form.pricing_completion_1k,
      enabled: form.enabled,
      lb_policy: form.lb_policy,
      fallback_model: form.fallback_model,
      supports_reasoning: form.supports_reasoning,
      thinking_budget: form.thinking_budget,
      tag_emulation_default: form.tag_emulation_default,
      pricing_reasoning: form.pricing_reasoning,
      targets: form.targets,
    };

    let resp;
    if (editingName.value) {
      resp = await api.put(`/admin/v1/models/${editingName.value}`, payload);
    } else {
      resp = await api.post('/admin/v1/models', payload);
    }
    if (resp && resp.ok) {
      message.success('模型与路由配置保存成功');
      modalVisible.value = false;
      loadModels();
    } else {
      message.error('保存模型失败');
    }
  } finally {
    saving.value = false;
  }
}

async function toggleModelStatus(row, val) {
  row.enabled = val;
  const resp = await api.put(`/admin/v1/models/${row.name}`, { ...row, enabled: val });
  if (resp && resp.ok) {
    message.success(`已${val ? '启用' : '禁用'}模型 ${row.name}`);
  } else {
    row.enabled = !val;
    message.error('修改模型状态失败');
  }
}

async function deleteModel(name) {
  const resp = await api.delete(`/admin/v1/models/${name}`);
  if (resp && resp.ok) {
    message.success('模型删除成功');
    loadModels();
  } else {
    message.error('删除模型失败');
  }
}

function openSimulationDrawer(row) {
  selectedModelForSim.value = row;
  runSimulation(row);
  simulationDrawerVisible.value = true;
}

function runSimulation(row) {
  const targets = Array.isArray(row.targets) && row.targets.length > 0
    ? row.targets
    : [{ provider: row.provider || row.provider_id || 'openai', endpoint: row.endpoint || '', priority: 0, weight: 10 }];

  const pri0 = targets.filter(t => (t.priority || 0) === 0);
  const pri1 = targets.filter(t => (t.priority || 0) > 0);

  let hit = pri0[0] || targets[0];
  const policy = row.lb_policy || 'priority';

  if (policy === 'weighted' || policy === 'weighted_round_robin') {
    const totalW = pri0.reduce((acc, t) => acc + (t.weight || 1), 0);
    let rand = Math.random() * totalW;
    for (const t of pri0) {
      rand -= (t.weight || 1);
      if (rand <= 0) {
        hit = t;
        break;
      }
    }
    simulationReason.value = `根据 ${policy} 算法，在主力 Priority 0 (合计权重 ${totalW}) 中随机加权命中`;
  } else if (policy === 'p2c') {
    if (pri0.length >= 2) {
      const idx1 = Math.floor(Math.random() * pri0.length);
      let idx2 = Math.floor(Math.random() * pri0.length);
      if (idx1 === idx2) idx2 = (idx1 + 1) % pri0.length;
      hit = (pri0[idx1].weight || 1) >= (pri0[idx2].weight || 1) ? pri0[idx1] : pri0[idx2];
      simulationReason.value = `两两随机抽样 (P2C) 比较候选 #${idx1 + 1} 与 #${idx2 + 1}，选取综合健康评分最优节点`;
    } else {
      simulationReason.value = `主力梯队仅 1 个活跃节点，直接路由命中`;
    }
  } else {
    simulationReason.value = `严格遵循 Priority 梯队顺序，首选 Priority 0 第一就绪节点`;
  }

  simulatedTarget.value = hit;

  if (pri1.length > 0) {
    fallbackTargetDesc.value = `自动热备切换至 Priority ${pri1[0].priority} 节点 (${pri1[0].provider})`;
  } else if (pri0.length > 1) {
    const alt = pri0.find(t => t !== hit);
    fallbackTargetDesc.value = `平移至同梯队备选节点 (${alt?.provider || '备选目标'})`;
  } else {
    fallbackTargetDesc.value = `无同级备选节点，直接触发全局 SLA 降级模型`;
  }
}

onMounted(() => {
  loadModels();
});
</script>
