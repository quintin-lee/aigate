<template>
  <div class="space-y-4">
    <div class="flex items-center justify-between">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">安全风控与内容防御 (Security & Guardrails)</h2>
      </div>
      <div class="flex items-center space-x-2">
        <n-button size="small" type="warning" secondary @click="reloadGuardrails">
          🔄 重载生效规则
        </n-button>
      </div>
    </div>

    <n-tabs type="line" animated class="custom-tabs">
      <!-- Tab 1: Rules -->
      <n-tab-pane name="rules" tab="🛡️ 敏感词与匹配规则">
        <div class="space-y-4 pt-2">
          <div class="flex justify-between items-center">
            <span class="text-xs text-slate-400">配置敏感词库、正则模式及动作（阻断 / 脱敏 / 告警）</span>
            <n-button size="small" type="primary" @click="openRuleModal()">+ 添加防御规则</n-button>
          </div>
          <div class="glass-panel rounded-xl overflow-hidden">
            <n-data-table :columns="ruleColumns" :data="rules" :loading="rulesLoading" />
          </div>
        </div>
      </n-tab-pane>

      <!-- Tab 2: PII Masking & Test -->
      <n-tab-pane name="pii" tab="🔒 PII 隐私脱敏测试">
        <div class="space-y-4 pt-2">
          <div class="grid grid-cols-1 lg:grid-cols-2 gap-6">
            <div class="glass-panel p-5 rounded-xl space-y-3">
              <h3 class="text-sm font-semibold text-white">PII 实时在线沙箱测试</h3>
              <p class="text-xs text-slate-400">输入含有手机号、身份证、邮箱、银行卡等隐私信息的文本进行实时脱敏预览</p>
              <n-input
                v-model:value="piiTestText"
                type="textarea"
                rows="5"
                placeholder="例如: 张三的联系电话是 13800138000，身份证号为 110101199003072391，邮箱 zhangsan@example.com"
              />
              <n-button type="primary" size="small" :loading="piiTesting" @click="runPiiTest">
                执行脱敏检测
              </n-button>
            </div>

            <div class="glass-panel p-5 rounded-xl space-y-3">
              <h3 class="text-sm font-semibold text-white">脱敏输出结果</h3>
              <div class="p-3 bg-slate-950 rounded-lg border border-slate-800 font-mono text-xs text-emerald-400 min-h-[120px] whitespace-pre-wrap">
                {{ piiResult || '等待执行脱敏检测...' }}
              </div>
            </div>
          </div>
        </div>
      </n-tab-pane>

      <!-- Tab 3: IP Bans & Whitelists -->
      <n-tab-pane name="bans" tab="🚫 IP 封禁与防御白名单">
        <div class="grid grid-cols-1 lg:grid-cols-2 gap-6 pt-2">
          <!-- IP Bans -->
          <div class="glass-panel p-5 rounded-xl space-y-4">
            <div class="flex justify-between items-center">
              <h3 class="text-sm font-semibold text-rose-400">动态 IP 封禁列表 (Bans)</h3>
              <n-button id="btn-ban-ip" size="tiny" type="error" @click="openAddBanModal">+ 添加封禁 IP</n-button>
            </div>
            <div id="ip-bans-table">
              <n-data-table :columns="banColumns" :data="bans" :loading="bansLoading" size="small" />
            </div>
          </div>

          <!-- Threat Whitelists -->
          <div class="glass-panel p-5 rounded-xl space-y-4">
            <div class="flex justify-between items-center">
              <h3 class="text-sm font-semibold text-emerald-400">高防白名单 (Whitelists)</h3>
              <n-button id="btn-add-threat-whitelist" size="tiny" type="primary" @click="openAddWhitelistModal">+ 添加白名单</n-button>
            </div>
            <div id="threat-whitelists-table">
              <n-data-table :columns="whitelistColumns" :data="whitelists" :loading="whitelistsLoading" size="small" />
            </div>
          </div>
        </div>
      </n-tab-pane>

      <!-- Tab 4: Watermark Forensic -->
      <n-tab-pane name="watermark" tab="🔍 隐式水印取证解码器">
        <div class="glass-panel p-6 rounded-xl space-y-4 pt-4 max-w-3xl">
          <div class="flex justify-between items-center">
            <h3 class="text-sm font-semibold text-white">零宽字符暗水印隐写取证 (Forensic Decoder)</h3>
            <div class="flex items-center space-x-2">
              <span id="chain-status-badge" class="text-xs px-2 py-0.5 rounded-full bg-emerald-500/10 text-emerald-400 border border-emerald-500/20">链状态正常</span>
              <n-button id="btn-verify-chain" size="tiny" type="info" secondary @click="reloadGuardrails">验证链</n-button>
            </div>
          </div>
          <p class="text-xs text-slate-400">
            aigate 可在 LLM 生成的内容中嵌入零宽字符隐式指纹。在此粘贴文本即可解析其来源密钥、请求哈希与生成时间戳。
          </p>
          <n-input
            id="watermark-input"
            v-model:value="watermarkInput"
            type="textarea"
            rows="4"
            placeholder="粘贴待溯源的大模型生成文本..."
          />
          <n-button id="btn-decode-watermark" type="primary" size="small" :loading="decodingWatermark" @click="decodeWatermark">
            解码溯源提取
          </n-button>

          <div id="watermark-result" v-if="watermarkResult" class="p-4 bg-slate-950 rounded-lg border border-cyan-500/30 space-y-2">
            <div class="text-xs font-semibold text-cyan-400">提取取证凭证成功：</div>
            <div class="font-mono text-xs text-slate-200 break-all space-y-1">
              <div><strong>用户/密钥指纹:</strong> {{ watermarkResult.key_id || 'N/A' }}</div>
              <div><strong>请求溯源码:</strong> {{ watermarkResult.request_id || 'N/A' }}</div>
              <div><strong>嵌入时间戳:</strong> {{ watermarkResult.timestamp || 'N/A' }}</div>
            </div>
          </div>
        </div>
      </n-tab-pane>
    </n-tabs>

    <!-- Rule Add/Edit Modal -->
    <n-modal
      v-model:show="ruleModalVisible"
      preset="card"
      :title="editingRuleId ? '编辑防御规则' : '添加防御规则'"
      class="max-w-md bg-slate-900 border border-slate-800 text-slate-100"
    >
      <n-form :model="ruleForm" label-placement="left" label-width="100" class="space-y-2">
        <n-form-item label="规则名称" required>
          <n-input v-model:value="ruleForm.name" placeholder="例如: 政治涉敏词过滤" />
        </n-form-item>
        <n-form-item label="匹配模式" required>
          <n-select v-model:value="ruleForm.match_type" :options="matchTypeOptions" />
        </n-form-item>
        <n-form-item label="匹配内容/正则" required>
          <n-input v-model:value="ruleForm.pattern" placeholder="词组用逗号隔开或输入正则表达式" />
        </n-form-item>
        <n-form-item label="触发动作" required>
          <n-select v-model:value="ruleForm.action" :options="actionOptions" />
        </n-form-item>
      </n-form>
      <template #footer>
        <div class="flex justify-end space-x-2">
          <n-button @click="ruleModalVisible = false">取消</n-button>
          <n-button type="primary" @click="saveRule">保存规则</n-button>
        </div>
      </template>
    </n-modal>
  </div>
