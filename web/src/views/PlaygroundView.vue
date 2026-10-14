<template>
  <div id="tab-playground" class="space-y-4">
    <!-- Top Header Control Bar -->
    <div class="flex flex-wrap items-center justify-between gap-3 bg-slate-900/80 p-3.5 rounded-xl border border-slate-800">
      <div class="flex items-center space-x-3">
        <h2 class="text-sm font-semibold text-white flex items-center space-x-1.5">
          <span>🎮 大模型交互测试场 (Playground)</span>
        </h2>
        <n-radio-group v-model:value="playgroundMode" size="small">
          <n-radio-button value="single">💬 单模型调试</n-radio-button>
          <n-radio-button value="arena">⚔️ 双模型竞技场 (Dual Arena)</n-radio-button>
        </n-radio-group>
      </div>

      <div class="flex items-center space-x-2">
        <n-button size="tiny" secondary @click="copyCurl">📋 复制 cURL</n-button>
        <n-button size="tiny" secondary @click="clearHistory">清空对话</n-button>
      </div>
    </div>

    <!-- Mode 1: Single Model View -->
    <div v-if="playgroundMode === 'single'" class="grid grid-cols-1 lg:grid-cols-12 gap-6">
      <!-- Left Configuration Panel -->
      <div class="lg:col-span-4 glass-panel p-5 rounded-xl space-y-4">
        <div class="flex items-center justify-between pb-2 border-b border-slate-800">
          <h3 class="text-sm font-semibold text-white">模型参数调试 (Parameters)</h3>
          <span class="text-xs text-slate-500 font-mono">{{ selectedModel }}</span>
        </div>

        <div class="space-y-3">
          <div>
            <label class="text-xs text-slate-400 block mb-1">测试模型 (Model)</label>
            <n-select v-model:value="selectedModel" :options="modelOptions" placeholder="选择测试模型" />
          </div>

          <div>
            <label class="text-xs text-slate-400 block mb-1">System Prompt</label>
            <n-input
              v-model:value="systemPrompt"
              type="textarea"
              rows="3"
              placeholder="设定大模型全局人设..."
            />
          </div>

          <div class="grid grid-cols-2 gap-3">
            <div>
              <div class="flex justify-between text-xs text-slate-400 mb-1">
                <span>Temperature</span>
                <span>{{ temperature }}</span>
              </div>
              <n-slider v-model:value="temperature" :min="0" :max="2" :step="0.1" />
            </div>
            <div>
              <div class="flex justify-between text-xs text-slate-400 mb-1">
                <span>Top-P</span>
                <span>{{ topP }}</span>
              </div>
              <n-slider v-model:value="topP" :min="0" :max="1" :step="0.05" />
            </div>
          </div>

          <div>
            <label class="text-xs text-slate-400 block mb-1">思考推理力度 (Reasoning Effort)</label>
            <n-select v-model:value="reasoningEffort" :options="reasoningOptions" />
          </div>

          <div>
            <label class="text-xs text-slate-400 block mb-1">思考标签格式</label>
            <n-select id="playReasoningFormat" v-model:value="reasoningFormat" :options="formatOptions" size="small" />
          </div>

          <div id="playDegradedBanner" v-if="degradedState" class="p-2 bg-amber-500/10 border border-amber-500/30 rounded text-[11px] text-amber-300">
            ⚠️ 注意：检测到响应包含 X-Aigate-Reasoning-Degraded 标记，思考输出已降级
          </div>

          <div class="flex items-center justify-between pt-2">
            <span class="text-xs text-slate-400">启用 SSE 流式输出 (Stream)</span>
            <n-switch v-model:value="streamMode" />
          </div>
        </div>
      </div>

      <!-- Right Chat Area -->
      <div class="lg:col-span-8 glass-panel p-6 rounded-xl flex flex-col h-[700px]">
        <!-- Messages Container -->
        <div ref="chatContainer" class="flex-1 overflow-y-auto space-y-4 pr-2 custom-scroll mb-4">
          <div v-if="messages.length === 0" class="h-full flex items-center justify-center text-slate-500 text-sm">
            暂无对话，在下方输入内容向大模型发送请求测试
          </div>

          <div
            v-for="(m, idx) in messages"
            :key="idx"
            :class="['flex flex-col', m.role === 'user' ? 'items-end' : 'items-start']"
          >
            <div class="text-[11px] text-slate-500 mb-1 font-mono">
              {{ m.role === 'user' ? '用户 (You)' : `网关代理 (${selectedModel})` }}
            </div>

            <!-- Thinking block accordion with test DOM contract -->
            <div
              id="reasoning-box"
              v-if="m.reasoning"
              class="reasoning-accordion thinking-accordion max-w-[92%] mb-2 p-3 rounded-lg border border-cyan-500/30 bg-cyan-950/20 text-xs font-mono text-cyan-200 badge-purple transition-all"
            >
              <div
                class="flex items-center justify-between cursor-pointer select-none pb-1"
                @click="m.thinkingCollapsed = !m.thinkingCollapsed"
              >
                <div class="flex items-center space-x-1.5 text-cyan-400 font-semibold">
                  <span v-if="m.isThinking" class="animate-spin text-[10px]">⏳</span>
                  <span>🧠 {{ m.isThinking ? '深度思考推理中...' : '深度思考推理链 (Extended Thinking)' }}</span>
                  <span v-if="m.thinkingDuration" class="text-[11px] text-cyan-300 font-normal">
                    (耗时 {{ m.thinkingDuration }}s · {{ m.reasoningTokens || 0 }} tokens)
                  </span>
                </div>
                <span class="text-[10px] text-cyan-400 hover:text-cyan-200">
                  {{ m.thinkingCollapsed ? '展开 ▾' : '收起 ▴' }}
                </span>
              </div>
              <div v-show="!m.thinkingCollapsed" class="whitespace-pre-wrap leading-relaxed opacity-90 mt-1 border-t border-cyan-500/20 pt-1.5">
                {{ m.reasoning }}
              </div>
            </div>

            <!-- Message Body -->
            <div
              :class="[
                'p-3.5 rounded-xl text-xs max-w-[85%] whitespace-pre-wrap leading-relaxed relative group',
                m.role === 'user'
                  ? 'bg-brand-600 text-white shadow-sm'
                  : 'bg-slate-900 border border-slate-800 text-slate-200 shadow-sm'
              ]"
            >
              {{ m.content }}
              <button
                v-if="m.role === 'assistant' && m.content"
                class="absolute right-2 top-2 opacity-0 group-hover:opacity-100 text-[10px] bg-slate-800 hover:bg-slate-700 text-slate-300 px-1.5 py-0.5 rounded transition"
                @click="copyText(m.content)"
              >
                复制
              </button>
            </div>
          </div>

          <div v-if="isGenerating" class="flex items-center space-x-2 text-xs text-brand-400">
            <n-spin size="small" />
            <span>大模型正在推理思考并生成流式响应...</span>
          </div>
        </div>

        <!-- Execution Stats Bar -->
        <div v-if="lastStats" class="pb-2 text-[11px] font-mono text-slate-400 flex flex-wrap items-center gap-x-4 gap-y-1 border-t border-slate-800/80 pt-2">
          <span>⏱️ 总耗时: {{ lastStats.latency }}ms</span>
          <span v-if="lastStats.ttft">⚡ 首字时延 (TTFT): {{ lastStats.ttft }}ms</span>
          <span>🚀 产出速率: {{ lastStats.speed }} t/s</span>
          <span id="playStatReasoningTokens">🧠 推理 Token: {{ lastStats.reasoningTokens || 0 }} (aigate_tokens_reasoning_total)</span>
          <span v-if="lastStats.costFormatted" class="text-emerald-400 font-semibold">💰 预估费用: {{ lastStats.costFormatted }}</span>
        </div>

        <!-- Input Bar -->
        <div class="pt-2 border-t border-slate-800 flex items-end space-x-2">
          <n-input
            v-model:value="userInput"
            type="textarea"
            :rows="2"
            placeholder="输入您的提示词... (Ctrl + Enter 快捷发送)"
            @keydown="handleKeydown"
            class="flex-1"
          />
          <n-button
            v-if="!isGenerating"
            type="primary"
            :disabled="!userInput.trim()"
            @click="sendMessage"
            class="h-14"
          >
            发送请求
          </n-button>
          <n-button
            v-else
            type="warning"
            @click="abortGeneration"
            class="h-14"
          >
            ⏹ 终止
          </n-button>
        </div>
      </div>
    </div>

    <!-- Mode 2: Dual Arena View -->
    <div v-else class="space-y-4">
      <!-- Arena Dual Configuration Bar -->
      <div class="glass-panel p-4 rounded-xl space-y-3">
        <div class="grid grid-cols-1 md:grid-cols-2 gap-4 pb-2 border-b border-slate-800">
          <!-- Model A Config -->
          <div class="flex items-center space-x-2">
            <span class="w-3 h-3 rounded-full bg-cyan-400 inline-block"></span>
            <span class="text-xs font-semibold text-cyan-300">模型 A:</span>
            <n-select v-model:value="modelAConfig.model" :options="modelOptions" size="small" class="flex-1" />
            <n-select v-model:value="modelAConfig.reasoningEffort" :options="reasoningOptions" size="small" class="w-28" />
          </div>

          <!-- Model B Config -->
          <div class="flex items-center space-x-2">
            <span class="w-3 h-3 rounded-full bg-purple-400 inline-block"></span>
            <span class="text-xs font-semibold text-purple-300">模型 B:</span>
            <n-select v-model:value="modelBConfig.model" :options="modelOptions" size="small" class="flex-1" />
            <n-select v-model:value="modelBConfig.reasoningEffort" :options="reasoningOptions" size="small" class="w-28" />
          </div>
        </div>

        <!-- Shared System Prompt -->
        <div class="flex items-center space-x-2">
          <label class="text-xs text-slate-400 whitespace-nowrap">全局人设 (System):</label>
          <n-input v-model:value="systemPrompt" size="small" placeholder="为两个对标模型设定完全一致的全局人设..." class="flex-1" />
          <n-switch v-model:value="streamMode" size="small">
            <template #checked>SSE 流式</template>
            <template #unchecked>同步响应</template>
          </n-switch>
        </div>
      </div>

      <!-- Arena Chat Grid (2 Columns) -->
      <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
        <!-- Model A Column -->
        <div class="glass-panel p-4 rounded-xl flex flex-col h-[650px] border border-cyan-500/20">
          <div class="flex items-center justify-between pb-2 border-b border-slate-800 mb-2">
            <div class="flex items-center space-x-2">
              <span class="px-2 py-0.5 rounded bg-cyan-500/20 text-cyan-300 font-mono text-xs font-semibold">
                {{ modelAConfig.model || '未选择模型' }}
              </span>
              <span v-if="arenaWinner?.speed === 'A'" class="text-[10px] px-1.5 py-0.5 rounded bg-emerald-500/20 text-emerald-400 font-semibold">
                🚀 生成更快
              </span>
              <span v-if="arenaWinner?.cost === 'A'" class="text-[10px] px-1.5 py-0.5 rounded bg-amber-500/20 text-amber-300 font-semibold">
                💰 成本更低
              </span>
            </div>
            <span v-if="isGeneratingA" class="text-xs text-cyan-400 flex items-center space-x-1">
              <n-spin size="small" />
              <span>生成中...</span>
            </span>
          </div>

          <!-- Model A Messages -->
          <div ref="chatContainerA" class="flex-1 overflow-y-auto space-y-3 pr-2 custom-scroll mb-2">
            <div v-if="messagesA.length === 0" class="h-full flex items-center justify-center text-slate-500 text-xs">
              在底部发送提示词，向模型 A 与模型 B 同步对标测试
            </div>

            <div
              v-for="(m, idx) in messagesA"
              :key="idx"
              :class="['flex flex-col', m.role === 'user' ? 'items-end' : 'items-start']"
            >
              <div class="text-[10px] text-slate-500 mb-0.5 font-mono">
                {{ m.role === 'user' ? '用户 (Prompt)' : modelAConfig.model }}
              </div>

              <!-- Thinking block -->
              <div
                v-if="m.reasoning"
                class="reasoning-accordion thinking-accordion max-w-[95%] mb-1.5 p-2.5 rounded-lg border border-cyan-500/30 bg-cyan-950/20 text-xs font-mono text-cyan-200 badge-purple transition-all"
              >
                <div
                  class="flex items-center justify-between cursor-pointer select-none pb-1"
                  @click="m.thinkingCollapsed = !m.thinkingCollapsed"
                >
                  <div class="flex items-center space-x-1 text-cyan-400 font-semibold text-[11px]">
                    <span v-if="m.isThinking" class="animate-spin text-[10px]">⏳</span>
                    <span>🧠 {{ m.isThinking ? '思考推理中...' : '深度思考过程' }}</span>
                    <span v-if="m.thinkingDuration" class="text-[10px] text-cyan-300 font-normal">
                      ({{ m.thinkingDuration }}s · {{ m.reasoningTokens || 0 }}t)
                    </span>
                  </div>
                  <span class="text-[10px] text-cyan-400">{{ m.thinkingCollapsed ? '展开' : '收起' }}</span>
                </div>
                <div v-show="!m.thinkingCollapsed" class="whitespace-pre-wrap leading-relaxed opacity-90 border-t border-cyan-500/20 pt-1 text-[11px]">
                  {{ m.reasoning }}
                </div>
              </div>

              <!-- Content Body -->
              <div
                :class="[
                  'p-3 rounded-xl text-xs max-w-[90%] whitespace-pre-wrap leading-relaxed relative group',
                  m.role === 'user'
                    ? 'bg-cyan-900/60 text-cyan-100 border border-cyan-700/50'
                    : 'bg-slate-900 border border-slate-800 text-slate-200'
                ]"
              >
                {{ m.content }}
                <button
                  v-if="m.role === 'assistant' && m.content"
                  class="absolute right-1.5 top-1.5 opacity-0 group-hover:opacity-100 text-[10px] bg-slate-800 hover:bg-slate-700 text-slate-300 px-1 py-0.5 rounded"
                  @click="copyText(m.content)"
                >
                  复制
                </button>
              </div>
            </div>
          </div>

          <!-- Model A Stats -->
          <div v-if="statsA" class="p-2 bg-slate-950 rounded-lg border border-slate-800 text-[11px] font-mono flex flex-wrap gap-x-3 text-slate-400">
            <span>TTFT: <strong class="text-slate-200">{{ statsA.ttft || 0 }}ms</strong></span>
            <span>耗时: <strong class="text-slate-200">{{ statsA.latency }}ms</strong></span>
            <span>速率: <strong class="text-slate-200">{{ statsA.speed }} t/s</strong></span>
            <span>Token: {{ statsA.tokens || 0 }}</span>
            <span class="text-emerald-400">成本: {{ statsA.costFormatted }}</span>
          </div>
        </div>

        <!-- Model B Column -->
        <div class="glass-panel p-4 rounded-xl flex flex-col h-[650px] border border-purple-500/20">
          <div class="flex items-center justify-between pb-2 border-b border-slate-800 mb-2">
            <div class="flex items-center space-x-2">
              <span class="px-2 py-0.5 rounded bg-purple-500/20 text-purple-300 font-mono text-xs font-semibold">
                {{ modelBConfig.model || '未选择模型' }}
              </span>
              <span v-if="arenaWinner?.speed === 'B'" class="text-[10px] px-1.5 py-0.5 rounded bg-emerald-500/20 text-emerald-400 font-semibold">
                🚀 生成更快
              </span>
              <span v-if="arenaWinner?.cost === 'B'" class="text-[10px] px-1.5 py-0.5 rounded bg-amber-500/20 text-amber-300 font-semibold">
                💰 成本更低
              </span>
            </div>
            <span v-if="isGeneratingB" class="text-xs text-purple-400 flex items-center space-x-1">
              <n-spin size="small" />
              <span>生成中...</span>
            </span>
          </div>

          <!-- Model B Messages -->
          <div ref="chatContainerB" class="flex-1 overflow-y-auto space-y-3 pr-2 custom-scroll mb-2">
            <div v-if="messagesB.length === 0" class="h-full flex items-center justify-center text-slate-500 text-xs">
              在底部发送提示词，向模型 A 与模型 B 同步对标测试
            </div>

            <div
              v-for="(m, idx) in messagesB"
              :key="idx"
              :class="['flex flex-col', m.role === 'user' ? 'items-end' : 'items-start']"
            >
              <div class="text-[10px] text-slate-500 mb-0.5 font-mono">
                {{ m.role === 'user' ? '用户 (Prompt)' : modelBConfig.model }}
              </div>

              <!-- Thinking block -->
              <div
                v-if="m.reasoning"
                class="reasoning-accordion thinking-accordion max-w-[95%] mb-1.5 p-2.5 rounded-lg border border-purple-500/30 bg-purple-950/20 text-xs font-mono text-purple-200 badge-purple transition-all"
              >
                <div
                  class="flex items-center justify-between cursor-pointer select-none pb-1"
                  @click="m.thinkingCollapsed = !m.thinkingCollapsed"
                >
                  <div class="flex items-center space-x-1 text-purple-400 font-semibold text-[11px]">
                    <span v-if="m.isThinking" class="animate-spin text-[10px]">⏳</span>
                    <span>🧠 {{ m.isThinking ? '思考推理中...' : '深度思考过程' }}</span>
                    <span v-if="m.thinkingDuration" class="text-[10px] text-purple-300 font-normal">
                      ({{ m.thinkingDuration }}s · {{ m.reasoningTokens || 0 }}t)
                    </span>
                  </div>
                  <span class="text-[10px] text-purple-400">{{ m.thinkingCollapsed ? '展开' : '收起' }}</span>
                </div>
                <div v-show="!m.thinkingCollapsed" class="whitespace-pre-wrap leading-relaxed opacity-90 border-t border-purple-500/20 pt-1 text-[11px]">
                  {{ m.reasoning }}
                </div>
              </div>

              <!-- Content Body -->
              <div
                :class="[
                  'p-3 rounded-xl text-xs max-w-[90%] whitespace-pre-wrap leading-relaxed relative group',
                  m.role === 'user'
                    ? 'bg-purple-900/60 text-purple-100 border border-purple-700/50'
                    : 'bg-slate-900 border border-slate-800 text-slate-200'
                ]"
              >
                {{ m.content }}
                <button
                  v-if="m.role === 'assistant' && m.content"
                  class="absolute right-1.5 top-1.5 opacity-0 group-hover:opacity-100 text-[10px] bg-slate-800 hover:bg-slate-700 text-slate-300 px-1 py-0.5 rounded"
                  @click="copyText(m.content)"
                >
                  复制
                </button>
              </div>
            </div>
          </div>

          <!-- Model B Stats -->
          <div v-if="statsB" class="p-2 bg-slate-950 rounded-lg border border-slate-800 text-[11px] font-mono flex flex-wrap gap-x-3 text-slate-400">
            <span>TTFT: <strong class="text-slate-200">{{ statsB.ttft || 0 }}ms</strong></span>
            <span>耗时: <strong class="text-slate-200">{{ statsB.latency }}ms</strong></span>
            <span>速率: <strong class="text-slate-200">{{ statsB.speed }} t/s</strong></span>
            <span>Token: {{ statsB.tokens || 0 }}</span>
            <span class="text-emerald-400">成本: {{ statsB.costFormatted }}</span>
          </div>
        </div>
      </div>

      <!-- Unified Arena Bottom Input Bar -->
      <div class="glass-panel p-3 rounded-xl flex items-end space-x-2">
        <n-input
          v-model:value="userInput"
          type="textarea"
          :rows="2"
          placeholder="输入提示词，同时向模型 A 与模型 B 发起并发流式请求... (Ctrl + Enter 快捷发送)"
          @keydown="handleKeydown"
          class="flex-1"
        />
        <n-button
          v-if="!isGeneratingA && !isGeneratingB"
          type="primary"
          :disabled="!userInput.trim()"
          @click="sendMessage"
          class="h-14 px-6"
        >
          ⚔️ 同步对标发送
        </n-button>
        <n-button
          v-else
          type="warning"
          @click="abortGeneration"
          class="h-14 px-6"
        >
          ⏹ 终止生成
        </n-button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted, nextTick, reactive, computed } from 'vue';
