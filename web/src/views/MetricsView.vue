<template>
  <div id="tab-metrics" class="space-y-4">
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">监控指标与链路追踪 (Metrics & Tracing)</h2>
      </div>
      <div class="flex items-center space-x-2">
        <n-button size="small" secondary @click="loadData">刷新指标</n-button>
      </div>
    </div>

    <n-tabs type="line" animated>
      <!-- Prometheus Metrics -->
      <n-tab-pane name="prometheus" tab="📊 Prometheus 原生指标 (/metrics)">
        <div class="glass-panel p-4 rounded-xl space-y-3 mt-3">
          <div class="flex justify-between items-center">
            <n-input v-model:value="filterKeyword" size="small" placeholder="在指标中过滤 (例如: aigate_tokens_total)..." class="max-w-md" clearable />
            <span class="text-xs text-slate-400 font-mono">共 {{ filteredLines.length }} 行指标数据</span>
          </div>
          <div class="p-3 bg-slate-950 rounded-lg border border-slate-800 font-mono text-xs text-slate-300 max-h-[550px] overflow-y-auto custom-scroll whitespace-pre leading-relaxed">
            {{ filteredText || '正在加载 /metrics 数据...' }}
          </div>
        </div>
      </n-tab-pane>

      <!-- Distributed Tracing Config -->
      <n-tab-pane name="tracing" tab="🕸️ 分布式链路追踪配置 (OpenTelemetry)">
        <div class="glass-panel p-6 rounded-xl space-y-4 mt-3 max-w-xl">
          <h3 class="text-sm font-semibold text-white">OpenTelemetry / Jaeger 追踪网关配置</h3>
          <n-form :model="traceConfig" label-placement="left" label-width="120" class="space-y-2">
            <n-form-item label="启用链路追踪">
              <n-switch v-model:value="traceConfig.enabled" />
            </n-form-item>
            <n-form-item label="采样率 (0.0 - 1.0)">
              <n-slider v-model:value="traceConfig.sample_rate" :min="0" :max="1" :step="0.05" />
            </n-form-item>
            <n-form-item label="OTLP 上报端点">
              <n-input v-model:value="traceConfig.endpoint" placeholder="例如: http://jaeger:4318/v1/traces" />
            </n-form-item>
          </n-form>
          <div class="flex justify-end">
            <n-button type="primary" size="small" :loading="savingTrace" @click="saveTraceConfig">
              保存追踪策略
            </n-button>
          </div>
        </div>
      </n-tab-pane>
    </n-tabs>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue';
import {
  NButton,
  NTabs,
  NTabPane,
  NInput,
  NForm,
  NFormItem,
  NSwitch,
  NSlider,
  useMessage,
} from 'naive-ui';
import { api } from '@/api/client';

const message = useMessage();
const rawMetrics = ref('');
const filterKeyword = ref('');
const savingTrace = ref(false);

const traceConfig = ref({
  enabled: false,
  sample_rate: 0.1,
  endpoint: 'http://localhost:4318/v1/traces',
});

const filteredLines = computed(() => {
  if (!rawMetrics.value) return [];
  const lines = rawMetrics.value.split('\n');
  if (!filterKeyword.value) return lines;
  const kw = filterKeyword.value.toLowerCase();
  return lines.filter(l => l.toLowerCase().includes(kw));
});

const filteredText = computed(() => {
  return filteredLines.value.join('\n');
});

async function loadData() {
  try {
    const text = await fetch('/metrics').then(r => r.ok ? r.text() : '');
    rawMetrics.value = text;
  } catch (err) {
    rawMetrics.value = `# 获取 /metrics 失败: ${err.message}`;
  }

  const tc = await api.getJson('/admin/v1/traces/config');
  if (tc) {
    traceConfig.value = { ...traceConfig.value, ...tc };
  }
}

async function saveTraceConfig() {
  savingTrace.value = true;
  try {
    const resp = await api.post('/admin/v1/traces/config', traceConfig.value);
    if (resp && resp.ok) {
      message.success('链路追踪配置已更新');
    } else {
      message.error('保存链路配置失败');
    }
  } finally {
    savingTrace.value = false;
  }
}

onMounted(() => {
  loadData();
});
</script>
