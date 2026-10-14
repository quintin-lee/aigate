<template>
  <div class="space-y-4">
    <!-- Header with Pause/Resume -->
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">实时流量运维大屏 (Live Traffic Operations)</h2>
        <span class="flex items-center space-x-1.5 px-2.5 py-0.5 rounded-full text-xs font-mono bg-emerald-500/10 text-emerald-400 border border-emerald-500/20">
          <span class="w-2 h-2 rounded-full bg-emerald-400 animate-ping"></span>
          <span>SSE LIVE STREAMING</span>
        </span>
      </div>
      <div class="flex items-center space-x-2">
        <n-button size="small" :type="paused ? 'primary' : 'warning'" secondary @click="togglePause">
          {{ paused ? '▶️ 恢复实时流' : '⏸️ 暂停推送' }}
        </n-button>
        <n-button size="small" secondary @click="clearEvents">清屏</n-button>
      </div>
    </div>

    <!-- Live Waterfall & Event Stream -->
    <div class="grid grid-cols-1 lg:grid-cols-12 gap-6">
      <!-- Live Request Waterfall -->
      <div class="lg:col-span-8 glass-panel p-5 rounded-xl flex flex-col h-[650px]">
        <div class="flex items-center justify-between pb-3 border-b border-slate-800">
          <span class="text-sm font-semibold text-white">实时请求流水瀑布 (Request Waterfall)</span>
          <span class="text-xs text-slate-400 font-mono">接收 {{ liveRequests.length }} 条事件</span>
        </div>

        <div class="flex-1 overflow-y-auto space-y-2.5 mt-3 pr-2 custom-scroll">
          <div v-if="liveRequests.length === 0" class="h-full flex items-center justify-center text-slate-500 text-sm">
            等待网关实时流量推送中 (GET /admin/v1/events)...
          </div>

          <div
            v-for="(req, idx) in liveRequests"
            :key="idx"
            class="p-3 rounded-lg border border-slate-800 bg-slate-900/60 hover:bg-slate-800/50 transition flex items-center justify-between text-xs font-mono"
          >
            <div class="flex items-center space-x-3">
              <span
                :class="[
                  'px-2 py-0.5 rounded font-bold text-[10px]',
                  req.status < 400 ? 'bg-emerald-500/10 text-emerald-400 border border-emerald-500/20' : 'bg-rose-500/10 text-rose-400 border border-rose-500/20'
                ]"
              >
                {{ req.status || 200 }}
              </span>
              <span class="text-white font-semibold">{{ req.model || 'gpt-4o' }}</span>
              <span class="text-slate-400 text-[11px]">{{ req.client_ip || '127.0.0.1' }}</span>
            </div>

            <div class="flex items-center space-x-4 text-slate-400">
              <span>⏱️ {{ req.latency_ms || 120 }}ms</span>
              <span>🪙 {{ req.total_tokens || 0 }} tok</span>
              <span class="text-slate-500 text-[10px]">{{ req.time || '刚刚' }}</span>
            </div>
          </div>
        </div>
      </div>

      <!-- Real-time Alerts Panel -->
      <div class="lg:col-span-4 glass-panel p-5 rounded-xl flex flex-col h-[650px]">
        <div class="flex items-center justify-between pb-3 border-b border-slate-800">
          <span class="text-sm font-semibold text-rose-400">🚨 实时告警与拦截</span>
          <span class="text-xs text-rose-400 font-mono">{{ liveAlerts.length }} 次阻断</span>
        </div>

        <div class="flex-1 overflow-y-auto space-y-2 mt-3 pr-2 custom-scroll">
          <div v-if="liveAlerts.length === 0" class="h-full flex items-center justify-center text-slate-500 text-xs">
            当前未检测到异常攻击或安全规则触发
          </div>

          <div
            v-for="(alt, idx) in liveAlerts"
            :key="idx"
            class="p-3 rounded-lg border border-rose-500/30 bg-rose-950/20 text-xs space-y-1"
          >
            <div class="flex justify-between items-center text-rose-300 font-semibold">
              <span>{{ alt.threat_type || '安全策略拦截' }}</span>
              <span class="text-[10px] text-slate-400 font-mono">{{ alt.time || '刚刚' }}</span>
            </div>
            <p class="text-[11px] text-slate-300 font-mono truncate">来源: {{ alt.client_ip || '未知 IP' }}</p>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted, onBeforeUnmount } from 'vue';
import { NButton } from 'naive-ui';
import { state } from '@/store/appStore';

const paused = ref(false);
const liveRequests = ref([]);
const liveAlerts = ref([]);
let eventSource = null;

function connectSSE() {
  if (eventSource) {
    eventSource.close();
  }

  const url = `/admin/v1/events?token=${encodeURIComponent(state.adminToken || '')}`;
  try {
    eventSource = new EventSource(url);
    eventSource.onmessage = (e) => {
      if (paused.value) return;
      try {
        const data = JSON.parse(e.data);
        const nowStr = new Date().toLocaleTimeString();
        data.time = nowStr;

        if (data.type === 'alert' || data.status >= 400) {
          liveAlerts.value.unshift(data);
          if (liveAlerts.value.length > 50) liveAlerts.value.pop();
        }

        liveRequests.value.unshift(data);
        if (liveRequests.value.length > 100) liveRequests.value.pop();
      } catch {}
    };

    eventSource.onerror = () => {
      // Reconnect handled automatically by browser EventSource
    };
  } catch (err) {
    console.error('SSE connection error:', err);
  }
}

function togglePause() {
  paused.value = !paused.value;
}

function clearEvents() {
  liveRequests.value = [];
  liveAlerts.value = [];
}

onMounted(() => {
  connectSSE();
});

onBeforeUnmount(() => {
  if (eventSource) {
    eventSource.close();
  }
});
</script>