import {
  NButton,
  NInput,
  NSelect,
  NSlider,
  NSwitch,
  NSpin,
  NRadioGroup,
  NRadioButton,
  useMessage,
} from 'naive-ui';
import { api } from '@/api/client';
import { state } from '@/store/appStore';

const message = useMessage();
const chatContainer = ref(null);
const chatContainerA = ref(null);
const chatContainerB = ref(null);

const playgroundMode = ref('single'); // 'single' vs 'arena'
const modelOptions = ref([]);

// Single Mode state
const selectedModel = ref('');
const systemPrompt = ref('You are a helpful AI assistant.');
const temperature = ref(0.7);
const topP = ref(1.0);
const streamMode = ref(true);
const reasoningEffort = ref('medium');
const reasoningFormat = ref('raw');
const degradedState = ref(false);
const userInput = ref('');
const isGenerating = ref(false);
const messages = ref([]);
const lastStats = ref(null);

// Arena Mode state
const modelAConfig = reactive({
  model: '',
  temperature: 0.7,
  topP: 1.0,
  reasoningEffort: 'high',
});

const modelBConfig = reactive({
  model: '',
  temperature: 0.7,
  topP: 1.0,
  reasoningEffort: 'medium',
});

const isGeneratingA = ref(false);
const isGeneratingB = ref(false);
const messagesA = ref([]);
const messagesB = ref([]);
const statsA = ref(null);
const statsB = ref(null);

