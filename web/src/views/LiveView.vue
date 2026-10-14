<template>
  <div class="space-y-4">
    <!-- Header with Live Pulse and Controls -->
    <div class="flex flex-wrap items-center justify-between gap-3">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">实时流量运维大屏 (Live Traffic Operations)</h2>
        <span
          class="flex items-center space-x-1.5 px-2.5 py-0.5 rounded-full text-xs font-mono transition"
          :class="sseConnected ? 'bg-emerald-500/10 text-emerald-400 border border-emerald-500/20' : 'bg-amber-500/10 text-amber-400 border border-amber-500/20'"
        >
          <span
            class="w-2 h-2 rounded-full inline-block"
            :class="sseConnected ? 'bg-emerald-400 animate-ping' : 'bg-amber-400'"
          ></span>
          <span>{{ sseConnected ? 'SSE LIVE STREAMING' : 'POLLING FALLBACK ACTIVE' }}</span>
        </span>
      </div>
      <div class="flex items-center space-x-2">
        <n-button
          size="small"
          :type="paused ? 'primary' : 'warning'"
          secondary
          @click="togglePause"
        >
          {{ paused ? '▶️ 恢复实时流' : '⏸️ 暂停推送' }}
        </n-button>
        <n-button size="small" secondary @click="clearEvents">
          🧹 清屏
        </n-button>
      </div>
    </div>

    <!-- Top Traffic & Performance Radar Cards -->
    <div class="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-4 gap-4">
      <div class="glass-panel p-4 rounded-xl relative overflow-hidden group hover:border-slate-700 transition">
        <div class="flex items-center justify-between mb-2">
          <span class="text-xs text-slate-400 font-medium">实时网关吞吐 (QPS)</span>
          <span class="text-xs px-2 py-0.5 rounded-full bg-sky-500/10 text-sky-400 border border-sky-500/20 font-mono">
            窗口 5s
          </span>
        </div>
        <div class="text-2xl font-bold font-mono text-sky-400">
          {{ currentQps.toFixed(1) }} <span class="text-xs font-normal text-slate-400">req/s</span>
        </div>
        <div class="text-[11px] text-slate-400 mt-1">
          当前队列保留 {{ liveRequests.length }} 条最近流水
        </div>
      </div>

      <div class="glass-panel p-4 rounded-xl relative overflow-hidden group hover:border-slate-700 transition">
        <div class="flex items-center justify-between mb-2">
          <span class="text-xs text-slate-400 font-medium">P90 / P99 响应延迟</span>
          <span class="text-xs px-2 py-0.5 rounded-full bg-emerald-500/10 text-emerald-400 border border-emerald-500/20 font-mono">
            滑动分位
          </span>
        </div>
        <div class="text-2xl font-bold font-mono text-emerald-400">
          {{ p90Latency }}ms <span class="text-sm font-normal text-slate-400">/ {{ p99Latency }}ms</span>
        </div>
        <div class="text-[11px] text-slate-400 mt-1">
          平均端到端时延 {{ avgLatency }}ms
        </div>
      </div>

      <div class="glass-panel p-4 rounded-xl relative overflow-hidden group hover:border-slate-700 transition">
        <div class="flex items-center justify-between mb-2">
          <span class="text-xs text-slate-400 font-medium">异常与阻断率</span>
          <span class="text-xs px-2 py-0.5 rounded-full bg-rose-500/10 text-rose-400 border border-rose-500/20 font-mono">
            4xx / 5xx
          </span>
        </div>
        <div class="text-2xl font-bold font-mono text-rose-400">
          {{ errorRate.toFixed(1) }}%
        </div>
        <div class="text-[11px] text-slate-400 mt-1">
          策略拦截 {{ blockedCount }} 次 · 上游故障 {{ upstreamErrorCount }} 次
        </div>
      </div>

      <div class="glass-panel p-4 rounded-xl relative overflow-hidden group hover:border-slate-700 transition">
        <div class="flex items-center justify-between mb-2">
          <span class="text-xs text-slate-400 font-medium">Token 吞吐速率</span>
          <span class="text-xs px-2 py-0.5 rounded-full bg-purple-500/10 text-purple-400 border border-purple-500/20 font-mono">
            Token Flow
          </span>
        </div>
        <div class="text-2xl font-bold font-mono text-purple-400">
          {{ tokenRate }} <span class="text-xs font-normal text-slate-400">tok/s</span>
        </div>
        <div class="text-[11px] text-slate-400 mt-1">
          全网关瞬时推理生成流动速率
        </div>
      </div>
    </div>

    <!-- Upstream Providers Health & Circuit Breaker Strip -->
    <div class="glass-panel p-3.5 rounded-xl space-y-2">
      <div class="flex items-center justify-between">
        <div class="flex items-center space-x-2">
          <span class="text-xs font-semibold text-white">🌐 上游渠道健康与熔断状态机 (Providers & Circuit Breaker)</span>
          <span v-if="selectedProvider !== 'all'" class="text-[10px] text-sky-400 font-mono">
            [已锁定过滤: {{ selectedProvider }}]
          </span>
        </div>
        <button
          v-if="selectedProvider !== 'all'"
          type="button"
          class="text-xs text-sky-400 hover:text-sky-300 transition"
          @click="selectedProvider = 'all'"
        >
          重置渠道过滤
        </button>
        <span v-else class="text-xs text-slate-500">点击卡片可快速筛选指定渠道流量</span>
      </div>

      <div class="flex items-center space-x-3 overflow-x-auto py-1 scrollbar-thin">
        <div
          v-for="prov in providerHealthList"
          :key="prov.id"
          class="shrink-0 p-2.5 rounded-lg border transition cursor-pointer select-none min-w-[170px]"
          :class="selectedProvider === prov.id ? 'bg-sky-500/15 border-sky-400' : (prov.cbState === 'DEGRADED' ? 'bg-amber-500/10 border-amber-500/40 hover:border-amber-400' : (prov.cbState === 'OPEN' ? 'bg-rose-500/10 border-rose-500/40 hover:border-rose-400' : 'bg-slate-900 border-slate-800 hover:border-slate-700'))"
          @click="toggleProviderFilter(prov.id)"
        >
          <div class="flex items-center justify-between mb-1">
            <span class="font-bold text-xs" :class="selectedProvider === prov.id ? 'text-sky-300' : 'text-slate-200'">
              {{ prov.name }}
            </span>
            <span
              class="text-[9px] px-1.5 py-0.2 rounded font-mono font-bold"
              :class="prov.cbState === 'DEGRADED' ? 'bg-amber-500/20 text-amber-300' : (prov.cbState === 'OPEN' ? 'bg-rose-500/20 text-rose-300' : (prov.isCache ? 'bg-sky-500/20 text-sky-300' : 'bg-emerald-500/20 text-emerald-300'))"
            >
              {{ prov.cbState }}
            </span>
          </div>
          <div class="flex items-center justify-between text-[10px] text-slate-400 font-mono">
            <span>探活时延: <strong class="text-slate-200">{{ prov.latency_ms }}ms</strong></span>
            <span>成功率: {{ prov.successRate }}%</span>
          </div>
          <div class="text-[9px] text-slate-500 truncate mt-1">
            累计 {{ prov.totalReqs }} 次请求 · {{ prov.statusText }}
          </div>
        </div>
      </div>
    </div>

    <!-- Main Live Streams Grid (8 cols waterfall : 4 cols alerts) -->
    <div class="grid grid-cols-1 lg:grid-cols-12 gap-5">
      <!-- Left (8 cols): Request Waterfall -->
      <div class="lg:col-span-8 glass-panel p-4 rounded-xl flex flex-col h-[650px]">
        <!-- Waterfall Filter Toolbar -->
        <div class="flex flex-wrap items-center justify-between gap-2 pb-3 border-b border-slate-800">
          <div class="flex items-center space-x-2">
            <span class="text-sm font-semibold text-white">实时请求流水瀑布 (Request Waterfall)</span>
            <span class="text-xs text-slate-400 font-mono">({{ filteredRequests.length }} / {{ liveRequests.length }})</span>
          </div>

          <div class="flex items-center space-x-2">
            <div class="flex rounded-lg border border-slate-800 bg-slate-900/80 p-0.5 text-[11px]">
              <button
                type="button"
                class="px-2 py-0.5 rounded transition"
                :class="statusFilter === 'all' ? 'bg-sky-500/20 text-sky-300 font-bold' : 'text-slate-400 hover:text-slate-200'"
                @click="statusFilter = 'all'"
              >
                全部
              </button>
              <button
                type="button"
                class="px-2 py-0.5 rounded transition"
                :class="statusFilter === '2xx' ? 'bg-emerald-500/20 text-emerald-300 font-bold' : 'text-slate-400 hover:text-slate-200'"
                @click="statusFilter = '2xx'"
              >
                2xx 成功
              </button>
              <button
                type="button"
                class="px-2 py-0.5 rounded transition"
                :class="statusFilter === 'err' ? 'bg-rose-500/20 text-rose-300 font-bold' : 'text-slate-400 hover:text-slate-200'"
                @click="statusFilter = 'err'"
              >
                4xx/5xx 异常
              </button>
              <button
                type="button"
                class="px-2 py-0.5 rounded transition"
                :class="statusFilter === 'cache' ? 'bg-sky-500/20 text-sky-300 font-bold' : 'text-slate-400 hover:text-slate-200'"
                @click="statusFilter = 'cache'"
              >
                ⚡ 缓存命中
              </button>
            </div>

            <button
              type="button"
              class="px-2.5 py-1 rounded text-xs border transition"
              :class="autoScroll ? 'bg-slate-800 border-slate-700 text-slate-200' : 'bg-amber-500/10 border-amber-500/30 text-amber-300'"
              @click="autoScroll = !autoScroll"
            >
              {{ autoScroll ? '🔒 滚屏锁定' : '🔓 自由停靠' }}
            </button>
          </div>
        </div>

        <!-- Waterfall Flow Container -->
        <div
          ref="waterfallContainerRef"
          class="flex-1 overflow-y-auto space-y-2 mt-3 pr-1.5 custom-scroll"
        >
          <div
            v-if="filteredRequests.length === 0"
            class="h-full flex flex-col items-center justify-center text-slate-500 text-xs space-y-2"
          >
            <span class="text-2xl">⏳</span>
            <span>等待网关实时流量推送中 (SSE /admin/v1/events 或轮询回退)...</span>
          </div>

          <div
            v-for="(req, idx) in filteredRequests"
            :key="req.trace_id || idx"
            class="p-2.5 rounded-lg border transition flex flex-wrap sm:flex-nowrap items-center justify-between text-xs font-mono cursor-pointer select-none"
            :class="req.status >= 400 ? 'bg-rose-950/20 border-rose-500/30 hover:bg-rose-950/30' : (req.cached ? 'bg-sky-950/15 border-sky-500/30 hover:bg-sky-950/25' : 'bg-slate-900/60 border-slate-800 hover:bg-slate-800/50')"
            @click="openRequestTrace(req)"
          >
            <!-- Left Info -->
            <div class="flex items-center space-x-2.5 truncate mr-2">
              <span
                class="px-2 py-0.5 rounded font-bold text-[10px] shrink-0"
                :class="req.status < 400 ? (req.cached ? 'bg-sky-500/20 text-sky-300 border border-sky-500/30' : 'bg-emerald-500/20 text-emerald-400 border border-emerald-500/30') : 'bg-rose-500/20 text-rose-400 border border-rose-500/30'"
              >
                {{ req.cached ? '200 CACHED' : (req.status || 200) }}
              </span>

              <span class="text-white font-semibold shrink-0">{{ req.model || 'gpt-4o' }}</span>
              <span class="text-slate-500 text-[10px] shrink-0">➔ {{ req.provider || 'openai' }}</span>
              <span class="text-slate-400 text-[11px] truncate">{{ req.client_ip || '127.0.0.1' }}</span>

              <span
                v-if="req.guardrail && req.guardrail !== 'none' && req.guardrail !== 'pass'"
                class="px-1.5 py-0.2 rounded text-[10px] bg-rose-500/20 text-rose-300 border border-rose-500/30 shrink-0"
              >
                ⛔ [{{ req.guardrail }}]
              </span>
            </div>

            <!-- Right Metrics -->
            <div class="flex items-center space-x-3 text-slate-400 shrink-0 mt-1 sm:mt-0 text-[11px]">
              <span :class="getLatencyColor(req.latency_ms || 0)">
                ⏱️ {{ req.latency_ms || 12 }}ms
              </span>
              <span>🪙 {{ req.total_tokens || ((req.prompt_tokens || 0) + (req.completion_tokens || 0)) }} tok</span>
              <span v-if="req.cost" class="text-indigo-300 text-[10px]">${{ Number(req.cost).toFixed(5) }}</span>
              <span class="text-slate-500 text-[10px]">{{ req.time || '刚刚' }}</span>
            </div>
          </div>
        </div>
      </div>

      <!-- Right (4 cols): Alerts, Breakers, Health Probes -->
      <div class="lg:col-span-4 glass-panel p-4 rounded-xl flex flex-col h-[650px]">
        <!-- Right Header -->
        <div class="flex items-center justify-between pb-3 border-b border-slate-800">
          <div class="flex items-center space-x-2">
            <span class="text-sm font-semibold text-rose-400">🚨 熔断自愈与安全告警流</span>
            <span class="text-xs text-rose-400 font-mono">{{ liveAlerts.length }} 条</span>
          </div>
          <button
            type="button"
            class="text-[11px] text-slate-400 hover:text-slate-200 transition"
            @click="liveAlerts = []"
          >
            清空告警
          </button>
        </div>

        <!-- Alerts Stream -->
        <div class="flex-1 overflow-y-auto space-y-2.5 mt-3 pr-1 custom-scroll">
          <div
            v-if="liveAlerts.length === 0"
            class="h-full flex flex-col items-center justify-center text-slate-500 text-xs space-y-2"
          >
            <span class="text-xl">🛡️</span>
            <span>当前网关运行平稳，未检测到异常攻击或熔断降级。</span>
          </div>

          <div
            v-for="(alt, idx) in liveAlerts"
            :key="idx"
            class="p-3 rounded-lg border text-xs space-y-1.5 transition"
            :class="alt.type === 'circuit_breaker' ? 'bg-amber-950/20 border-amber-500/30 text-amber-200' : (alt.type === 'health_probe' ? 'bg-sky-950/20 border-sky-500/30 text-sky-200' : 'bg-rose-950/25 border-rose-500/40 text-rose-200')"
          >
            <!-- Alert Title -->
            <div class="flex justify-between items-center font-semibold">
              <span class="flex items-center space-x-1">
                <span v-if="alt.type === 'circuit_breaker'">⚡ 熔断状态机流转</span>
                <span v-else-if="alt.type === 'health_probe'">💓 上游探活心跳</span>
                <span v-else-if="alt.type === 'budget_alert'">💰 预算超额预警</span>
                <span v-else>⛔ {{ alt.threat_type || alt.guardrail || '安全风控策略阻断' }}</span>
              </span>
              <span class="text-[10px] text-slate-400 font-mono">{{ alt.time || '刚刚' }}</span>
            </div>

            <!-- Content Detail -->
            <div v-if="alt.type === 'circuit_breaker'" class="font-mono text-[11px] text-slate-300">
              <div>{{ alt.provider }}: {{ alt.old_state }} ➔ <strong class="text-amber-300">{{ alt.new_state }}</strong></div>
              <div class="text-[10px] text-slate-400 mt-0.5">原因: {{ alt.reason || 'SLA 触发阈值' }}</div>
            </div>

            <div v-else-if="alt.type === 'health_probe'" class="font-mono text-[11px] text-slate-300">
              <div>{{ alt.provider }}: 状态 <strong>{{ alt.status }}</strong> · 延迟 {{ alt.latency_ms }}ms</div>
            </div>

            <div v-else class="font-mono text-[11px] text-slate-300 space-y-0.5">
              <div>来源: {{ alt.client_ip || '127.0.0.1' }} (模型: {{ alt.model || 'gpt-4o' }})</div>
              <div v-if="alt.reason || alt.rule_tag" class="text-rose-300 text-[10px]">
                触发规则: {{ alt.reason || alt.rule_tag }}
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- Request Trace Diagnostic Drawer -->
    <n-drawer v-model:show="traceDrawerVisible" width="560" placement="right">
      <n-drawer-content
        :title="`🔎 实时请求全链路诊断 · Trace ID: ${selectedTrace?.trace_id || 'N/A'}`"
        closable
        class="bg-slate-900 text-slate-100"
      >
        <div v-if="selectedTrace" class="space-y-4 text-xs font-mono">
          <!-- Status Banner -->
          <div
            class="p-3.5 rounded-xl border flex items-center justify-between"
            :class="selectedTrace.status >= 400 ? 'bg-rose-500/10 border-rose-500/30' : 'bg-emerald-500/10 border-emerald-500/30'"
          >
            <div>
              <div class="text-sm font-bold" :class="selectedTrace.status >= 400 ? 'text-rose-400' : 'text-emerald-400'">
                HTTP {{ selectedTrace.status || 200 }} {{ selectedTrace.cached ? '(Cached Hit)' : '' }}
              </div>
              <div class="text-slate-400 text-[11px] mt-0.5">
                {{ selectedTrace.status < 400 ? '请求已正常通过网关并完成响应' : '请求触发安全风控熔断或上游异常' }}
              </div>
            </div>
            <div class="text-right">
              <span class="text-xs px-2 py-0.5 rounded-full font-bold" :class="selectedTrace.status < 400 ? 'bg-emerald-500/20 text-emerald-300' : 'bg-rose-500/20 text-rose-300'">
                {{ selectedTrace.latency_ms || 0 }} ms
              </span>
            </div>
          </div>

          <!-- Metadata Section -->
          <div class="p-3.5 bg-slate-950 rounded-lg border border-slate-800 space-y-2">
            <div class="flex justify-between pb-1.5 border-b border-slate-900">
              <span class="text-slate-400">调用模型:</span>
              <span class="text-white font-bold">{{ selectedTrace.model || 'gpt-4o' }}</span>
            </div>
            <div class="flex justify-between pb-1.5 border-b border-slate-900">
              <span class="text-slate-400">路由上游:</span>
              <span class="text-indigo-300">{{ selectedTrace.provider || 'openai' }}</span>
            </div>
            <div class="flex justify-between pb-1.5 border-b border-slate-900">
              <span class="text-slate-400">客户端来源 IP:</span>
              <span class="text-slate-200">{{ selectedTrace.client_ip || '127.0.0.1' }}</span>
            </div>
            <div class="flex justify-between pb-1.5 border-b border-slate-900">
              <span class="text-slate-400">凭证 Key ID:</span>
              <span class="text-slate-200">#{{ selectedTrace.key_id || '1001' }}</span>
            </div>
            <div class="flex justify-between">
              <span class="text-slate-400">发生时间:</span>
              <span class="text-slate-200">{{ selectedTrace.time || '刚刚' }}</span>
            </div>
          </div>

          <!-- Token & Cost Accounting -->
          <div class="p-3.5 bg-slate-950 rounded-lg border border-slate-800 space-y-2">
            <div class="text-xs font-semibold text-purple-400 mb-1">🪙 Token 计量与费用核算</div>
            <div class="grid grid-cols-2 gap-2 text-[11px]">
              <div>输入 Tokens: <strong class="text-white">{{ selectedTrace.prompt_tokens || 0 }}</strong></div>
              <div>输出 Tokens: <strong class="text-white">{{ selectedTrace.completion_tokens || 0 }}</strong></div>
              <div>合计消耗: <strong class="text-cyan-400">{{ (selectedTrace.prompt_tokens || 0) + (selectedTrace.completion_tokens || 0) }}</strong></div>
              <div>预估成本: <strong class="text-emerald-400">${{ Number(selectedTrace.cost || 0).toFixed(6) }}</strong></div>
            </div>
          </div>

          <!-- Policy Evaluation Details -->
          <div v-if="selectedTrace.guardrail && selectedTrace.guardrail !== 'none'" class="p-3.5 bg-slate-950 rounded-lg border border-rose-500/30 space-y-1.5">
            <div class="text-xs font-semibold text-rose-400">🛡️ 安全防御拦截触发</div>
            <div class="text-slate-300">处置动作: <strong>{{ selectedTrace.guardrail }}</strong></div>
            <div v-if="selectedTrace.reason" class="text-slate-400 text-[10px]">
              规则原因: {{ selectedTrace.reason }}
            </div>
          </div>

          <!-- Copy Action -->
          <div class="flex justify-end pt-2">
            <n-button size="small" secondary type="info" @click="copyTraceJson">
              📋 复制 Trace JSON 诊断快照
            </n-button>
          </div>
        </div>
      </n-drawer-content>
    </n-drawer>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onBeforeUnmount } from 'vue';
