<template>
  <div id="tab-keys" class="space-y-4">
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">客户端 API 密钥 (Client Keys)</h2>
        <span class="text-xs px-2 py-0.5 rounded-full bg-slate-800 text-slate-400 border border-slate-700">
          共 {{ keys.length }} 个密钥
        </span>
      </div>
      <div class="flex items-center space-x-2">
        <n-button size="small" secondary @click="loadKeys">刷新</n-button>
        <n-button size="small" type="primary" @click="openCreateModal">+ 创建 API 密钥</n-button>
      </div>
    </div>

    <!-- Keys Table -->
    <div class="glass-panel rounded-xl overflow-hidden">
      <n-data-table
        :columns="columns"
        :data="keys"
        :loading="loading"
        :pagination="pagination"
      />
    </div>

    <!-- Create Key Modal -->
    <n-modal
      v-model:show="createModalVisible"
      preset="card"
      title="创建新客户端 API 密钥"
      class="max-w-md bg-slate-900 border border-slate-800 text-slate-100"
    >
      <n-form :model="form" label-placement="left" label-width="110" class="space-y-2">
        <n-form-item label="密钥别名" required>
          <n-input v-model:value="form.name" placeholder="例如: 财务分析助手应用" />
        </n-form-item>
        <n-form-item label="所属部门/分组">
          <n-select v-model:value="form.group_id" :options="groupOptions" clearable placeholder="可选" />
        </n-form-item>
        <div class="grid grid-cols-2 gap-3">
          <n-form-item label="分钟限流 (RPM)">
            <n-input-number v-model:value="form.rate_limit_rpm" :min="0" placeholder="0 表示不限" />
          </n-form-item>
          <n-form-item label="日 Token 限额">
            <n-input-number v-model:value="form.rate_limit_tpd" :min="0" placeholder="0 表示不限" />
          </n-form-item>
        </div>
        <div class="grid grid-cols-2 gap-3">
          <n-form-item label="金丝雀灰度密钥">
            <n-switch id="kFormIsCanary" v-model:value="form.is_canary" />
          </n-form-item>
          <n-form-item label="推理配额 Token">
            <n-input-number id="kFormReasoningQuota" v-model:value="form.reasoning_quota" :min="0" placeholder="0 表示不限" />
          </n-form-item>
        </div>
        <n-form-item label="IP 白名单">
          <n-input v-model:value="form.ip_whitelist" placeholder="逗号分隔，如: 192.168.1.0/24, 10.0.0.1" />
        </n-form-item>
      </n-form>
      <template #footer>
        <div class="flex justify-end space-x-2">
          <n-button @click="createModalVisible = false">取消</n-button>
          <n-button type="primary" :loading="creating" @click="createKey">确认生成</n-button>
        </div>
      </template>
    </n-modal>

    <!-- Success Modal showing plaintext key -->
    <n-modal
      v-model:show="successModalVisible"
      preset="card"
      title="🔑 API 密钥生成成功"
      class="max-w-lg bg-slate-900 border border-emerald-500/30 text-slate-100"
    >
      <div class="space-y-4">
        <n-alert type="warning" title="重要提示：该私钥仅展示一次" :bordered="false">
          出于最高安全标准，网关不存储明文凭证。请立即复制并妥善保管在环境变量或密钥库中。
        </n-alert>
        <div class="p-3 bg-slate-950 rounded-lg border border-slate-800 font-mono text-sm break-all text-emerald-400 select-all">
          {{ newlyCreatedKey }}
        </div>
      </div>
      <template #footer>
        <div class="flex justify-end space-x-2">
          <n-button type="primary" @click="copyKey">一键复制并关闭</n-button>
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
  NAlert,
  NPopconfirm,
  useMessage,
} from 'naive-ui';
import { api } from '@/api/client';

const message = useMessage();
const loading = ref(false);
const creating = ref(false);
const keys = ref([]);
const groupOptions = ref([]);