let abortCtrlSingle = null;
let abortCtrlA = null;
let abortCtrlB = null;

const formatOptions = [
  { label: '原始输出 (Raw Content)', value: 'raw' },
  { label: '思考标签包裹 (<think>...</think>)', value: 'think' },
];

const reasoningOptions = [
  { label: '关闭 (None)', value: 'none' },
  { label: '低度思考 (Low)', value: 'low' },
  { label: '中度思考 (Medium)', value: 'medium' },
  { label: '深度思考 (High)', value: 'high' },
];

// Standard pricing table ($ / 1M tokens)
const MODEL_PRICES = {
  'deepseek-chat': { prompt: 0.14, completion: 0.28, reasoning: 0.28 },
  'deepseek-v3': { prompt: 0.14, completion: 0.28, reasoning: 0.28 },
  'deepseek-r1': { prompt: 0.55, completion: 2.19, reasoning: 2.19 },
  'gpt-4o': { prompt: 2.50, completion: 10.00, reasoning: 10.00 },
  'gpt-4o-mini': { prompt: 0.15, completion: 0.60, reasoning: 0.60 },
  'claude-3-7-sonnet': { prompt: 3.00, completion: 15.00, reasoning: 15.00 },
  'claude-3-5-sonnet': { prompt: 3.00, completion: 15.00, reasoning: 15.00 },
  'claude-3-5-haiku': { prompt: 0.80, completion: 4.00, reasoning: 4.00 },
  'gemini-2.0-flash': { prompt: 0.10, completion: 0.40, reasoning: 0.40 },
  'default': { prompt: 1.00, completion: 3.00, reasoning: 3.00 },
};

