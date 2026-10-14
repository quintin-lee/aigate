<template>
  <div id="tab-groups" class="space-y-4">
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">部门与业务分组 (Department Groups & Quotas)</h2>
        <span class="text-xs px-2 py-0.5 rounded-full bg-slate-800 text-slate-400 border border-slate-700">
          共 {{ groups.length }} 个分组
        </span>
      </div>
      <div class="flex items-center space-x-2">
        <n-button size="small" secondary @click="loadGroups">刷新</n-button>
        <n-button size="small" type="primary" @click="openModal()">+ 添加部门分组</n-button>
      </div>
    </div>

    <!-- Groups Table -->
    <div class="glass-panel rounded-xl overflow-hidden">
      <n-data-table
        :columns="columns"
        :data="groups"
        :loading="loading"
        :pagination="pagination"
      />
    </div>

    <!-- Group Modal -->
    <n-modal
      v-model:show="modalVisible"
      preset="card"
      :title="editingId ? '编辑部门分组' : '新建部门分组'"
      class="max-w-md bg-slate-900 border border-slate-800 text-slate-100"
    >
      <n-form :model="form" label-placement="left" label-width="110" class="space-y-2">
        <n-form-item label="分组标识 (ID)" required>
          <n-input v-model:value="form.id" :disabled="!!editingId" placeholder="例如: dept-rnd" />
        </n-form-item>
        <n-form-item label="部门名称" required>
          <n-input v-model:value="form.name" placeholder="例如: 研发中心" />
        </n-form-item>
        <n-form-item label="月度预算 ($)">
          <n-input-number v-model:value="form.budget_limit_usd" :min="0" :precision="2" :step="100" />
        </n-form-item>
        <div class="grid grid-cols-2 gap-3">
          <n-form-item label="限流倍率">
            <n-input-number v-model:value="form.rate_limit_multiplier" :min="0.1" :max="10" :step="0.5" />
          </n-form-item>
          <n-form-item label="日推理限额">
            <n-input-number id="gFormDailyReasoningQuota" v-model:value="form.daily_reasoning_quota" :min="0" />
          </n-form-item>
        </div>
      </n-form>
      <template #footer>
        <div class="flex justify-end space-x-2">
          <n-button @click="modalVisible = false">取消</n-button>
          <n-button type="primary" :loading="saving" @click="saveGroup">保存</n-button>
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
  NPopconfirm,
  useMessage,
} from 'naive-ui';
import { api } from '@/api/client';

const message = useMessage();
const loading = ref(false);
const saving = ref(false);
const groups = ref([]);
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

const form = reactive({
  id: '',
  name: '',
  budget_limit_usd: 1000,
  rate_limit_multiplier: 1.0,
});

const columns = [
  { title: '分组 ID', key: 'id', width: 150 },
  { title: '部门名称', key: 'name', width: 180 },
  {
    title: '月度预算限额',
    key: 'budget_limit_usd',
    width: 140,
    render(row) {
      return `$${(row.budget_limit_usd || 0).toLocaleString()}`;
    },
  },
  {
    title: '限流倍率',
    key: 'rate_limit_multiplier',
    width: 110,
    render(row) {
      return `${row.rate_limit_multiplier || 1.0}x`;
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
            onPositiveClick: () => deleteGroup(row.id),
          },
          {
            trigger: () =>
              h(NButton, { size: 'tiny', type: 'error', secondary: true }, { default: () => '删除' }),
            default: () => `确定删除部门分组 ${row.name || row.id} 吗？`,
          }
        ),
      ]);
    },
  },
];

async function loadGroups() {
  loading.value = true;
  try {
    const data = await api.getJson('/admin/v1/groups');
    if (data && Array.isArray(data)) {
      groups.value = data;
    }
  } finally {
    loading.value = false;
  }
}

function openModal(row = null) {
  if (row) {
    editingId.value = row.id;
    form.id = row.id;
    form.name = row.name;
    form.budget_limit_usd = row.budget_limit_usd || 1000;
    form.rate_limit_multiplier = row.rate_limit_multiplier || 1.0;
  } else {
    editingId.value = null;
    form.id = '';
    form.name = '';
    form.budget_limit_usd = 1000;
    form.rate_limit_multiplier = 1.0;
  }
  modalVisible.value = true;
}

async function saveGroup() {
  if (!form.id || !form.name) {
    message.warning('请填写分组 ID 和名称');
    return;
  }
  saving.value = true;
  try {
    let resp;
    if (editingId.value) {
      resp = await api.put(`/admin/v1/groups/${editingId.value}`, form);
    } else {
      resp = await api.post('/admin/v1/groups', form);
    }
    if (resp && resp.ok) {
      message.success('分组保存成功');
      modalVisible.value = false;
      loadGroups();
    } else {
      message.error('保存失败');
    }
  } finally {
    saving.value = false;
  }
}

async function deleteGroup(id) {
  const resp = await api.delete(`/admin/v1/groups/${id}`);
  if (resp && resp.ok) {
    message.success('分组已成功删除');
    loadGroups();
  } else {
    message.error('删除分组失败');
  }
}

onMounted(() => {
  loadGroups();
});
</script>