import { NButton, NDrawer, NDrawerContent, useMessage } from 'naive-ui';
import { state } from '@/store/appStore';
import { api } from '@/api/client';

const message = useMessage();
const paused = ref(false);
const autoScroll = ref(true);
const sseConnected = ref(false);

const liveRequests = ref([]);
const liveAlerts = ref([]);
const waterfallContainerRef = ref(null);

const statusFilter = ref('all');
const selectedProvider = ref('all');

const traceDrawerVisible = ref(false);
const selectedTrace = ref(null);

let eventSource = null;
let fallbackTimer = null;
let statTimer = null;

// Realtime Sliding Metrics (5s window)
const currentQps = ref(0);
const p90Latency = ref(0);
const p99Latency = ref(0);
const avgLatency = ref(0);
const errorRate = ref(0);
const tokenRate = ref(0);
const blockedCount = ref(0);
const upstreamErrorCount = ref(0);

// Provider Health State Registry
const providerRegistry = ref({
  openai: { id: 'openai', name: 'OpenAI 官方', cbState: 'CLOSED', latency_ms: 38, totalReqs: 0, errCount: 0, statusText: '运行平稳' },
  anthropic: { id: 'anthropic', name: 'Anthropic Claude', cbState: 'CLOSED', latency_ms: 64, totalReqs: 0, errCount: 0, statusText: '运行平稳' },
  deepseek: { id: 'deepseek', name: 'DeepSeek R1', cbState: 'CLOSED', latency_ms: 120, totalReqs: 0, errCount: 0, statusText: '运行平稳' },
  cache: { id: 'cache', name: '本地智能缓存', cbState: 'CACHE', isCache: true, latency_ms: 1, totalReqs: 0, errCount: 0, statusText: '极速就绪' },
});

