<template>
  <div id="tab-shadow" data-tab="shadow" class="space-y-4">
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">流量镜像与金丝雀灰度 (Traffic Shadowing & Canary)</h2>
        <span class="text-xs px-2 py-0.5 rounded-full bg-slate-800 text-slate-400 border border-slate-700">
          共 {{ rules.length }} 条规则
        </span>
      </div>
      <div class="flex items-center space-x-2">
        <n-button size="small" secondary @click="loadRules">刷新</n-button>
        <n-button size="small" type="primary" @click="openModal()">+ 添加镜像规则</n-button>
      </div>
    </div>

    <!-- Shadow KPIs -->
    <div class="grid grid-cols-2 sm:grid-cols-4 gap-3">
      <div class="p-3 bg-slate-900 border border-slate-800 rounded-lg">
        <span class="text-[11px] text-slate-400">镜像总请求</span>
        <div id="shadowKpiTotal" class="text-lg font-bold font-mono text-white mt-1">{{ stats.total || 0 }}</div>
      </div>
      <div class="p-3 bg-slate-900 border border-slate-800 rounded-lg">
        <span class="text-[11px] text-slate-400">平均延迟差</span>
        <div id="shadowKpiLatency" class="text-lg font-bold font-mono text-cyan-400 mt-1">{{ stats.latency_delta || 0 }}ms</div>
      </div>
      <div class="p-3 bg-slate-900 border border-slate-800 rounded-lg">
        <span class="text-[11px] text-slate-400">影子额外开销</span>
        <div id="shadowKpiCost" class="text-lg font-bold font-mono text-emerald-400 mt-1">${{ (stats.cost || 0).toFixed(4) }}</div>
      </div>
      <div class="p-3 bg-slate-900 border border-slate-800 rounded-lg">
        <span class="text-[11px] text-slate-400">一致性健康度</span>
        <div id="shadowKpiHealth" class="text-lg font-bold font-mono text-indigo-400 mt-1">{{ stats.health || '100%' }}</div>
      </div>
    </div>

    <!-- Shadow Rules Table -->
    <div class="glass-panel rounded-xl overflow-hidden">
      <n-data-table :columns="columns" :data="rules" :loading="loading" />
    </div>

    <!-- Modal -->
    <n-modal
      id="shadowRuleModal"
      v-model:show="modalVisible"
      preset="card"
      title="配置流量镜像规则"
      class="max-w-md bg-slate-900 border border-slate-800 text-slate-100"
    >
      <n-form :model="form" label-placement="left" label-width="120" class="space-y-2">
        <n-form-item label="主生产模型" required>
          <n-input v-model:value="form.source_model" placeholder="例如: gpt-4o" />
        </n-form-item>
        <n-form-item label="影子对照模型" required>
          <n-input v-model:value="form.shadow_model" placeholder="例如: qwen-max-latest" />
        </n-form-item>
        <n-form-item label="采样率 (0.01 - 1.0)">
          <n-slider v-model:value="form.sample_rate" :min="0.01" :max="1" :step="0.05" />
        </n-form-item>
        <n-form-item label="启用规则">
          <n-switch v-model:value="form.enabled" />
        </n-form-item>
      </n-form>
      <template #footer>
        <div class="flex justify-end space-x-2">
          <n-button @click="modalVisible = false">取消</n-button>
          <n-button type="primary" @click="saveRule">保存规则</n-button>
        </div>
      </template>
    </n-modal>

    <!-- Diff Modal -->
    <n-modal
      id="shadowDiffModal"
      v-model:show="diffModalVisible"
      preset="card"
      title="影子流量评估与比对"
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

const stats = ref({
  total: 0,
  latency_delta: 0,
  cost: 0,
  health: '100%',
});

const form = reactive({
  source_model: 'gpt-4o',
  shadow_model: '',
  sample_rate: 0.1,
  enabled: true,
});

const columns = [
  { title: '主生产模型 (Source)', key: 'source_model', width: 170 },
  { title: '影子镜像模型 (Shadow)', key: 'shadow_model', width: 170 },
  {
    title: '采样率',
    key: 'sample_rate',
    width: 120,
    render(row) {
      return `${Math.round((row.sample_rate || 0.1) * 100)}% 流量`;
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
          default: () => '确定删除此镜像规则吗？',
        }
      );
    },
  },
];

async function loadRules() {
  loading.value = true;
  try {
    const data = await api.getJson('/admin/v1/shadow/rules');
    if (data && Array.isArray(data)) rules.value = data;
  } finally {
    loading.value = false;
  }
}

function openModal() {
  form.source_model = 'gpt-4o';
  form.shadow_model = '';
  form.sample_rate = 0.1;
  form.enabled = true;
  modalVisible.value = true;
}

async function saveRule() {
  if (!form.source_model || !form.shadow_model) {
    message.warning('请填写主生产模型与镜像模型');
    return;
  }
  const resp = await api.post('/admin/v1/shadow/rules', form);
  if (resp && resp.ok) {
    message.success('镜像规则已成功配置');
    modalVisible.value = false;
    loadRules();
  } else {
    message.error('保存失败');
  }
}

async function deleteRule(id) {
  const resp = await api.delete(`/admin/v1/shadow/rules/${id}`);
  if (resp && resp.ok) {
    message.success('镜像规则已删除');
    loadRules();
  }
}

onMounted(() => {
  loadRules();
});
</script>
