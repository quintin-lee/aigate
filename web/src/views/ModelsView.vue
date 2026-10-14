<template>
  <div id="tab-models" class="space-y-4">
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">上游模型 (Models & Routing)</h2>
        <span class="text-xs px-2 py-0.5 rounded-full bg-slate-800 text-slate-400 border border-slate-700">
          共 {{ models.length }} 个
        </span>
      </div>
      <div class="flex items-center space-x-2">
        <n-button size="small" secondary @click="loadModels">刷新</n-button>
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
      />
    </div>

    <!-- Model Edit/Add Modal -->
    <n-modal
      v-model:show="modalVisible"
      preset="card"
      :title="editingName ? '编辑模型' : '添加模型'"
      class="max-w-xl bg-slate-900 border border-slate-800 text-slate-100"
    >
      <n-form :model="form" label-placement="left" label-width="120" class="space-y-2">
        <n-form-item label="公开模型名" required>
          <n-input v-model:value="form.name" :disabled="!!editingName" placeholder="例如: gpt-4o" />
        </n-form-item>
        <n-form-item label="显示名称">
          <n-input v-model:value="form.display_name" placeholder="例如: GPT-4o 旗舰多模态" />
        </n-form-item>
        <n-form-item label="关联供应商" required>
          <n-select v-model:value="form.provider_id" :options="providerOptions" />
        </n-form-item>
        <n-form-item label="上游实际模型名" required>
          <n-input v-model:value="form.provider_model_name" placeholder="例如: gpt-4o-2024-08-06" />
        </n-form-item>
        <div class="grid grid-cols-2 gap-3">
          <n-form-item label="上下文长度">
            <n-input-number v-model:value="form.context_length" :min="1024" :step="1024" />
          </n-form-item>
          <n-form-item label="启用状态">
            <n-switch v-model:value="form.enabled" />
          </n-form-item>
        </div>
        <div class="grid grid-cols-2 gap-3">
          <n-form-item label="输入费率 ($/1M)">
            <n-input-number v-model:value="form.pricing_prompt_1k" :min="0" :precision="4" :step="0.001" />
          </n-form-item>
          <n-form-item label="输出费率 ($/1M)">
            <n-input-number v-model:value="form.pricing_completion_1k" :min="0" :precision="4" :step="0.001" />
          </n-form-item>
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
      </n-form>
      <template #footer>
        <div class="flex justify-end space-x-2">
          <n-button @click="modalVisible = false">取消</n-button>
          <n-button type="primary" :loading="saving" @click="saveModel">保存</n-button>
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

const form = reactive({
  name: '',
  display_name: '',
  provider_id: '',
  provider_model_name: '',
  context_length: 128000,
  pricing_prompt_1k: 0.0025,
  pricing_completion_1k: 0.01,
  enabled: true,
});

const columns = [
  { title: '公开模型标识', key: 'name', width: 160 },
  { title: '显示别名', key: 'display_name', width: 160, ellipsis: { tooltip: true } },
  {
    title: '供应商',
    key: 'provider_id',
    width: 140,
    render(row) {
      return h(NTag, { size: 'small', type: 'info', bordered: false }, { default: () => row.provider_id });
    },
  },
  { title: '上游映射名', key: 'provider_model_name', ellipsis: { tooltip: true } },
  {
    title: '上下文',
    key: 'context_length',
    width: 110,
    render(row) {
      const k = Math.round((row.context_length || 0) / 1024);
      return `${k}K Tokens`;
    },
  },
  {
    title: '启用状态',
    key: 'enabled',
    width: 100,
    render(row) {
      return h(NSwitch, {
        value: row.enabled,
        onUpdateValue: (val) => toggleModelStatus(row, val),
      });
    },
  },
  {
    title: '操作',
    key: 'actions',
    width: 130,
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

async function loadModels() {
  loading.value = true;
  try {
    const [mList, pList] = await Promise.all([
      api.getJson('/admin/v1/models'),
      api.getJson('/admin/v1/providers'),
    ]);
    if (mList && Array.isArray(mList)) {
      models.value = mList;
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
    form.provider_id = row.provider_id || '';
    form.provider_model_name = row.provider_model_name || '';
    form.context_length = row.context_length || 128000;
    form.pricing_prompt_1k = row.pricing_prompt_1k || 0;
    form.pricing_completion_1k = row.pricing_completion_1k || 0;
    form.enabled = row.enabled !== false;
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
  }
  modalVisible.value = true;
}

async function saveModel() {
  if (!form.name || !form.provider_id || !form.provider_model_name) {
    message.warning('请补全必要字段 (模型名、供应商、上游实际模型名)');
    return;
  }

  saving.value = true;
  try {
    let resp;
    if (editingName.value) {
      resp = await api.put(`/admin/v1/models/${editingName.value}`, form);
    } else {
      resp = await api.post('/admin/v1/models', form);
    }
    if (resp && resp.ok) {
      message.success('模型保存成功');
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

onMounted(() => {
  loadModels();
});
</script>