function getModelPricing(modelName) {
  if (!modelName) return MODEL_PRICES.default;
  const lower = modelName.toLowerCase();
  for (const [key, price] of Object.entries(MODEL_PRICES)) {
    if (lower.includes(key)) return price;
  }
  return MODEL_PRICES.default;
}

function calculateCost(modelName, promptTokens, completionTokens, reasoningTokens) {
  const p = getModelPricing(modelName);
  const total =
    (promptTokens * p.prompt +
      completionTokens * p.completion +
      reasoningTokens * p.reasoning) /
    1000000;
  return total;
}

const arenaWinner = computed(() => {
  if (!statsA.value || !statsB.value) return null;
  const speedA = parseFloat(statsA.value.speed) || 0;
  const speedB = parseFloat(statsB.value.speed) || 0;
  const costA = statsA.value.rawCost || 0;
  const costB = statsB.value.rawCost || 0;

  return {
    speed: speedA > speedB ? 'A' : speedB > speedA ? 'B' : null,
    cost: costA < costB ? 'A' : costB < costA ? 'B' : null,
  };
});

async function loadModels() {
  try {
    const mList = await api.getJson('/admin/v1/models');
    if (mList && Array.isArray(mList)) {
      modelOptions.value = mList.map((m) => ({ label: m.name, value: m.name }));
      if (mList.length > 0) {
        if (!selectedModel.value) selectedModel.value = mList[0].name;
        if (!modelAConfig.model) modelAConfig.model = mList[0].name;
        if (!modelBConfig.model) modelBConfig.model = mList.length > 1 ? mList[1].name : mList[0].name;
      }
    }
  } catch {}
}

