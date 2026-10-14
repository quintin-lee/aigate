<template>
  <div id="tab-audit" class="space-y-4">
    <!-- Header with Verification Trigger -->
    <div class="flex flex-wrap items-center justify-between gap-3">
      <div class="flex items-center space-x-3">
        <h2 class="text-base font-semibold text-white">安全合规审计与防篡改存证 (Compliance Audit & Hash Chain)</h2>
        <span class="text-xs px-2 py-0.5 rounded-full bg-slate-800 text-slate-400 border border-slate-700">
          共 {{ auditEvents.length }} 条事件
        </span>
      </div>
      <div class="flex items-center space-x-2">
        <n-button
          size="small"
          type="info"
          secondary
          :loading="verifyingChain"
          @click="verifyHashChain"
        >
          🔐 校验哈希链完整性
        </n-button>
        <n-button size="small" secondary @click="toggleAuditLiveStream">
          {{ livePolling ? '⏸️ 暂停审计流' : '▶️ 实时审计流' }}
        </n-button>
        <n-button size="small" secondary @click="exportAuditNdjson">导出 NDJSON</n-button>
        <n-button size="small" secondary @click="loadEvents">刷新</n-button>
      </div>
    </div>

    <!-- Hash Chain Status Alert -->
    <div v-if="chainStatus" class="p-4 rounded-xl border bg-slate-900" :class="chainStatus.verified ? 'border-emerald-500/30' : 'border-rose-500/30'">
      <div class="flex items-center justify-between">
        <div class="flex items-center space-x-2">
          <span class="text-base">{{ chainStatus.verified ? '✅' : '❌' }}</span>
          <span class="font-semibold text-sm" :class="chainStatus.verified ? 'text-emerald-400' : 'text-rose-400'">
            {{ chainStatus.verified ? '哈希防篡改链校验通过 (Integrity Verified)' : '警告：检测到审计日志篡改或链断裂！' }}
          </span>
        </div>
        <span class="text-xs font-mono text-slate-400">已连续校验 {{ chainStatus.blocks_checked || 0 }} 个区块</span>
      </div>
      <div v-if="chainStatus.latest_hash" class="mt-2 text-[11px] font-mono text-slate-400 break-all">
        最新区块哈希 (Tip Hash): {{ chainStatus.latest_hash }}
      </div>
    </div>

    <!-- Audit Events Table -->
    <div class="glass-panel rounded-xl overflow-hidden">
      <n-data-table
        :columns="columns"
        :data="auditEvents"
        :loading="loading"
        :pagination="pagination"
      />
    </div>

    <!-- Forensic Drawer -->
    <n-drawer id="auditForensicDrawer" v-model:show="drawerVisible" width="560" placement="right">
      <n-drawer-content title="🔎 安全审计取证详情 (Forensic Inspection)" closable class="bg-slate-900 text-slate-100">
        <div v-if="selectedEvent" class="space-y-4 text-xs">
          <div class="p-3 bg-slate-950 rounded-lg border border-slate-800 space-y-2 font-mono">
            <div><strong class="text-slate-400">事件编号:</strong> {{ selectedEvent.id || 'N/A' }}</div>
            <div><strong class="text-slate-400">发生时间:</strong> {{ selectedEvent.created_at || 'N/A' }}</div>
            <div><strong class="text-slate-400">客户端 IP:</strong> {{ selectedEvent.client_ip || 'N/A' }}</div>
            <div><strong class="text-slate-400">触发规则:</strong> {{ selectedEvent.rule_name || selectedEvent.threat_type || 'N/A' }}</div>
            <div><strong class="text-slate-400">处置动作:</strong> {{ selectedEvent.action || 'block' }}</div>
          </div>

          <div>
            <h4 class="font-semibold text-slate-300 mb-1">请求关键元数据 (Request Payload Sample)</h4>
            <div class="p-3 bg-slate-950 rounded-lg border border-slate-800 font-mono text-[11px] text-slate-300 max-h-48 overflow-y-auto whitespace-pre-wrap">
              {{ selectedEvent.payload || selectedEvent.prompt || '无违规请求体留存' }}
            </div>
          </div>

          <div>
            <h4 class="font-semibold text-slate-300 mb-1">密码学取证存证摘要 (Cryptographic Proof)</h4>
            <div class="p-3 bg-slate-950 rounded-lg border border-cyan-500/20 font-mono text-[11px] text-cyan-300 break-all space-y-1">
              <div><strong>Block Hash:</strong> {{ selectedEvent.block_hash || 'SHA256:4f89d3...' }}</div>
              <div><strong>Previous Hash:</strong> {{ selectedEvent.prev_hash || 'GENESIS' }}</div>
            </div>
          </div>
        </div>
      </n-drawer-content>
    </n-drawer>
  </div>
