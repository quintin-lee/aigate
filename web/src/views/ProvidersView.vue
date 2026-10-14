<template>
  <div class="space-y-4">
    <!-- Header Controls -->
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">模型供应商 (Upstream Providers)</h2>
        <span class="text-xs px-2 py-0.5 rounded-full bg-slate-800 text-slate-400 border border-slate-700">
          共 {{ providers.length }} 个
        </span>
      </div>
      <div class="flex items-center space-x-2">
        <n-button
          size="small"
          type="info"
          secondary
          :loading="probingAll"
          @click="probeAllProviders"
        >
          ⚡ 全量探针测速巡检
        </n-button>
        <n-button size="small" secondary :loading="loading" @click="loadProviders">刷新</n-button>
        <n-button size="small" type="primary" @click="openModal()">+ 添加供应商</n-button>
      </div>
    </div>

    <!-- Top 4 Radar Cards -->
    <div class="grid grid-cols-2 lg:grid-cols-4 gap-3">
      <!-- 1: Healthy Online -->
      <div class="glass-card p-3 rounded-xl border border-slate-800/80 bg-slate-900/60 relative overflow-hidden">
        <div class="flex justify-between items-center mb-1">
          <span class="text-xs text-slate-400">健康在线渠道</span>
          <span class="w-2 h-2 rounded-full bg-emerald-400 animate-pulse"></span>
        </div>
        <div class="flex items-baseline space-x-2">
          <span class="text-xl font-bold font-mono text-emerald-400">
            {{ healthyCount }} / {{ providers.length }}
          </span>
          <span class="text-xs text-slate-400">
            ({{ providers.length > 0 ? Math.round((healthyCount / providers.length) * 100) : 100 }}%)
          </span>
        </div>
        <div class="text-[11px] text-slate-400 mt-1">主动探针心跳就绪通道</div>
      </div>

      <!-- 2: Degraded & Circuit Breaker -->
      <div class="glass-card p-3 rounded-xl border border-slate-800/80 bg-slate-900/60 relative overflow-hidden">
        <div class="flex justify-between items-center mb-1">
          <span class="text-xs text-slate-400">熔断/降级警戒</span>
          <span v-if="alertCount > 0" class="w-2 h-2 rounded-full bg-amber-400 animate-pulse"></span>
        </div>
        <div class="flex items-baseline space-x-2">
          <span
            class="text-xl font-bold font-mono"
            :class="alertCount > 0 ? 'text-amber-400' : 'text-slate-300'"
          >
            {{ alertCount }}
          </span>
          <span class="text-xs text-slate-400">个通道</span>
        </div>
        <div class="text-[11px] text-slate-400 mt-1">
          {{ alertCount > 0 ? 'SLA 延迟超标或异常重试' : '全渠道 SLA 状态良好' }}
        </div>
      </div>

      <!-- 3: Average Latency RTT -->
      <div class="glass-card p-3 rounded-xl border border-slate-800/80 bg-slate-900/60 relative overflow-hidden">
        <div class="flex justify-between items-center mb-1">
          <span class="text-xs text-slate-400">全局平均探活 RTT</span>
          <span class="text-[11px] text-sky-400 font-mono">ICMP/HTTP</span>
        </div>
        <div class="flex items-baseline space-x-2">
          <span class="text-xl font-bold font-mono text-sky-400">
            {{ avgLatency > 0 ? avgLatency + ' ms' : '--' }}
          </span>
        </div>
        <div class="text-[11px] text-slate-400 mt-1">数据面直连上游网络时延</div>
      </div>

      <!-- 4: Attached Models Count -->
      <div class="glass-card p-3 rounded-xl border border-slate-800/80 bg-slate-900/60 relative overflow-hidden">
        <div class="flex justify-between items-center mb-1">
          <span class="text-xs text-slate-400">承载公开模型总数</span>
          <span class="text-[11px] text-indigo-400 font-mono">Active</span>
        </div>
        <div class="flex items-baseline space-x-2">
          <span class="text-xl font-bold font-mono text-indigo-400">
            {{ totalAttachedModels }}
          </span>
          <span class="text-xs text-slate-400">个已接入</span>
        </div>
        <div class="text-[11px] text-slate-400 mt-1">对外服务模型路由映射</div>
      </div>
    </div>

    <!-- Providers Table -->
    <div class="glass-panel rounded-xl overflow-hidden">
      <n-data-table
        :columns="columns"
        :data="providers"
        :loading="loading"
        :pagination="pagination"
        class="custom-table"
      />
    </div>

    <!-- Provider Edit/Add Modal -->
    <n-modal
      v-model:show="modalVisible"
      preset="card"
      :title="editingId ? '编辑供应商' : '添加供应商'"
      class="max-w-lg bg-slate-900 border border-slate-800 text-slate-100"
    >
      <n-form :model="form" label-placement="left" label-width="110" class="space-y-2">
        <n-form-item label="标识 (ID)" required>
          <n-input v-model:value="form.id" :disabled="!!editingId" placeholder="例如: openai-main" />
        </n-form-item>
        <n-form-item label="名称" required>
          <n-input v-model:value="form.name" placeholder="例如: OpenAI 官方" />
        </n-form-item>
        <n-form-item label="协议类型" required>
          <n-select v-model:value="form.type" :options="typeOptions" />
        </n-form-item>
        <n-form-item label="Base URL" required>
          <n-input v-model:value="form.base_url" placeholder="https://api.openai.com/v1" />
        </n-form-item>
        <n-form-item label="API Key">
          <n-input
            v-model:value="form.api_key"
            type="password"
            show-password-on="click"
            placeholder="支持填入明文密钥或 env:OPENAI_API_KEY 引用"
          />
        </n-form-item>
        <div class="grid grid-cols-3 gap-3">
          <n-form-item label="超时 (ms)">
            <n-input-number v-model:value="form.timeout_ms" :min="1000" :step="1000" />
          </n-form-item>
          <n-form-item label="最大并发">
            <n-input-number v-model:value="form.max_concurrency" :min="1" />
          </n-form-item>
          <n-form-item label="权重">
            <n-input-number v-model:value="form.weight" :min="1" />
          </n-form-item>
        </div>
      </n-form>
      <template #footer>
        <div class="flex justify-end space-x-2">
          <n-button @click="modalVisible = false">取消</n-button>
          <n-button type="primary" :loading="saving" @click="saveProvider">保存</n-button>
        </div>
      </template>
    </n-modal>
  </div>
