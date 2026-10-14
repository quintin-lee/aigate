<template>
  <div id="tab-cost" class="space-y-4">
    <!-- Header Filter Controls -->
    <div class="flex flex-wrap items-center justify-between gap-3">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">成本核算与归属分析 (Cost & Budget Attribution)</h2>
      </div>
      <div class="flex flex-wrap items-center gap-2">
        <n-select v-model:value="byDimension" :options="dimensionOptions" class="w-36" size="small" />
        <n-date-picker v-model:value="dateRange" type="daterange" size="small" clearable />
        <n-button size="small" type="primary" :loading="loading" @click="loadCost">
          查询报表
        </n-button>
      </div>
    </div>

    <!-- Summary KPI Cards -->
    <div class="grid grid-cols-1 sm:grid-cols-3 gap-4">
      <div class="glass-panel p-4 rounded-xl">
        <span class="text-xs text-slate-400">总成本支出 (USD)</span>
        <div class="text-2xl font-bold font-mono text-emerald-400 mt-1">${{ totalCost.toFixed(4) }}</div>
      </div>
      <div class="glass-panel p-4 rounded-xl">
        <span class="text-xs text-slate-400">总计费 Tokens</span>
        <div class="text-2xl font-bold font-mono text-white mt-1">{{ totalTokens.toLocaleString() }}</div>
      </div>
      <div class="glass-panel p-4 rounded-xl">
        <span class="text-xs text-slate-400">核算计费请求量</span>
        <div class="text-2xl font-bold font-mono text-cyan-400 mt-1">{{ totalReqs.toLocaleString() }}</div>
      </div>
    </div>

    <!-- Cost Table -->
    <div class="glass-panel rounded-xl overflow-hidden">
      <n-data-table
        :columns="columns"
        :data="rows"
        :loading="loading"
        :pagination="pagination"
      />
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, reactive } from 'vue';
import { NButton, NDataTable, NSelect, NDatePicker } from 'naive-ui';
import { api } from '@/api/client';

const loading = ref(false);
const byDimension = ref('model');
const rows = ref([]);

const now = Date.now();
const thirtyDaysAgo = now - 30 * 24 * 3600 * 1000;
const dateRange = ref([thirtyDaysAgo, now]);

const dimensionOptions = [
  { label: '按模型归属', value: 'model' },
  { label: '按部门分组', value: 'group' },
  { label: '按客户端密钥', value: 'key' },
  { label: '按模型供应商', value: 'provider' },
];

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

const columns = computed(() => {
  const dimTitle = {
    model: '模型名称',
    group: '部门分组',
    key: 'API 密钥',
    provider: '供应商',
  }[byDimension.value] || '归属维度';

  return [
    { title: dimTitle, key: 'name', width: 200 },
    {
      title: '请求次数',
      key: 'requests',
      width: 130,
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
      title: '费用支出 ($)',
      key: 'cost_usd',
      width: 140,
      render(row) {
        const c = row.cost_usd || 0;
        return `$${c.toFixed(4)}`;
      },
    },
  ];
});

const totalCost = computed(() => {
  return rows.value.reduce((acc, r) => acc + (r.cost_usd || 0), 0);
});

const totalTokens = computed(() => {
  return rows.value.reduce((acc, r) => acc + (r.prompt_tokens || 0) + (r.completion_tokens || 0), 0);
});

const totalReqs = computed(() => {
  return rows.value.reduce((acc, r) => acc + (r.requests || 0), 0);
});

async function loadCost() {
  loading.value = true;
  try {
    let url = `/admin/v1/cost?by=${byDimension.value}`;
    if (dateRange.value && dateRange.value[0] && dateRange.value[1]) {
      const fromStr = new Date(dateRange.value[0]).toISOString().slice(0, 10);
      const toStr = new Date(dateRange.value[1]).toISOString().slice(0, 10);
      url += `&from=${fromStr}&to=${toStr}`;
    }
    const data = await api.getJson(url);
    if (data && Array.isArray(data)) {
      rows.value = data;
    } else {
      rows.value = [];
    }
  } finally {
    loading.value = false;
  }
}

onMounted(() => {
  loadCost();
});
</script>