const createModalVisible = ref(false);
const successModalVisible = ref(false);
const newlyCreatedKey = ref('');

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
  group_id: null,
  rate_limit_rpm: 60,
  rate_limit_tpd: 1000000,
  ip_whitelist: '',
});

const columns = [
  { title: '密钥标识 / 前缀', key: 'key_prefix', width: 170, ellipsis: { tooltip: true } },
  { title: '别名 / 描述', key: 'name', width: 160 },
  {
    title: '所属部门',
    key: 'group_id',
    width: 130,
    render(row) {
      return row.group_id
        ? h(NTag, { size: 'small', type: 'info', bordered: false }, { default: () => row.group_id })
        : h('span', { class: 'text-slate-500' }, '默认');
    },
  },
  {
    title: '限流规则',
    key: 'limits',
    width: 170,
    render(row) {
      const rpm = row.rate_limit_rpm ? `${row.rate_limit_rpm} RPM` : '无限制';
      const tpd = row.rate_limit_tpd ? `${(row.rate_limit_tpd / 1000).toFixed(0)}k TPD` : '无限制';
      return `${rpm} / ${tpd}`;
    },
  },
  {
    title: '状态',
    key: 'revoked',
    width: 100,
    render(row) {
      return row.revoked
        ? h(NTag, { size: 'small', type: 'error', bordered: false }, { default: () => '已吊销' })
        : h(NTag, { size: 'small', type: 'success', bordered: false }, { default: () => '活跃正常' });
    },
  },
  {
    title: '操作',
    key: 'actions',
    width: 110,
    render(row) {
      if (row.revoked) return h('span', { class: 'text-slate-600 text-xs' }, '已失效');
      return h(
        NPopconfirm,
        {
          onPositiveClick: () => revokeKey(row.id),
        },
        {
          trigger: () =>
            h(NButton, { size: 'tiny', type: 'error', secondary: true }, { default: () => '吊销密钥' }),
          default: () => `确认立即吊销密钥 ${row.name || row.id} 吗？吊销后调用将立即拦截。`,
        }
      );
    },
  },
];

async function loadKeys() {
  loading.value = true;
  try {
    const [kList, gList] = await Promise.all([
      api.getJson('/admin/v1/keys'),
      api.getJson('/admin/v1/groups'),
    ]);
    if (kList && Array.isArray(kList)) {
      keys.value = kList;
    }
    if (gList && Array.isArray(gList)) {
      groupOptions.value = gList.map(g => ({ label: `${g.name} (${g.id})`, value: g.id }));
    }
  } finally {
    loading.value = false;
  }
}

function openCreateModal() {
  form.name = '';
  form.group_id = null;
  form.rate_limit_rpm = 60;
  form.rate_limit_tpd = 1000000;
  form.ip_whitelist = '';
  createModalVisible.value = true;
}

async function createKey() {
  if (!form.name) {
    message.warning('请输入密钥别名');
    return;
  }
  creating.value = true;
  try {
    const resp = await api.postJson('/admin/v1/keys', form);
    if (resp && resp.key) {
      newlyCreatedKey.value = resp.key;
      createModalVisible.value = false;
      successModalVisible.value = true;
      loadKeys();
    } else {
      message.error('创建密钥失败');
    }
  } finally {
    creating.value = false;
  }
}

function copyKey() {
  if (navigator.clipboard && newlyCreatedKey.value) {
    navigator.clipboard.writeText(newlyCreatedKey.value);
    message.success('密钥已复制到剪贴板！');
  }
  successModalVisible.value = false;
}

async function revokeKey(id) {
  const resp = await api.delete(`/admin/v1/keys/${id}`);
  if (resp && resp.ok) {
    message.success('密钥已被吊销');
    loadKeys();
  } else {
    message.error('吊销密钥失败');
  }
}

onMounted(() => {
  loadKeys();
});
</script>