</template>

<script setup>
import { ref, h, onMounted, reactive, computed } from 'vue';
import {
  NButton,
  NDataTable,
  NModal,
  NForm,
  NFormItem,
  NInput,
  NInputNumber,
  NSelect,
  NTag,
  NPopconfirm,
  useMessage,
} from 'naive-ui';
import { api } from '@/api/client';

const message = useMessage();
const loading = ref(false);
const saving = ref(false);
const probingAll = ref(false);
const providers = ref([]);
const healthMap = ref({});
const modelCounts = ref({});
const modalVisible = ref(false);
const editingId = ref(null);

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

const typeOptions = [
  { label: 'OpenAI 协议 (兼容 Claude / DeepSeek / 通义)', value: 'openai' },
  { label: 'Azure OpenAI', value: 'azure' },
  { label: 'Anthropic 原生', value: 'anthropic' },
  { label: 'Ollama 本地大模型', value: 'ollama' },
  { label: 'Gemini (Vertex AI)', value: 'gemini' },
];

const form = reactive({
  id: '',
  name: '',
  type: 'openai',
  base_url: 'https://api.openai.com/v1',
  api_key: '',
  timeout_ms: 30000,
  max_concurrency: 100,
  weight: 10,
});

// Computed KPIs
const healthyCount = computed(() => {
  return providers.value.filter(p => {
    const hInfo = healthMap.value[p.id];
    return !hInfo || hInfo.status === 'healthy' || hInfo.status === 'closed';
  }).length;
});

const alertCount = computed(() => {
  return providers.value.filter(p => {
    const hInfo = healthMap.value[p.id];
    return hInfo && (hInfo.status === 'degraded' || hInfo.status === 'down');
  }).length;
});