</template>

<script setup>
import { ref, h, onMounted, reactive } from 'vue';
import {
  NButton,
  NDataTable,
  NTabs,
  NTabPane,
  NModal,
  NForm,
  NFormItem,
  NInput,
  NSelect,
  NTag,
  NPopconfirm,
  useMessage,
} from 'naive-ui';
import { api } from '@/api/client';

const message = useMessage();
const rules = ref([]);
const rulesLoading = ref(false);
const ruleModalVisible = ref(false);
const editingRuleId = ref(null);

const piiTestText = ref('');
const piiResult = ref('');
const piiTesting = ref(false);

const bans = ref([]);
const bansLoading = ref(false);
const whitelists = ref([]);
const whitelistsLoading = ref(false);

const watermarkInput = ref('');
const watermarkResult = ref(null);
const decodingWatermark = ref(false);

const matchTypeOptions = [
  { label: '关键词包含 (Keyword)', value: 'keyword' },
  { label: '正则表达式 (Regex)', value: 'regex' },
  { label: '向量相似度 (Semantic)', value: 'semantic' },
];

const actionOptions = [
  { label: '直接熔断阻断 (Block 403)', value: 'block' },
  { label: '自动替换星号掩码 (Mask)', value: 'mask' },
  { label: '静默记录审计日志 (Audit)', value: 'audit' },
];

const ruleForm = reactive({
  name: '',
  match_type: 'keyword',
  pattern: '',
  action: 'block',
});

const ruleColumns = [
  { title: '规则名称', key: 'name', width: 160 },
  {
    title: '匹配类型',
    key: 'match_type',
    width: 120,
    render(row) {
      return h(NTag, { size: 'small', type: 'info', bordered: false }, { default: () => row.match_type });
    },
  },
  { title: '规则模式 / 关键词', key: 'pattern', ellipsis: { tooltip: true } },
  {
    title: '防御动作',
    key: 'action',
    width: 120,
    render(row) {
      const type = row.action === 'block' ? 'error' : row.action === 'mask' ? 'warning' : 'default';
      return h(NTag, { size: 'small', type, bordered: false }, { default: () => row.action });
    },
  },
  {
    title: '操作',
    key: 'actions',
    width: 110,
    render(row) {
      return h(
        NPopconfirm,
        { onPositiveClick: () => deleteRule(row.id) },
        {
          trigger: () => h(NButton, { size: 'tiny', type: 'error', secondary: true }, { default: () => '删除' }),
          default: () => '确定删除此安全规则吗？',
        }
      );
    },
  },
];