const providerHealthList = computed(() => {
  return Object.values(providerRegistry.value).map(p => {
    const rate = p.totalReqs > 0 ? (((p.totalReqs - p.errCount) / p.totalReqs) * 100).toFixed(1) : '100.0';
    return {
      ...p,
      successRate: rate,
    };
  });
});

function toggleProviderFilter(provId) {
  if (selectedProvider.value === provId) {
    selectedProvider.value = 'all';
  } else {
    selectedProvider.value = provId;
  }
}

// Filtered Requests Pipeline
const filteredRequests = computed(() => {
  return liveRequests.value.filter(req => {
    if (selectedProvider.value !== 'all') {
      const p = (req.provider || '').toLowerCase();
      if (!p.includes(selectedProvider.value.toLowerCase())) return false;
    }
    if (statusFilter.value === '2xx') {
      if (req.status >= 400) return false;
    } else if (statusFilter.value === 'err') {
      if (req.status < 400) return false;
    } else if (statusFilter.value === 'cache') {
      if (!req.cached && req.provider !== 'cache') return false;
    }
    return true;
  });
});

function getLatencyColor(lat) {
  if (lat < 300) return 'text-emerald-400';
  if (lat < 1000) return 'text-amber-400';
  return 'text-rose-400 font-bold';
}

