<template>
  <div id="tab-cache-optimizer" data-tab="cache-optimizer" class="space-y-4">
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">Prompt Cache 前缀对齐与 KV 优化 (KV Cache Optimizer)</h2>
        <span class="text-xs px-2 py-0.5 rounded-full bg-slate-800 text-slate-400 border border-slate-700">
          共 {{ rules.length }} 条对齐规则
        </span>
      </div>
      <div class="flex items-center space-x-2">
        <n-button size="small" secondary @click="loadRules">刷新</n-button>
        <n-button size="small" type="primary" @click="openModal()">+ 添加前缀对齐规则</n-button>
      </div>
    </div>

    <!-- Cache Optimizer KPIs -->
    <div class="grid grid-cols-2 sm:grid-cols-4 gap-3">
      <div class="p-3 bg-slate-900 border border-slate-800 rounded-lg">
        <span class="text-[11px] text-slate-400">KV 命中率提升</span>
        <div id="cacheOptKpiHitRate" class="text-lg font-bold font-mono text-emerald-400 mt-1">48.6%</div>
      </div>
      <div class="p-3 bg-slate-900 border border-slate-800 rounded-lg">
        <span class="text-[11px] text-slate-400">对齐优化 Tokens</span>
        <div id="cacheOptKpiTokens" class="text-lg font-bold font-mono text-white mt-1">8,920,000</div>
      </div>
      <div class="p-3 bg-slate-900 border border-slate-800 rounded-lg">
        <span class="text-[11px] text-slate-400">折算推理节省</span>
        <div id="cacheOptKpiSavings" class="text-lg font-bold font-mono text-cyan-400 mt-1">$120.50</div>
      </div>
      <div class="p-3 bg-slate-900 border border-slate-800 rounded-lg">
        <span class="text-[11px] text-slate-400">TTFT 首字加速</span>
        <div id="cacheOptKpiLatency" class="text-lg font-bold font-mono text-indigo-400 mt-1">-340ms</div>
      </div>
    </div>

    <!-- Cache Optimizer Rules Table -->
    <div class="glass-panel rounded-xl overflow-hidden">
      <n-data-table :columns="columns" :data="rules" :loading="loading" />
    </div>

    <!-- Modal -->
    <n-modal
      id="cacheOptRuleModal"
      v-model:show="modalVisible"
      preset="card"
      title="配置 Prompt Cache 前缀对齐规则"
      class="max-w-md bg-slate-900 border border-slate-800 text-slate-100"
    >
      <n-form :model="form" label-placement="left" label-width="120" class="space-y-2">
        <n-form-item label="生效模型" required>
          <n-input v-model:value="form.model" placeholder="* (全部) 或指定模型" />
        </n-form-item>
        <n-form-item label="最小前缀长度">
          <n-input-number v-model:value="form.min_prefix_tokens" :min="256" :step="256" />
        </n-form-item>
        <n-form-item label="对齐算法">
          <n-select v-model:value="form.strategy" :options="strategyOptions" />
        </n-form-item>
        <n-form-item label="启用对齐">
          <n-switch v-model:value="form.enabled" />
        </n-form-item>
      </n-form>
      <template #footer>
        <div class="flex justify-end space-x-2">
          <n-button @click="modalVisible = false">取消</n-button>
          <n-button type="primary" @click="saveRule">保存配置</n-button>
        </div>
      </template>
    </n-modal>

    <!-- Detail Modal -->
    <n-modal
      id="cacheOptDetailModal"
      v-model:show="detailModalVisible"
      preset="card"
      title="前缀缓存对齐分析明细"
      class="max-w-md bg-slate-900 border border-slate-800 text-slate-100"
    >
      <div class="text-xs text-slate-300">前缀对齐详情</div>
    </n-modal>
  </div>
</template>

<script setup>
import { ref, h, onMounted, reactive } from 'vue';
import {
  NButton,
  NDataTable,
  NModal,
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
const rules = ref([]);
const loading = ref(false);
const modalVisible = ref(false);
const detailModalVisible = ref(false);

const strategyOptions = [
  { label: '系统前缀严格对其 (System Prefix Alignment)', value: 'system_prefix' },
  { label: '多轮会话上下文树截断 (Multi-turn Context Tree)', value: 'multi_turn' },
];

const form = reactive({
  model: '*',
  min_prefix_tokens: 1024,
  strategy: 'system_prefix',
  enabled: true,
});

const columns = [
  { title: '生效模型', key: 'model', width: 160 },
  {
    title: '对齐策略',
    key: 'strategy',
    width: 200,
    render(row) {
      return h(NTag, { size: 'small', type: 'info', bordered: false }, { default: () => row.strategy });
    },
  },
  {
    title: '最小前缀 Tokens',
    key: 'min_prefix_tokens',
    width: 150,
    render(row) {
      return `${row.min_prefix_tokens || 1024} Tokens`;
    },
  },
  {
    title: '状态',
    key: 'enabled',
    width: 100,
    render(row) {
      return row.enabled
        ? h(NTag, { size: 'small', type: 'success', bordered: false }, { default: () => '生效中' })
        : h(NTag, { size: 'small', type: 'default', bordered: false }, { default: () => '已停用' });
    },
  },
  {
    title: '操作',
    key: 'actions',
    width: 100,
    render(row) {
      return h(
        NPopconfirm,
        { onPositiveClick: () => deleteRule(row.id) },
        {
          trigger: () => h(NButton, { size: 'tiny', type: 'error', secondary: true }, { default: () => '删除' }),
          default: () => '确定删除此前缀对齐规则吗？',
        }
      );
    },
  },
];

async function loadRules() {
  loading.value = true;
  try {
    const data = await api.getJson('/admin/v1/cache-optimizer/rules');
    if (data && Array.isArray(data)) rules.value = data;
  } finally {
    loading.value = false;
  }
}

function openModal() {
  form.model = '*';
  form.min_prefix_tokens = 1024;
  form.strategy = 'system_prefix';
  form.enabled = true;
  modalVisible.value = true;
}

async function saveRule() {
  const resp = await api.post('/admin/v1/cache-optimizer/rules', form);
  if (resp && resp.ok) {
    message.success('前缀对齐规则保存成功');
    modalVisible.value = false;
    loadRules();
  } else {
    message.error('保存失败');
  }
}

async function deleteRule(id) {
  const resp = await api.delete(`/admin/v1/cache-optimizer/rules/${id}`);
  if (resp && resp.ok) {
    message.success('规则已删除');
    loadRules();
  }
}

onMounted(() => {
  loadRules();
});
</script>
