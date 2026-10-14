<template>
  <div class="space-y-4">
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">Prompt 提示词模板管理 (Prompts & Variables)</h2>
        <span class="text-xs px-2 py-0.5 rounded-full bg-slate-800 text-slate-400 border border-slate-700">
          共 {{ presets.length }} 个模板
        </span>
      </div>
      <div class="flex items-center space-x-2">
        <n-button size="small" type="primary" @click="createPreset">+ 新建模板</n-button>
      </div>
    </div>

    <div class="grid grid-cols-1 lg:grid-cols-12 gap-6">
      <!-- Left list -->
      <div class="lg:col-span-4 glass-panel p-4 rounded-xl space-y-3">
        <n-input v-model:value="searchKeyword" size="small" placeholder="搜索提示词模板..." clearable />
        <div class="space-y-1.5 max-h-[600px] overflow-y-auto custom-scroll pr-1">
          <div
            v-for="p in filteredPresets"
            :key="p.id"
            @click="selectPreset(p)"
            :class="[
              'p-3 rounded-lg border cursor-pointer transition text-left',
              selectedId === p.id
                ? 'bg-brand-600/20 border-brand-500/50 text-white'
                : 'bg-slate-900/60 border-slate-800/80 text-slate-300 hover:bg-slate-800/50'
            ]"
          >
            <div class="flex justify-between items-center mb-1">
              <span class="font-medium text-xs truncate">{{ p.name }}</span>
              <n-tag size="tiny" type="info" :bordered="false">{{ p.category || '通用' }}</n-tag>
            </div>
            <p class="text-[11px] text-slate-400 line-clamp-2 leading-relaxed">{{ p.description || p.user_prompt }}</p>
          </div>
        </div>
      </div>

      <!-- Right workspace -->
      <div class="lg:col-span-8 glass-panel p-6 rounded-xl space-y-4">
        <div v-if="selectedPreset" class="space-y-4">
          <div class="flex justify-between items-center pb-2 border-b border-slate-800">
            <div class="flex items-center space-x-2 flex-1 mr-4">
              <n-input v-model:value="selectedPreset.name" placeholder="模板名称" />
              <n-input v-model:value="selectedPreset.category" placeholder="分类" class="max-w-[120px]" />
            </div>
            <div class="flex space-x-2">
              <n-button size="small" type="primary" @click="savePresets">保存模板</n-button>
              <n-button size="small" type="error" secondary @click="deletePreset">删除</n-button>
            </div>
          </div>

          <div class="space-y-3">
            <div>
              <label class="text-xs font-semibold text-slate-400 mb-1 block">System Prompt (系统人设设定)</label>
              <n-input
                v-model:value="selectedPreset.system_prompt"
                type="textarea"
                rows="3"
                placeholder="设定模型的角色背景与回答规则..."
              />
            </div>

            <div>
              <div class="flex justify-between items-center mb-1">
                <label class="text-xs font-semibold text-slate-400">User Prompt 模板 (支持 &#123;&#123;变量名&#125;&#125; 动态插槽)</label>
                <span class="text-[11px] text-brand-400 font-mono">
                  已解析 {{ detectedVars.length }} 个插槽
                </span>
              </div>
              <n-input
                v-model:value="selectedPreset.user_prompt"
                type="textarea"
                rows="5"
                placeholder="例如: 请将以下文本翻译为 {{target_lang}}: {{content}}"
              />
            </div>

            <!-- Dynamic variable inputs -->
            <div v-if="detectedVars.length > 0" class="p-4 bg-slate-950 rounded-lg border border-slate-800 space-y-2">
              <span class="text-xs font-semibold text-slate-300">动态变量测试赋值</span>
              <div class="grid grid-cols-1 sm:grid-cols-2 gap-3">
                <div v-for="v in detectedVars" :key="v" class="space-y-1">
                  <span class="text-[11px] font-mono text-cyan-400">&#123;&#123;{{ v }}&#125;&#125;</span>
                  <n-input v-model:value="varValues[v]" size="small" :placeholder="`输入 ${v} 的值`" />
                </div>
              </div>
            </div>

            <!-- Live Preview -->
            <div>
              <label class="text-xs font-semibold text-slate-400 mb-1 block">实时替换预览 (Rendered Output)</label>
              <div class="p-3 bg-slate-950 rounded-lg border border-slate-800 font-mono text-xs text-slate-200 whitespace-pre-wrap min-h-[70px]">
                {{ renderedPrompt }}
              </div>
            </div>
          </div>
        </div>

        <div v-else class="text-center py-20 text-slate-500 text-sm">
          请在左侧选择或新建一个 Prompt 提示词模板
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue';
import { NButton, NInput, NTag, useMessage } from 'naive-ui';

const message = useMessage();
const searchKeyword = ref('');
const selectedId = ref(null);
const varValues = ref({});

const defaultPresets = [
  {
    id: 'p_translate',
    name: '多语言专业翻译专家',
    category: '翻译',
    description: '保持术语准确并支持自定义目标语种',
    system_prompt: '你是一位精通跨语言多学科术语翻译的专家，输出纯净译文，无需额外寒暄。',
    user_prompt: '请将以下文本翻译为 {{target_lang}}:\n\n{{text}}',
  },
  {
    id: 'p_code_review',
    name: '代码审计与重构审查',
    category: '编程',
    description: '深度分析代码逻辑缺陷、潜在死锁、内存泄露与安全隐患',
    system_prompt: '你是世界顶尖的代码安全与性能优化首席架构师。',
    user_prompt: '请严格审查以下 {{language}} 代码的内存安全与性能瓶颈:\n\n```{{language}}\n{{code}}\n```',
  },
];

const presets = ref([]);

const filteredPresets = computed(() => {
  if (!searchKeyword.value) return presets.value;
  const kw = searchKeyword.value.toLowerCase();
  return presets.value.filter(p => p.name.toLowerCase().includes(kw) || (p.category && p.category.toLowerCase().includes(kw)));
});

const selectedPreset = computed(() => {
  return presets.value.find(p => p.id === selectedId.value);
});

const detectedVars = computed(() => {
  if (!selectedPreset.value || !selectedPreset.value.user_prompt) return [];
  const matches = selectedPreset.value.user_prompt.match(/\{\{([a-zA-Z0-9_-]+)\}\}/g);
  if (!matches) return [];
  return Array.from(new Set(matches.map(m => m.slice(2, -2))));
});

const renderedPrompt = computed(() => {
  if (!selectedPreset.value || !selectedPreset.value.user_prompt) return '';
  let txt = selectedPreset.value.user_prompt;
  detectedVars.value.forEach(v => {
    const val = varValues.value[v] || `{{${v}}}`;
    txt = txt.replaceAll(`{{${v}}}`, val);
  });
  return txt;
});

function loadPresets() {
  try {
    const stored = localStorage.getItem('aigate_prompt_presets');
    if (stored) {
      presets.value = JSON.parse(stored);
    } else {
      presets.value = defaultPresets;
      savePresets();
    }
  } catch {
    presets.value = defaultPresets;
  }
  if (presets.value.length > 0 && !selectedId.value) {
    selectPreset(presets.value[0]);
  }
}

function selectPreset(p) {
  selectedId.value = p.id;
  varValues.value = {};
}

function createPreset() {
  const newId = `p_${Date.now()}`;
  const newP = {
    id: newId,
    name: '未命名 Prompt 模板',
    category: '通用',
    description: '',
    system_prompt: '',
    user_prompt: '输入提示词内容，使用 {{var}} 插入变量...',
  };
  presets.value.unshift(newP);
  selectPreset(newP);
}

function deletePreset() {
  presets.value = presets.value.filter(p => p.id !== selectedId.value);
  savePresets();
  if (presets.value.length > 0) {
    selectPreset(presets.value[0]);
  } else {
    selectedId.value = null;
  }
  message.success('模板已删除');
}

function savePresets() {
  localStorage.setItem('aigate_prompt_presets', JSON.stringify(presets.value));
  message.success('Prompt 模板库已保存');
}

onMounted(() => {
  loadPresets();
});
</script>