const banColumns = [
  { title: '封禁 IP', key: 'ip' },
  { title: '封禁原因', key: 'reason', ellipsis: { tooltip: true } },
  {
    title: '操作',
    key: 'actions',
    width: 80,
    render(row) {
      return h(NButton, { size: 'tiny', secondary: true, type: 'warning', onClick: () => unbanIp(row.ip) }, { default: () => '解封' });
    },
  },
];

const whitelistColumns = [
  { title: '白名单 IP', key: 'ip' },
  { title: '备注', key: 'comment', ellipsis: { tooltip: true } },
  {
    title: '操作',
    key: 'actions',
    width: 80,
    render(row) {
      return h(NButton, { size: 'tiny', secondary: true, type: 'error', onClick: () => deleteWhitelist(row.ip) }, { default: () => '移除' });
    },
  },
];

async function loadRules() {
  rulesLoading.value = true;
  try {
    const data = await api.getJson('/admin/v1/guardrails');
    if (data && Array.isArray(data)) rules.value = data;
  } finally {
    rulesLoading.value = false;
  }
}

async function loadBans() {
  bansLoading.value = true;
  try {
    const data = await api.getJson('/admin/v1/bans');
    if (data && Array.isArray(data)) bans.value = data;
  } finally {
    bansLoading.value = false;
  }
}

async function loadWhitelists() {
  whitelistsLoading.value = true;
  try {
    const data = await api.getJson('/admin/v1/threat-whitelists');
    if (data && Array.isArray(data)) whitelists.value = data;
  } finally {
    whitelistsLoading.value = false;
  }
}

function openRuleModal(row = null) {
  if (row) {
    editingRuleId.value = row.id;
    ruleForm.name = row.name;
    ruleForm.match_type = row.match_type;
    ruleForm.pattern = row.pattern;
    ruleForm.action = row.action;
  } else {
    editingRuleId.value = null;
    ruleForm.name = '';
    ruleForm.match_type = 'keyword';
    ruleForm.pattern = '';
    ruleForm.action = 'block';
  }
  ruleModalVisible.value = true;
}

async function saveRule() {
  if (!ruleForm.name || !ruleForm.pattern) {
    message.warning('请填写规则名称与模式');
    return;
  }
  const resp = await api.post('/admin/v1/guardrails', ruleForm);
  if (resp && resp.ok) {
    message.success('规则保存成功');
    ruleModalVisible.value = false;
    loadRules();
  } else {
    message.error('保存失败');
  }
}

async function deleteRule(id) {
  const resp = await api.delete(`/admin/v1/guardrails/${id}`);
  if (resp && resp.ok) {
    message.success('规则已删除');
    loadRules();
  }
}

async function reloadGuardrails() {
  const resp = await api.post('/admin/v1/guardrails/reload');
  if (resp && resp.ok) {
    message.success('安全引擎已重载最新规则');
  } else {
    message.error('重载失败');
  }
}

async function runPiiTest() {
  if (!piiTestText.value) return;
  piiTesting.value = true;
  try {
    const resp = await api.postJson('/admin/v1/guardrails/pii/test', { text: piiTestText.value });
    if (resp && resp.masked_text) {
      piiResult.value = resp.masked_text;
    } else {
      piiResult.value = JSON.stringify(resp, null, 2);
    }
  } finally {
    piiTesting.value = false;
  }
}

async function decodeWatermark() {
  if (!watermarkInput.value) return;
  decodingWatermark.value = true;
  try {
    const resp = await api.postJson('/admin/v1/watermark/decode', { text: watermarkInput.value });
    if (resp && resp.success) {
      watermarkResult.value = resp;
      message.success('隐式指纹解码成功');
    } else {
      message.warning('未在文本中提取到有效指纹');
    }
  } finally {
    decodingWatermark.value = false;
  }
}

async function unbanIp(ip) {
  const resp = await api.delete(`/admin/v1/bans/${ip}`);
  if (resp && resp.ok) {
    message.success(`已解封 IP ${ip}`);
    loadBans();
  }
}

async function deleteWhitelist(ip) {
  const resp = await api.delete(`/admin/v1/threat-whitelists/${ip}`);
  if (resp && resp.ok) {
    message.success(`已移除白名单 IP ${ip}`);
    loadWhitelists();
  }
}

onMounted(() => {
  loadRules();
  loadBans();
  loadWhitelists();
});
</script>
