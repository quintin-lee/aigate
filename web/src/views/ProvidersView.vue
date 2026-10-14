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
        <n-button size="small" secondary @click="loadProviders">刷新</n-button>
        <n-button size="small" type="primary" @click="openModal()">+ 添加供应商</n-button>
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
            placeholder="留空则不修改密钥"
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
  NTag,
  NPopconfirm,
  useMessage,
} from 'naive-ui';
import { api } from '@/api/client';

const message = useMessage();
const loading = ref(false);
const saving = ref(false);
const providers = ref([]);
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

const columns = [
  { title: 'ID', key: 'id', width: 140 },
  { title: '供应商名称', key: 'name', width: 150 },
  {
    title: '协议类型',
    key: 'type',
    width: 120,
    render(row) {
      return h(NTag, { size: 'small', type: 'info', bordered: false }, { default: () => row.type });
    },
  },
  { title: 'Base URL', key: 'base_url', ellipsis: { tooltip: true } },
  {
    title: '并发 / 权重',
    key: 'stats',
    width: 130,
    render(row) {
      return `${row.max_concurrency || 100} / 权重 ${row.weight || 10}`;
    },
  },
  {
    title: '健康探针',
    key: 'health',
    width: 110,
    render(row) {
      return h(
        NButton,
        {
          size: 'tiny',
          secondary: true,
          type: 'warning',
          onClick: () => probeProvider(row.id),
        },
        { default: () => '探针测速' }
      );
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
    const data = await api.getJson('/admin/v1/providers');
    if (data && Array.isArray(data)) {
      providers.value = data;
    }
  } finally {
    loading.value = false;
  }
}

function openModal(row = null) {
  if (row) {
    editingId.value = row.id;
    form.id = row.id;
    form.name = row.name || '';
    form.type = row.type || 'openai';
    form.base_url = row.base_url || '';
    form.api_key = '';
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
  const resp = await api.post(`/admin/v1/providers/${id}/probe`);
  if (resp && resp.ok) {
    message.success(`探针测试完成: 供应商状态正常`);
  } else {
    message.warning(`探针测试异常或不可达`);
  }
}

onMounted(() => {
  loadProviders();
});
</script>