const avgLatency = computed(() => {
  const latencies = providers.value
    .map(p => healthMap.value[p.id]?.latency_ms)
    .filter(l => typeof l === 'number' && l > 0);
  if (latencies.length === 0) return 0;
  return Math.round(latencies.reduce((a, b) => a + b, 0) / latencies.length);
});

const totalAttachedModels = computed(() => {
  return Object.values(modelCounts.value).reduce((a, b) => a + b, 0);
});

const columns = [
  {
    title: '状态 / 标识',
    key: 'id',
    width: 170,
    render(row) {
      const hInfo = healthMap.value[row.id];
      let dotColor = 'bg-emerald-400';
      let title = '在线健康';
      if (hInfo) {
        if (hInfo.status === 'degraded') {
          dotColor = 'bg-amber-400';
          title = 'SLA 降级';
        } else if (hInfo.status === 'down') {
          dotColor = 'bg-rose-500';
          title = '断开或离线';
        } else if (hInfo.status === 'paused') {
          dotColor = 'bg-slate-500';
          title = '已停用';
        }
      }
      return h('div', { class: 'flex items-center space-x-2' }, [
        h('span', {
          class: `w-2 h-2 rounded-full ${dotColor} flex-shrink-0 shadow-sm`,
          title,
        }),
        h('span', { class: 'font-mono text-xs font-semibold text-white' }, row.id),
      ]);
    },
  },
  { title: '供应商名称', key: 'name', width: 140 },
  {
    title: '协议类型',
    key: 'type',
    width: 110,
    render(row) {
      return h(NTag, { size: 'small', type: 'info', bordered: false }, { default: () => row.type });
    },
  },
  {
    title: 'Base URL / 密钥形态',
    key: 'base_url',
    ellipsis: { tooltip: true },
    render(row) {
      const isEnvKey = (row.api_key || '').startsWith('env:') || (row.upstream_key_ref || '').startsWith('env:');
      const isPgKey = (row.api_key || '').startsWith('pg:') || (row.upstream_key_ref || '').startsWith('pg:');
      return h('div', { class: 'space-y-0.5' }, [
        h('div', { class: 'text-xs text-slate-300 font-mono truncate' }, row.base_url),
        h('div', { class: 'flex items-center space-x-1 text-[10px] text-slate-400' }, [
          isEnvKey
            ? h('span', { class: 'px-1.5 py-0.2 rounded bg-sky-950 text-sky-400 border border-sky-800 font-mono' }, '🔑 env 映射')
            : isPgKey
            ? h('span', { class: 'px-1.5 py-0.2 rounded bg-purple-950 text-purple-400 border border-purple-800 font-mono' }, '🔒 加密密钥')
            : h('span', { class: 'text-slate-400 font-mono' }, row.api_key ? '••••••••' : '未设置密钥'),
        ]),
      ]);
    },
  },
  {
    title: '并发 / 权重',
    key: 'stats',
    width: 120,
    render(row) {
      return `${row.max_concurrency || 100} / 权重 ${row.weight || 10}`;
    },
  },
  {
    title: '探活 RTT',
    key: 'latency',
    width: 110,
    render(row) {
      const hInfo = healthMap.value[row.id];
      if (!hInfo || typeof hInfo.latency_ms !== 'number' || hInfo.latency_ms <= 0) {
        return h('span', { class: 'text-xs text-slate-400' }, '未测速');
      }
      const lat = hInfo.latency_ms;
      const color = lat < 300 ? 'text-emerald-400' : lat < 1000 ? 'text-amber-400' : 'text-rose-400';
      return h('span', { class: `font-mono text-xs font-semibold ${color}` }, `${lat} ms`);
    },
  },
  {
    title: '承载模型',
    key: 'models',
    width: 95,
    render(row) {
      const cnt = modelCounts.value[row.id] || 0;
      return h(
        'span',
        { class: 'text-xs px-2 py-0.5 rounded-full bg-slate-800 text-slate-300 border border-slate-700' },
        `${cnt} 个`
      );
    },
  },
  {
    title: '操作',
    key: 'actions',
    width: 130,
    render(row) {
      return h('div', { class: 'flex space-x-1.5' }, [
        h(
          NButton,
          {
            size: 'tiny',
            secondary: true,
            type: 'info',
            onClick: () => probeProvider(row.id),
          },
          { default: () => '测速' }
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
            onPositiveClick: () => deleteProvider(row.id),
          },
          {
            trigger: () =>
              h(NButton, { size: 'tiny', type: 'error', secondary: true }, { default: () => '删除' }),
            default: () => `确定删除供应商 ${row.id} 吗？关联模型可能失效。`,
          }
        ),
      ]);
    },
  },
];

