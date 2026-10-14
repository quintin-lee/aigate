<template>
  <div id="tab-usage" class="space-y-4">
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">用量与请求明细 (Usage & Request History)</h2>
      </div>
      <div class="flex items-center space-x-2">
        <n-button size="small" secondary @click="loadData">刷新</n-button>
      </div>
    </div>

    <n-tabs type="line" animated>
      <!-- Tab 1: Daily Usage Aggregation -->
      <n-tab-pane name="daily" tab="📈 每日用量统计">
        <div class="glass-panel rounded-xl overflow-hidden mt-3">
          <n-data-table
            :columns="dailyColumns"
            :data="dailyRows"
            :loading="loadingDaily"
            :pagination="dailyPagination"
          />
        </div>
      </n-tab-pane>

      <!-- Tab 2: Individual Requests -->
      <n-tab-pane name="requests" tab="📋 请求调用明细">
        <div class="glass-panel rounded-xl overflow-hidden mt-3">
          <n-data-table
            :columns="requestColumns"
            :data="requestRows"
            :loading="loadingRequests"
            :pagination="requestPagination"
          />
        </div>
      </n-tab-pane>
    </n-tabs>
  </div>
</template>

<script setup>
import { ref, h, onMounted, reactive } from 'vue';
import { NButton, NDataTable, NTabs, NTabPane, NTag } from 'naive-ui';
import { api } from '@/api/client';

const loadingDaily = ref(false);
const loadingRequests = ref(false);
const dailyRows = ref([]);
const requestRows = ref([]);

const dailyPagination = reactive({
  page: 1,
  pageSize: 10,
  showSizePicker: true,
  pageSizes: [10, 20, 50],
  onChange: (page) => { dailyPagination.page = page; },
  onUpdatePageSize: (pageSize) => {
    dailyPagination.pageSize = pageSize;
    dailyPagination.page = 1;
  },
});

const requestPagination = reactive({
  page: 1,
  pageSize: 15,
  showSizePicker: true,
  pageSizes: [15, 30, 50],
  onChange: (page) => { requestPagination.page = page; },
  onUpdatePageSize: (pageSize) => {
    requestPagination.pageSize = pageSize;
    requestPagination.page = 1;
  },
});

const dailyColumns = [
  { title: '日期', key: 'day', width: 130 },
  { title: '模型', key: 'model', width: 160 },
  {
    title: '请求次数',
    key: 'requests',
    width: 120,
    render(row) {
      return (row.requests || 0).toLocaleString();
    },
  },
  {
    title: '输入 Tokens',
    key: 'prompt_tokens',
    render(row) {
      return (row.prompt_tokens || 0).toLocaleString();
    },
  },
  {
    title: '输出 Tokens',
    key: 'completion_tokens',
    render(row) {
      return (row.completion_tokens || 0).toLocaleString();
    },
  },
  {
    title: '错误数',
    key: 'errors',
    width: 100,
    render(row) {
      return row.errors > 0
        ? h(NTag, { size: 'small', type: 'error', bordered: false }, { default: () => `${row.errors}` })
        : '0';
    },
  },
];

const requestColumns = [
  { title: '时间', key: 'created_at', width: 170, ellipsis: { tooltip: true } },
  { title: '模型', key: 'model', width: 140 },
  {
    title: '状态码',
    key: 'status_code',
    width: 90,
    render(row) {
      const isErr = row.status_code >= 400;
      return h(
        NTag,
        { size: 'small', type: isErr ? 'error' : 'success', bordered: false },
        { default: () => `${row.status_code || 200}` }
      );
    },
  },
  {
    title: '耗时 (ms)',
    key: 'latency_ms',
    width: 100,
    render(row) {
      return `${row.latency_ms || 0} ms`;
    },
  },
  {
    title: 'Tokens (Prompt / Comp)',
    key: 'tokens',
    width: 180,
    render(row) {
      return `${(row.prompt_tokens || 0).toLocaleString()} / ${(row.completion_tokens || 0).toLocaleString()}`;
    },
  },
  { title: '客户端 IP / 密钥', key: 'key_id', ellipsis: { tooltip: true } },
];

async function loadData() {
  loadDaily();
  loadRequests();
}

async function loadDaily() {
  loadingDaily.value = true;
  try {
    const data = await api.getJson('/admin/v1/usage');
    if (data && Array.isArray(data)) dailyRows.value = data;
  } finally {
    loadingDaily.value = false;
  }
}

async function loadRequests() {
  loadingRequests.value = true;
  try {
    const data = await api.getJson('/admin/v1/usage/requests');
    if (data && Array.isArray(data)) requestRows.value = data;
  } finally {
    loadingRequests.value = false;
  }
}

onMounted(() => {
  loadData();
});
</script>