function handleKeydown(e) {
  if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
    e.preventDefault();
    sendMessage();
  }
}

function copyText(txt) {
  if (navigator.clipboard && txt) {
    navigator.clipboard.writeText(txt);
    message.success('已复制到剪贴板');
  }
}

function copyCurl() {
  const targetModel = playgroundMode.value === 'single' ? selectedModel.value : modelAConfig.model;
  const host = window.location.origin || 'http://localhost:8080';
  const curlCmd = `curl -X POST ${host}/v1/chat/completions \\
  -H "Content-Type: application/json" \\
  -H "Authorization: Bearer \${AIGATE_TOKEN}" \\
  -d '{
    "model": "${targetModel || 'gpt-4o'}",
    "messages": [
      {"role": "system", "content": "${systemPrompt.value.replace(/"/g, '\\"')}"},
      {"role": "user", "content": "${(userInput.value || 'Hello').replace(/"/g, '\\"')}"}
    ],
    "temperature": ${temperature.value},
    "stream": ${streamMode.value}
  }'`;
  copyText(curlCmd);
}

function abortGeneration() {
  if (abortCtrlSingle) abortCtrlSingle.abort();
  if (abortCtrlA) abortCtrlA.abort();
  if (abortCtrlB) abortCtrlB.abort();
  isGenerating.value = false;
  isGeneratingA.value = false;
  isGeneratingB.value = false;
  message.info('已终止大模型生成');
}