</template>

<script setup>
import { ref, h, onMounted, reactive } from 'vue';
import {
  NButton,
  NDataTable,
  NDrawer,
  NDrawerContent,
  NTag,
  useMessage,
} from 'naive-ui';
import { api } from '@/api/client';

const message = useMessage();
const loading = ref(false);
const auditEvents = ref([]);
const verifyingChain = ref(false);
const chainStatus = ref(null);

const drawerVisible = ref(false);
const selectedEvent = ref(null);

const pagination = reactive({
  page: 1,
  pageSize: 15,
  showSizePicker: true,
  pageSizes: [15, 30, 50],
  onChange: (page) => { pagination.page = page; },
  onUpdatePageSize: (pageSize) => {
    pagination.pageSize = pageSize;
    pagination.page = 1;
  },
});

const columns = [
  { title: '时间', key: 'created_at', width: 170, ellipsis: { tooltip: true } },
  { title: '来源客户端 IP', key: 'client_ip', width: 140 },
  {
    title: '威胁 / 触发规则',
    key: 'rule_name',
    ellipsis: { tooltip: true },
    render(row) {
      return row.rule_name || row.threat_type || '安全策略触发';
    },
  },
  {
    title: '处置动作',
    key: 'action',
    width: 100,
    render(row) {
      const isBlock = row.action === 'block' || !row.action;
      return h(
        NTag,
        { size: 'small', type: isBlock ? 'error' : 'warning', bordered: false },
        { default: () => row.action || 'block' }
      );
    },
  },
  {
    title: '取证详情',
    key: 'actions',
    width: 100,
    render(row) {
      return h(
        NButton,
        {
          size: 'tiny',
          secondary: true,
          type: 'info',
          onClick: () => viewDetails(row),
        },
        { default: () => '查看取证' }
      );
    },
  },
];

async function loadEvents() {
  loading.value = true;
  try {
    const data = await api.getJson('/admin/v1/audit/violations');
    if (data && Array.isArray(data)) {
      auditEvents.value = data;
    }
  } finally {
    loading.value = false;
  }
}

async function verifyHashChain() {
  verifyingChain.value = true;
  try {
    const resp = await api.postJson('/admin/v1/audit/chain/verify');
    if (resp) {
      chainStatus.value = resp;
      if (resp.verified) {
        message.success(`哈希防篡改链校验完成：全部 ${resp.blocks_checked || 0} 个区块无异常`);
      } else {
        message.error('哈希链完整性校验未通过！');
      }
    } else {
      message.warning('网关暂无审计区块链数据');
    }
  } finally {
    verifyingChain.value = false;
  }
}

const livePolling = ref(false);
let liveTimer = null;

function toggleAuditLiveStream() {
  livePolling.value = !livePolling.value;
  if (livePolling.value) {
    message.info('已开启实时审计轮询');
    liveTimer = setInterval(loadEvents, 3000);
  } else {
    message.info('已暂停实时审计轮询');
    clearInterval(liveTimer);
    liveTimer = null;
  }
}

function exportAuditNdjson() {
  if (auditEvents.value.length === 0) {
    message.warning('当前暂无可导出的审计日志');
    return;
  }
  const ndjson = auditEvents.value.map(e => JSON.stringify(e)).join('\n');
  const blob = new Blob([ndjson], { type: 'application/x-ndjson' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `audit_${Date.now()}.ndjson`;
  a.click();
  URL.revokeObjectURL(url);
  message.success('已导出审计日志 NDJSON');
}

function viewDetails(row) {
  selectedEvent.value = row;
  drawerVisible.value = true;
}

onMounted(() => {
  loadEvents();
});
</script>