function computeSlidingMetrics() {
  const now = Date.now();
  // Filter events within last 5 seconds
  const recent = liveRequests.value.filter(r => (now - (r.timestamp || now)) <= 5000);
  const count = recent.length;

  currentQps.value = count / 5.0;

  if (count > 0) {
    const latencies = recent.map(r => r.latency_ms || 0).sort((a, b) => a - b);
    avgLatency.value = Math.round(latencies.reduce((a, b) => a + b, 0) / count);
    p90Latency.value = latencies[Math.floor(count * 0.9)] || latencies[count - 1];
    p99Latency.value = latencies[Math.floor(count * 0.99)] || latencies[count - 1];

    let errs = 0;
    let tokens = 0;
    let blocked = 0;
    let upErrors = 0;

    recent.forEach(r => {
      const isErr = (r.status >= 400);
      if (isErr) {
        errs++;
        if (r.status === 403) blocked++;
        else upErrors++;
      }
      tokens += ((r.prompt_tokens || 0) + (r.completion_tokens || 0));
    });

    errorRate.value = (errs / count) * 100.0;
    tokenRate.value = Math.round(tokens / 5.0);
    blockedCount.value = blocked;
    upstreamErrorCount.value = upErrors;
  } else {
    currentQps.value = 0;
    avgLatency.value = 0;
    p90Latency.value = 0;
    p99Latency.value = 0;
    errorRate.value = 0;
    tokenRate.value = 0;
  }
}

