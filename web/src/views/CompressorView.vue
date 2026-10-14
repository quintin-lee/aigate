<template>
  <div id="tab-compressor" data-tab="compressor" class="space-y-4">
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">提示词自适应压缩与 Token 瘦身 (Prompt Compressor)</h2>
        <span class="text-xs px-2 py-0.5 rounded-full bg-slate-800 text-slate-400 border border-slate-700">
          共 {{ rules.length }} 条策略
        </span>
      </div>
      <div class="flex items-center space-x-2">
        <n-button size="small" secondary @click="loadRules">刷新</n-button>
        <n-button size="small" type="primary" @click="openModal()">+ 添加压缩策略</n-button>
      </div>
    </div>

    <!-- Compressor KPIs -->
    <div class="grid grid-cols-2 sm:grid-cols-4 gap-3">
      <div class="p-3 bg-slate-900 border border-slate-800 rounded-lg">
        <span class="text-[11px] text-slate-400">已累计节约 Token</span>
        <div id="compressorKpiSavedTokens" class="text-lg font-bold font-mono text-emerald-400 mt-1">1,240,000</div>
      </div>
      <div class="p-3 bg-slate-900 border border-slate-800 rounded-lg">
        <span class="text-[11px] text-slate-400">累计资金节省</span>
        <div id="compressorKpiSavings" class="text-lg font-bold font-mono text-white mt-1">$45.80</div>
      </div>
      <div class="p-3 bg-slate-900 border border-slate-800 rounded-lg">
        <span class="text-[11px] text-slate-400">平均压缩比</span>
        <div id="compressorKpiRatio" class="text-lg font-bold font-mono text-cyan-400 mt-1">32.4%</div>
      </div>
      <div class="p-3 bg-slate-900 border border-slate-800 rounded-lg">
        <span class="text-[11px] text-slate-400">处理开销耗时</span>
        <div id="compressorKpiLatency" class="text-lg font-bold font-mono text-indigo-400 mt-1">1.2ms</div>
      </div>
    </div>

    <!-- Compressor Rules Table -->
    <div class="glass-panel rounded-xl overflow-hidden">
      <n-data-table :columns="columns" :data="rules" :loading="loading" />
    </div>

    <!-- Modal -->
    <n-modal
      id="compressorRuleModal"
      v-model:show="modalVisible"
      preset="card"
      title="配置提示词压缩策略"
      class="max-w-md bg-slate-900 border border-slate-800 text-slate-100"
    >
      <n-form :model="form" label-placement="left" label-width="120" class="space-y-2">
        <n-form-item label="生效模型" required>
          <n-input v-model:value="form.model" placeholder="* (全部) 或指定模型" />
        </n-form-item>
        <n-form-item label="压缩算法" required>
          <n-select v-model:value="form.strategy" :options="strategyOptions" />
        </n-form-item>
        <n-form-item label="目标瘦身率">
          <n-slider v-model:value="form.target_ratio" :min="0.1" :max="0.8" :step="0.05" />
        </n-form-item>
        <n-form-item label="启用压缩">
          <n-switch v-model:value="form.enabled" />
        </n-form-item>
      </n-form>
      <template #footer>
        <div class="flex justify-end space-x-2">
          <n-button @click="modalVisible = false">取消</n-button>
          <n-button type="primary" @click="saveRule">保存策略</n-button>
        </div>
      </template>
    </n-modal>

    <!-- Diff Modal -->
    <n-modal
      id="compressorDiffModal"
      v-model:show="diffModalVisible"
      preset="card"
      title="压缩比对明细"
      class="max-w-md bg-slate-900 border border-slate-800 text-slate-100"
    >
      <div class="text-xs text-slate-300">对照比对结果留存</div>
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
  NSelect,
  NSlider,
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
const diffModalVisible = ref(false);

const strategyOptions = [
  { label: '空白与冗余剪枝 (Whitespace & Redundancy)', value: 'whitespace' },
  { label: '低熵停用词精简 (Stop-word Pruning)', value: 'stopword' },
  { label: '抽取式语义精炼 (Extractive Compression)', value: 'extractive' },
];

const form = reactive({
  model: '*',
  strategy: 'whitespace',
  target_ratio: 0.3,
  enabled: true,
});

const columns = [
  { title: '生效模型', key: 'model', width: 160 },
  {
    title: '压缩算法',
    key: 'strategy',
    width: 180,
    render(row) {
      return h(NTag, { size: 'small', type: 'info', bordered: false }, { default: () => row.strategy });
    },
  },
  {
    title: '期望节约比例',
    key: 'target_ratio',
    width: 130,
    render(row) {
      return `${Math.round((row.target_ratio || 0.3) * 100)}%`;
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
          default: () => '确定删除此压缩策略吗？',
        }
      );
    },
  },
];

async function loadRules() {
  loading.value = true;
  try {
    const data = await api.getJson('/admin/v1/compressor/rules');
    if (data && Array.isArray(data)) rules.value = data;
  } finally {
    loading.value = false;
  }
}

function openModal() {
  form.model = '*';
  form.strategy = 'whitespace';
  form.target_ratio = 0.3;
  form.enabled = true;
  modalVisible.value = true;
}

async function saveRule() {
  const resp = await api.post('/admin/v1/compressor/rules', form);
  if (resp && resp.ok) {
    message.success('压缩策略配置成功');
    modalVisible.value = false;
    loadRules();
  } else {
    message.error('保存失败');
  }
}

async function deleteRule(id) {
  const resp = await api.delete(`/admin/v1/compressor/rules/${id}`);
  if (resp && resp.ok) {
    message.success('压缩策略已删除');
    loadRules();
  }
}

onMounted(() => {
  loadRules();
});
</script>
