<template>
  <div id="tab-overview" class="space-y-6">
    <!-- Top Stat Cards Grid -->
    <div class="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-5 gap-4">
      <div class="glass-panel p-4 rounded-xl relative overflow-hidden group hover:border-slate-700 transition">
        <div class="flex items-center justify-between mb-2">
          <span class="text-xs text-slate-400 font-medium">活跃模型</span>
          <span class="text-xs px-2 py-0.5 rounded-full bg-emerald-500/10 text-emerald-400 border border-emerald-500/20 font-mono">
            共 {{ stats.totalModels }} 个
          </span>
        </div>
        <div class="text-2xl font-bold font-mono text-white">{{ stats.activeModels }}</div>
        <div class="text-[11px] text-slate-400 mt-1">就绪可调用上游</div>
      </div>

      <div class="glass-panel p-4 rounded-xl relative overflow-hidden group hover:border-slate-700 transition">
        <div class="flex items-center justify-between mb-2">
          <span class="text-xs text-slate-400 font-medium">客户端密钥</span>
          <span class="text-xs px-2 py-0.5 rounded-full bg-indigo-500/10 text-indigo-400 border border-indigo-500/20 font-mono">
            {{ stats.activeKeys }} 活跃
          </span>
        </div>
        <div class="text-2xl font-bold font-mono text-white">{{ stats.totalKeys }}</div>
        <div class="text-[11px] text-slate-400 mt-1">已下发凭证总数</div>
      </div>

      <div class="glass-panel p-4 rounded-xl relative overflow-hidden group hover:border-slate-700 transition">
        <div class="flex items-center justify-between mb-2">
          <span class="text-xs text-slate-400 font-medium">总请求数</span>
          <span class="text-xs px-2 py-0.5 rounded-full bg-rose-500/10 text-rose-400 border border-rose-500/20 font-mono">
            {{ stats.totalErrors }} 错误
          </span>
        </div>
        <div class="text-2xl font-bold font-mono text-white">{{ stats.totalRequests.toLocaleString() }}</div>
        <div class="text-[11px] text-slate-400 mt-1">历史处理请求量</div>
      </div>

      <div class="glass-panel p-4 rounded-xl relative overflow-hidden group hover:border-slate-700 transition">
        <div class="flex items-center justify-between mb-2">
          <span class="text-xs text-slate-400 font-medium">总消耗 Token</span>
          <span class="text-xs px-2 py-0.5 rounded-full bg-purple-500/10 text-purple-400 border border-purple-500/20 font-mono">
            全周期
          </span>
        </div>
        <div class="text-2xl font-bold font-mono text-white">{{ stats.totalTokens.toLocaleString() }}</div>
        <div class="text-[11px] text-slate-400 mt-1">{{ stats.promptTokens.toLocaleString() }} 输入 / {{ stats.completionTokens.toLocaleString() }} 输出</div>
      </div>

      <div class="glass-panel p-4 rounded-xl relative overflow-hidden group hover:border-slate-700 transition">
        <div class="flex items-center justify-between mb-2">
          <span class="text-xs text-slate-400 font-medium">思考推理 Token</span>
          <span class="text-xs px-2 py-0.5 rounded-full bg-cyan-500/10 text-cyan-400 border border-cyan-500/20 font-mono">
            Reasoning
          </span>
        </div>
        <div class="text-2xl font-bold font-mono text-cyan-400">{{ stats.reasoningTokens.toLocaleString() }}</div>
        <div class="text-[11px] text-slate-400 mt-1">深度推理模式产出</div>
      </div>
    </div>

    <!-- Chart Panel -->
    <div class="glass-panel p-6 rounded-xl space-y-4">
      <div class="flex items-center justify-between">
        <div class="flex items-center space-x-2">
          <span class="text-base font-semibold text-white">日用量趋势分析</span>
          <span class="text-xs px-2 py-0.5 rounded bg-slate-800 text-slate-400 border border-slate-700">Token 消耗堆叠图</span>
        </div>
        <n-button size="small" secondary @click="loadData">
          刷新
        </n-button>
      </div>

      <div class="h-64 relative w-full">
        <canvas ref="chartCanvas" class="w-full h-full"></canvas>
        <div v-if="noData" class="absolute inset-0 flex items-center justify-center text-slate-500 text-sm">
          暂无足够历史 Token 用量统计数据
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted, onBeforeUnmount } from 'vue';
import { NButton } from 'naive-ui';
import Chart from 'chart.js/auto';
import { api } from '@/api/client';