function processIncomingEvent(data) {
  const nowStr = new Date().toLocaleTimeString();
  data.time = nowStr;
  data.timestamp = Date.now();

  // Route by event type
  if (data.type === 'circuit_breaker') {
    liveAlerts.value.unshift(data);
    if (liveAlerts.value.length > 50) liveAlerts.value.pop();

    const provKey = (data.provider || '').toLowerCase();
    for (const k in providerRegistry.value) {
      if (provKey.includes(k)) {
        providerRegistry.value[k].cbState = data.new_state || 'DEGRADED';
        providerRegistry.value[k].statusText = data.reason || 'SLA 熔断器状态机流转';
      }
    }
    return;
  }

  if (data.type === 'health_probe') {
    const provKey = (data.provider || '').toLowerCase();
    for (const k in providerRegistry.value) {
      if (provKey.includes(k)) {
        providerRegistry.value[k].latency_ms = data.latency_ms || 20;
        providerRegistry.value[k].statusText = `探活正常 (${data.latency_ms}ms)`;
      }
    }
    return;
  }

  // Request Event
  const isAlert = (data.type === 'alert' || data.status >= 400 || (data.guardrail && data.guardrail !== 'none' && data.guardrail !== 'pass'));
  if (isAlert) {
    liveAlerts.value.unshift(data);
    if (liveAlerts.value.length > 50) liveAlerts.value.pop();
  }

  // Update provider statistics
  const provKey = (data.provider || '').toLowerCase();
  for (const k in providerRegistry.value) {
    if (provKey.includes(k)) {
      providerRegistry.value[k].totalReqs++;
      if (data.status >= 400) providerRegistry.value[k].errCount++;
      if (data.latency_ms) providerRegistry.value[k].latency_ms = data.latency_ms;
    }
  }

  // Add to request waterfall
  liveRequests.value.unshift(data);
  if (liveRequests.value.length > 200) liveRequests.value.pop();
}