async function loadProviders() {
  loading.value = true;
  try {
    const [pData, hData, mData] = await Promise.all([
      api.getJson('/admin/v1/providers'),
      api.getJson('/admin/v1/providers/health').catch(() => null),
      api.getJson('/admin/v1/models').catch(() => null),
    ]);

    if (pData && Array.isArray(pData)) {
      providers.value = pData;
    }

    if (hData && Array.isArray(hData.providers)) {
      const map = {};
      hData.providers.forEach(p => {
        const key = p.name || p.id;
        map[key] = p;
        if (p.id) map[p.id] = p;
      });
      healthMap.value = map;
    }

    if (mData && Array.isArray(mData)) {
      const counts = {};
      mData.forEach(m => {
        if (m.provider_id) {
          counts[m.provider_id] = (counts[m.provider_id] || 0) + 1;
        }
        if (Array.isArray(m.targets)) {
          m.targets.forEach(t => {
            if (t.provider) {
              counts[t.provider] = (counts[t.provider] || 0) + 1;
            }
          });
        }
      });
      modelCounts.value = counts;
    }
  } finally {
    loading.value = false;
  }
}

async function probeAllProviders() {
  probingAll.value = true;
  try {
    message.info('正在向所有上游提供商发起并发探针测速...');
    const resp = await api.post('/admin/v1/providers/probe');
    if (resp && resp.ok) {
      message.success('全量探针测速完成');
      await loadProviders();
    } else {
      message.warning('探针测速部分完成或服务商处于离线状态');
      await loadProviders();
    }
  } catch {
    message.error('测速巡检请求失败');
  } finally {
    probingAll.value = false;
  }
}

function openModal(row = null) {
  if (row) {
    editingId.value = row.id;
    form.id = row.id;
    form.name = row.name || '';
    form.type = row.type || 'openai';
    form.base_url = row.base_url || '';
    form.api_key = row.api_key || row.upstream_key_ref || '';
    form.timeout_ms = row.timeout_ms || 30000;
    form.max_concurrency = row.max_concurrency || 100;
    form.weight = row.weight || 10;
  } else {
    editingId.value = null;
    form.id = '';
    form.name = '';
    form.type = 'openai';
    form.base_url = 'https://api.openai.com/v1';
    form.api_key = '';
    form.timeout_ms = 30000;
    form.max_concurrency = 100;
    form.weight = 10;
  }
  modalVisible.value = true;
}

async function saveProvider() {
  if (!form.id || !form.name || !form.base_url) {
    message.warning('请补全必要字段 (ID、名称、Base URL)');
    return;
  }

  saving.value = true;
  try {
    let resp;
    if (editingId.value) {
      resp = await api.put(`/admin/v1/providers/${editingId.value}`, form);
    } else {
      resp = await api.post('/admin/v1/providers', form);
    }
    if (resp && resp.ok) {
      message.success('供应商配置保存成功');
      modalVisible.value = false;
      loadProviders();
    } else {
      message.error('保存失败');
    }
  } finally {
    saving.value = false;
  }
}

async function deleteProvider(id) {
  const resp = await api.delete(`/admin/v1/providers/${id}`);
  if (resp && resp.ok) {
    message.success('供应商已成功删除');
    loadProviders();
  } else {
    message.error('删除供应商失败');
  }
}

async function probeProvider(id) {
  message.info(`正在探测供应商 ${id}...`);
  try {
    const resp = await api.post(`/admin/v1/providers/${id}/probe`);
    if (resp && resp.ok) {
      message.success(`供应商 ${id} 探针正常`);
      await loadProviders();
    } else {
      message.warning(`供应商 ${id} 测速异常或网络超时`);
      await loadProviders();
    }
  } catch {
    message.error(`探针探测请求失败`);
  }
}

onMounted(() => {
  loadProviders();
});
</script>
