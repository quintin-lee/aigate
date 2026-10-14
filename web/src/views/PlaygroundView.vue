<template>
  <div id="tab-playground" class="grid grid-cols-1 lg:grid-cols-12 gap-6">
    <!-- Left Configuration Panel -->
    <div class="lg:col-span-4 glass-panel p-5 rounded-xl space-y-4">
      <div class="flex items-center justify-between pb-2 border-b border-slate-800">
        <h3 class="text-sm font-semibold text-white">模型参数调试 (Parameters)</h3>
        <n-button size="tiny" secondary @click="clearHistory">清空对话</n-button>
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

          <!-- Thinking block if present -->
          <div
            id="reasoning-box"
            v-if="m.reasoning"
            class="reasoning-accordion thinking-accordion max-w-[85%] mb-2 p-3 rounded-lg border border-cyan-500/30 bg-cyan-950/20 text-xs font-mono text-cyan-200 badge-purple"
          >
            <div class="flex items-center space-x-1.5 text-cyan-400 font-semibold mb-1">
              <span>🧠 深度思考推理链:</span>
            </div>
            <div class="whitespace-pre-wrap leading-relaxed opacity-90">{{ m.reasoning }}</div>
          </div>

          <!-- Message Body -->
          <div
            :class="[
              'p-3.5 rounded-xl text-xs max-w-[85%] whitespace-pre-wrap leading-relaxed',
              m.role === 'user'
                ? 'bg-brand-600 text-white shadow-sm'
                : 'bg-slate-900 border border-slate-800 text-slate-200 shadow-sm'
            ]"
          >
            {{ m.content }}
          </div>
        </div>

        <div v-if="isGenerating" class="flex items-center space-x-2 text-xs text-brand-400">
          <n-spin size="small" />
          <span>大模型正在推理思考并生成流式响应...</span>
        </div>
      </div>

      <!-- Execution Stats bar -->
      <div v-if="lastStats" class="pb-2 text-[11px] font-mono text-slate-400 flex items-center space-x-4 border-t border-slate-800/80 pt-2">
        <span>⏱️ 响应耗时: {{ lastStats.latency }}ms</span>
        <span>⚡ 产出速率: {{ lastStats.speed }} t/s</span>
        <span id="playStatReasoningTokens">🧠 推理 Token: {{ lastStats.reasoningTokens || 0 }} (aigate_tokens_reasoning_total)</span>
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
          type="primary"
          :disabled="isGenerating || !userInput.trim()"
          @click="sendMessage"
          class="h-14"
        >
          发送请求
        </n-button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted, nextTick } from 'vue';
import {
  NButton,
  NInput,
  NSelect,
  NSlider,
  NSwitch,
  NSpin,
  useMessage,
} from 'naive-ui';
import { api } from '@/api/client';
import { state } from '@/store/appStore';

const message = useMessage();
const chatContainer = ref(null);

const modelOptions = ref([]);
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

async function loadModels() {
  const mList = await api.getJson('/admin/v1/models');
  if (mList && Array.isArray(mList)) {
    modelOptions.value = mList.map(m => ({ label: m.name, value: m.name }));
    if (mList.length > 0 && !selectedModel.value) {
      selectedModel.value = mList[0].name;
    }
  }
}

function handleKeydown(e) {
  if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
    e.preventDefault();
    sendMessage();
  }
}

async function sendMessage() {
  const text = userInput.value.trim();
  if (!text || !selectedModel.value) return;

  messages.value.push({ role: 'user', content: text });
  userInput.value = '';
  isGenerating.value = true;
  lastStats.value = null;

  scrollToBottom();

  const reqMessages = [];
  if (systemPrompt.value) {
    reqMessages.push({ role: 'system', content: systemPrompt.value });
  }
  messages.value.forEach(m => {
    reqMessages.push({ role: m.role, content: m.content });
  });

  const payload = {
    model: selectedModel.value,
    messages: reqMessages,
    temperature: temperature.value,
    top_p: topP.value,
    stream: streamMode.value,
  };
  if (reasoningEffort.value !== 'none') {
    payload.reasoning_effort = reasoningEffort.value;
  }

  const startTime = Date.now();
  const assistantMsg = { role: 'assistant', content: '', reasoning: '' };
  messages.value.push(assistantMsg);

  try {
    const headers = { 'Content-Type': 'application/json' };
    if (state.adminToken) {
      headers['Authorization'] = `Bearer ${state.adminToken}`;
    }

    const resp = await fetch('/v1/chat/completions', {
      method: 'POST',
      headers,
      body: JSON.stringify(payload),
    });

    if (!resp.ok) {
      const errText = await resp.text();
      assistantMsg.content = `[请求失败 ${resp.status}]: ${errText}`;
      isGenerating.value = false;
      return;
    }

    if (!streamMode.value) {
      const json = await resp.json();
      assistantMsg.content = json.choices?.[0]?.message?.content || '';
      if (json.choices?.[0]?.message?.reasoning_content) {
        assistantMsg.reasoning = json.choices[0].message.reasoning_content;
      }
      const dur = Date.now() - startTime;
      lastStats.value = { latency: dur, speed: 'N/A' };
    } else {
      const reader = resp.body.getReader();
      const decoder = new TextDecoder('utf-8');
      let buffer = '';
      let tokenCount = 0;

      while (true) {
        const { done, value } = await reader.read();
        if (done) break;

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
              if (delta.content) {
                assistantMsg.content += delta.content;
                tokenCount++;
              }
              if (delta.reasoning_content) {
                assistantMsg.reasoning += delta.reasoning_content;
              }
            }
          } catch {}
        }
        scrollToBottom();
      }

      const dur = Date.now() - startTime;
      const speed = dur > 0 ? ((tokenCount / dur) * 1000).toFixed(1) : 0;
      lastStats.value = { latency: dur, speed };
    }
  } catch (err) {
    assistantMsg.content = `[网络异常]: ${err.message}`;
  } finally {
    isGenerating.value = false;
    scrollToBottom();
  }
}

function clearHistory() {
  messages.value = [];
  lastStats.value = null;
}

function scrollToBottom() {
  nextTick(() => {
    if (chatContainer.value) {
      chatContainer.value.scrollTop = chatContainer.value.scrollHeight;
    }
  });
}

onMounted(() => {
  loadModels();
});
</script>