async function streamWorker({
  modelName,
  temp,
  tp,
  effort,
  messagesList,
  onStart,
  onDone,
  containerRef,
}) {
  const abortCtrl = new AbortController();
  onStart(abortCtrl);

  const reqMessages = [];
  if (systemPrompt.value) {
    reqMessages.push({ role: 'system', content: systemPrompt.value });
  }
  messagesList.forEach((m) => {
    reqMessages.push({ role: m.role, content: m.content });
  });

  const payload = {
    model: modelName,
    messages: reqMessages,
    temperature: temp,
    top_p: tp,
    stream: streamMode.value,
  };
  if (effort && effort !== 'none') {
    payload.reasoning_effort = effort;
  }

  const startTime = Date.now();
  let firstByteTime = null;
  let thinkingEndTime = null;

  const assistantMsg = reactive({
    role: 'assistant',
    content: '',
    reasoning: '',
    isThinking: false,
    thinkingCollapsed: false,
    thinkingDuration: null,
    reasoningTokens: 0,
  });
  messagesList.push(assistantMsg);

  try {
    const headers = { 'Content-Type': 'application/json' };
    if (state.adminToken) {
      headers['Authorization'] = `Bearer ${state.adminToken}`;
    }

    const resp = await fetch('/v1/chat/completions', {
      method: 'POST',
      headers,
      body: JSON.stringify(payload),
      signal: abortCtrl.signal,
    });

    if (resp.headers.get('X-Aigate-Reasoning-Degraded')) {
      degradedState.value = true;
    }

    if (!resp.ok) {
      const errText = await resp.text();
      assistantMsg.content = `[请求失败 ${resp.status}]: ${errText}`;
      return null;
    }

    if (!streamMode.value) {
      const json = await resp.json();
      firstByteTime = Date.now();
      assistantMsg.content = json.choices?.[0]?.message?.content || '';
      if (json.choices?.[0]?.message?.reasoning_content) {
        assistantMsg.reasoning = json.choices[0].message.reasoning_content;
        assistantMsg.reasoningTokens = Math.round(assistantMsg.reasoning.length / 3);
      }
      const dur = Date.now() - startTime;
      const compTokens = Math.round(assistantMsg.content.length / 3);
      const promptTokens = Math.round(JSON.stringify(reqMessages).length / 3);
      const rawCost = calculateCost(modelName, promptTokens, compTokens, assistantMsg.reasoningTokens);

      const stats = {
        latency: dur,
        ttft: dur,
        speed: 'N/A',
        tokens: compTokens + assistantMsg.reasoningTokens,
        reasoningTokens: assistantMsg.reasoningTokens,
        rawCost,
        costFormatted: rawCost < 0.0001 ? '<$0.0001' : `$${rawCost.toFixed(5)}`,
      };
      return stats;
    } else {
      const reader = resp.body.getReader();
      const decoder = new TextDecoder('utf-8');
      let buffer = '';
      let tokenCount = 0;
      let reasoningCount = 0;

      while (true) {
        const { done, value } = await reader.read();
        if (done) break;

        if (!firstByteTime) {
          firstByteTime = Date.now();
        }

        buffer += decoder.decode(value, { stream: true });
        const lines = buffer.split('\n');
        buffer = lines.pop() || '';

        for (const line of lines) {
          const trimmed = line.trim();
          if (!trimmed.startsWith('data:')) continue;
          const dataStr = trimmed.slice(5).trim();
          if (dataStr === '[DONE]') break;

          try {
            const parsed = JSON.parse(dataStr);
            const delta = parsed.choices?.[0]?.delta;
            if (delta) {
              if (delta.reasoning_content) {
                assistantMsg.isThinking = true;
                assistantMsg.reasoning += delta.reasoning_content;
                reasoningCount++;
              }
              if (delta.content) {
                if (assistantMsg.isThinking) {
                  assistantMsg.isThinking = false;
                  thinkingEndTime = Date.now();
                  assistantMsg.thinkingDuration = ((thinkingEndTime - firstByteTime) / 1000).toFixed(1);
                  assistantMsg.thinkingCollapsed = true; // Auto fold on content arrival
                }
                assistantMsg.content += delta.content;
                tokenCount++;
              }
            }
          } catch {}
        }
        scrollHelper(containerRef);
      }

      if (assistantMsg.isThinking) {
        assistantMsg.isThinking = false;
        thinkingEndTime = Date.now();
        assistantMsg.thinkingDuration = ((thinkingEndTime - firstByteTime) / 1000).toFixed(1);
      }
      assistantMsg.reasoningTokens = reasoningCount;

      const dur = Date.now() - startTime;
      const ttft = firstByteTime ? firstByteTime - startTime : dur;
      const speed = dur > 0 ? (((tokenCount + reasoningCount) / dur) * 1000).toFixed(1) : 0;
      const promptTokens = Math.round(JSON.stringify(reqMessages).length / 3);
      const rawCost = calculateCost(modelName, promptTokens, tokenCount, reasoningCount);

      const stats = {
        latency: dur,
        ttft,
        speed,
        tokens: tokenCount + reasoningCount,
        reasoningTokens: reasoningCount,
        rawCost,
        costFormatted: rawCost < 0.0001 ? '<$0.0001' : `$${rawCost.toFixed(5)}`,
      };
      return stats;
    }
  } catch (err) {
    if (err.name !== 'AbortError') {
      assistantMsg.content = `[网络异常]: ${err.message}`;
    }
    return null;
  } finally {
    onDone();
    scrollHelper(containerRef);
  }
}

