<template>
  <div class="space-y-4">
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">响应语义缓存与优化 (Response Semantic Cache)</h2>
      </div>
      <div class="flex items-center space-x-2">
        <n-button size="small" secondary @click="loadStats">刷新统计</n-button>
        <n-popconfirm @positive-click="purgeCache">
          <template #trigger>
            <n-button size="small" type="error" :loading="purging">🗑️ 一键清空全量缓存</n-button>
          </template>
          确认清空网关 Redis/内存语义缓存吗？清空后上游调用量将短期增加。
        </n-popconfirm>
      </div>
    </div>

    <!-- Stat Cards -->
    <div class="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-4 gap-4">
      <div class="glass-panel p-5 rounded-xl">
        <span class="text-xs text-slate-400 font-medium">缓存命中率 (Hit Ratio)</span>
        <div class="text-3xl font-bold font-mono text-emerald-400 mt-2">{{ hitRatio }}%</div>
        <n-progress
          type="line"
          :percentage="parseFloat(hitRatio)"
          status="success"
          :show-indicator="false"
          class="mt-3"
        />
      </div>

      <div class="glass-panel p-5 rounded-xl">
        <span class="text-xs text-slate-400 font-medium">累计命中次数 (Hits)</span>
        <div class="text-3xl font-bold font-mono text-white mt-2">{{ stats.hits.toLocaleString() }}</div>
        <div class="text-xs text-emerald-400 mt-2">直接拦截短路返回</div>
      </div>

      <div class="glass-panel p-5 rounded-xl">
        <span class="text-xs text-slate-400 font-medium">穿透请求数 (Misses)</span>
        <div class="text-3xl font-bold font-mono text-slate-300 mt-2">{{ stats.misses.toLocaleString() }}</div>
        <div class="text-xs text-slate-400 mt-2">真实转发至上游大模型</div>
      </div>

      <div class="glass-panel p-5 rounded-xl">
        <span class="text-xs text-slate-400 font-medium">缓存键条目 (Keys)</span>
        <div class="text-3xl font-bold font-mono text-cyan-400 mt-2">{{ stats.cached_keys.toLocaleString() }}</div>
        <div class="text-xs text-slate-400 mt-2">有效有效期内存储条目</div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue';
import { NButton, NProgress, NPopconfirm, useMessage } from 'naive-ui';
import { api } from '@/api/client';

const message = useMessage();
const purging = ref(false);

const stats = ref({
  hits: 0,
  misses: 0,
  cached_keys: 0,
});

const hitRatio = computed(() => {
  const total = stats.value.hits + stats.value.misses;
  if (total === 0) return '0.0';
  return ((stats.value.hits / total) * 100).toFixed(1);
});

async function loadStats() {
  const data = await api.getJson('/admin/v1/cache/stats');
  if (data) {
    stats.value.hits = data.hits || data.cache_hits || 0;
    stats.value.misses = data.misses || data.cache_misses || 0;
    stats.value.cached_keys = data.cached_keys || data.keys_count || 0;
  }
}

async function purgeCache() {
  purging.value = true;
  try {
    const resp = await api.post('/admin/v1/cache/purge');
    if (resp && resp.ok) {
      message.success('响应语义缓存已成功清空！');
      loadStats();
    } else {
      message.error('清空缓存失败');
    }
  } finally {
    purging.value = false;
  }
}

onMounted(() => {
  loadStats();
});
</script>