const chartCanvas = ref(null);
const noData = ref(false);
let chartInstance = null;

const stats = ref({
  totalModels: 0,
  activeModels: 0,
  totalKeys: 0,
  activeKeys: 0,
  totalRequests: 0,
  totalErrors: 0,
  totalTokens: 0,
  promptTokens: 0,
  completionTokens: 0,
  reasoningTokens: 0,
});

async function loadData() {
  const [models, keys, usage, metrics] = await Promise.all([
    api.getJson('/admin/v1/models'),
    api.getJson('/admin/v1/keys'),
    api.getJson('/admin/v1/usage'),
    fetch('/metrics').then(r => r.ok ? r.text() : '').catch(() => ''),
  ]);

  if (models && Array.isArray(models)) {
    stats.value.totalModels = models.length;
    stats.value.activeModels = models.filter(m => m.enabled).length;
  }

  if (keys && Array.isArray(keys)) {
    stats.value.totalKeys = keys.length;
    stats.value.activeKeys = keys.filter(k => !k.revoked).length;
  }

  if (metrics) {
    const match = metrics.match(/aigate_tokens_reasoning_total\s+(\d+)/);
    if (match) {
      stats.value.reasoningTokens = parseInt(match[1], 10) || 0;
    }
  }

  if (usage && Array.isArray(usage)) {
    let reqs = 0, ptok = 0, ctok = 0, errs = 0;
    const dayMap = {};

    usage.forEach(r => {
      reqs += (r.requests || 0);
      ptok += (r.prompt_tokens || 0);
      ctok += (r.completion_tokens || 0);
      errs += (r.errors || 0);

      const d = r.day || '未知';
      if (!dayMap[d]) dayMap[d] = { ptok: 0, ctok: 0 };
      dayMap[d].ptok += (r.prompt_tokens || 0);
      dayMap[d].ctok += (r.completion_tokens || 0);
    });

    stats.value.totalRequests = reqs;
    stats.value.promptTokens = ptok;
    stats.value.completionTokens = ctok;
    stats.value.totalTokens = ptok + ctok;
    stats.value.totalErrors = errs;

    renderChart(dayMap);
  } else {
    noData.value = true;
  }
}

function renderChart(dayMap) {
  if (!chartCanvas.value) return;
  const labels = Object.keys(dayMap).sort();
  if (labels.length === 0) {
    noData.value = true;
    return;
  }
  noData.value = false;

  const ptokData = labels.map(l => dayMap[l].ptok);
  const ctokData = labels.map(l => dayMap[l].ctok);

  if (chartInstance) {
    chartInstance.destroy();
  }

  const ctx = chartCanvas.value.getContext('2d');
  const gradPrompt = ctx.createLinearGradient(0, 0, 0, 256);
  gradPrompt.addColorStop(0, 'rgba(99, 102, 241, 0.95)');
  gradPrompt.addColorStop(1, 'rgba(99, 102, 241, 0.25)');

  const gradComp = ctx.createLinearGradient(0, 0, 0, 256);
  gradComp.addColorStop(0, 'rgba(34, 211, 238, 0.9)');
  gradComp.addColorStop(1, 'rgba(34, 211, 238, 0.2)');

  chartInstance = new Chart(ctx, {
    type: 'bar',
    data: {
      labels,
      datasets: [
        { label: '输入 Token (Prompt)', data: ptokData, backgroundColor: gradPrompt, borderRadius: 6 },
        { label: '输出 Token (Completion)', data: ctokData, backgroundColor: gradComp, borderRadius: 6 },
      ],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      plugins: {
        legend: { labels: { color: '#cbd5e1' } },
      },
      scales: {
        x: { stacked: true, grid: { color: 'rgba(255, 255, 255, 0.05)' }, ticks: { color: '#94a3b8' } },
        y: { stacked: true, grid: { color: 'rgba(255, 255, 255, 0.05)' }, ticks: { color: '#94a3b8' } },
      },
    },
  });
}

onMounted(() => {
  loadData();
});

onBeforeUnmount(() => {
  if (chartInstance) chartInstance.destroy();
});
</script>