function connectSSE() {
  if (eventSource) {
    eventSource.close();
  }

  const tokenParam = state.adminToken ? `?token=${encodeURIComponent(state.adminToken)}` : '';
  const url = `/admin/v1/events${tokenParam}`;
  try {
    eventSource = new EventSource(url);
    eventSource.onopen = () => {
      sseConnected.value = true;
      if (fallbackTimer) {
        clearInterval(fallbackTimer);
        fallbackTimer = null;
      }
    };

    eventSource.onmessage = (e) => {
      if (paused.value) return;
      try {
        const data = JSON.parse(e.data);
        processIncomingEvent(data);
      } catch (err) {
        console.error('Parse event error', err);
      }
    };

    eventSource.onerror = () => {
      sseConnected.value = false;
      startFallbackPolling();
    };
  } catch (err) {
    console.error('SSE connect error', err);
    sseConnected.value = false;
    startFallbackPolling();
  }
}

function startFallbackPolling() {
  if (fallbackTimer) return;
  fallbackTimer = setInterval(async () => {
    if (paused.value || sseConnected.value) return;
    try {
      const res = await api.getJson('/admin/v1/audit/events?limit=15');
      if (res && Array.isArray(res.events)) {
        res.events.forEach(ev => {
          // Normalize audit event to live request format
          const req = {
            trace_id: ev.trace_id,
            model: ev.model || ev.requested_model,
            provider: ev.provider || ev.routed_model,
            client_ip: ev.client_ip,
            status: ev.http_status || 200,
            latency_ms: ev.total_latency_ms || 12,
            prompt_tokens: ev.prompt_tokens || 0,
            completion_tokens: ev.completion_tokens || 0,
            cost: (ev.prompt_tokens + ev.completion_tokens) * 0.000002,
            guardrail: ev.rule_tag || 'none',
            cached: ev.provider === 'cache',
          };
          // Avoid duplicate trace_ids if already in list
          if (!liveRequests.value.some(r => r.trace_id === req.trace_id)) {
            processIncomingEvent(req);
          }
        });
      }
    } catch (e) {
      console.warn('Fallback poll error', e);
    }
  }, 2500);
}

function togglePause() {
  paused.value = !paused.value;
  if (paused.value) {
    message.info('已暂停实时流量推送');
  } else {
    message.info('已恢复实时流量推送');
  }
}

function clearEvents() {
  liveRequests.value = [];
  liveAlerts.value = [];
  message.success('已清空实时流水与告警列表');
}

function openRequestTrace(req) {
  selectedTrace.value = req;
  traceDrawerVisible.value = true;
}

function copyTraceJson() {
  if (navigator.clipboard && selectedTrace.value) {
    navigator.clipboard.writeText(JSON.stringify(selectedTrace.value, null, 2));
    message.success('已复制 Trace JSON 诊断快照');
  }
}

onMounted(() => {
  connectSSE();
  statTimer = setInterval(computeSlidingMetrics, 1000);
});

onBeforeUnmount(() => {
  if (eventSource) {
    eventSource.close();
  }
  if (fallbackTimer) {
    clearInterval(fallbackTimer);
  }
  if (statTimer) {
    clearInterval(statTimer);
  }
});
</script>