async function sendMessage() {
  const text = userInput.value.trim();
  if (!text) return;

  userInput.value = '';

  if (playgroundMode.value === 'single') {
    if (!selectedModel.value) return;
    messages.value.push({ role: 'user', content: text });
    isGenerating.value = true;
    lastStats.value = null;
    scrollHelper(chatContainer);

    const stats = await streamWorker({
      modelName: selectedModel.value,
      temp: temperature.value,
      tp: topP.value,
      effort: reasoningEffort.value,
      messagesList: messages.value,
      onStart: (ctrl) => (abortCtrlSingle = ctrl),
      onDone: () => (isGenerating.value = false),
      containerRef: chatContainer,
    });
    if (stats) lastStats.value = stats;
  } else {
    // Dual Arena Mode: dispatch to Model A and Model B in parallel
    if (!modelAConfig.model || !modelBConfig.model) {
      message.warning('请选择竞技对标的两个大模型');
      return;
    }

    messagesA.value.push({ role: 'user', content: text });
    messagesB.value.push({ role: 'user', content: text });
    isGeneratingA.value = true;
    isGeneratingB.value = true;
    statsA.value = null;
    statsB.value = null;

    scrollHelper(chatContainerA);
    scrollHelper(chatContainerB);

    const taskA = streamWorker({
      modelName: modelAConfig.model,
      temp: modelAConfig.temperature,
      tp: modelAConfig.topP,
      effort: modelAConfig.reasoningEffort,
      messagesList: messagesA.value,
      onStart: (ctrl) => (abortCtrlA = ctrl),
      onDone: () => (isGeneratingA.value = false),
      containerRef: chatContainerA,
    });

    const taskB = streamWorker({
      modelName: modelBConfig.model,
      temp: modelBConfig.temperature,
      tp: modelBConfig.topP,
      effort: modelBConfig.reasoningEffort,
      messagesList: messagesB.value,
      onStart: (ctrl) => (abortCtrlB = ctrl),
      onDone: () => (isGeneratingB.value = false),
      containerRef: chatContainerB,
    });

    const [resA, resB] = await Promise.allSettled([taskA, taskB]);
    if (resA.status === 'fulfilled' && resA.value) statsA.value = resA.value;
    if (resB.status === 'fulfilled' && resB.value) statsB.value = resB.value;
  }
}

function clearHistory() {
  messages.value = [];
  messagesA.value = [];
  messagesB.value = [];
  lastStats.value = null;
  statsA.value = null;
  statsB.value = null;
}

function scrollHelper(containerRef) {
  nextTick(() => {
    if (containerRef && containerRef.value) {
      containerRef.value.scrollTop = containerRef.value.scrollHeight;
    }
  });
}

onMounted(() => {
  loadModels();
});
</script>
